/*
 * wlrd-bench.c — Stage 2: 連続キャプチャの FPS と damage（変化領域）を測定する
 *
 * 使い方:
 *   ./wlrd-bench [-t 秒] [-f] [-c] [-o 出力番号] [-l]
 *     -t 秒  測定時間（既定 10 秒）
 *     -f     copy_with_damage を使わず毎フレーム全面コピーする
 *     -c     カーソルを合成してキャプチャする
 *     -o n   キャプチャする出力（モニタ）の番号。-l で一覧を確認する
 *     -l     出力の一覧（番号と接続名）を表示して終了する
 *
 * 出力:
 *   stdout: 1フレーム1行の TSV（gnuplot / awk でそのまま解析できる）
 *   stderr: 測定終了時のサマリ（FPS・damage率・帯域見積り）
 *
 * Stage 1 との差分（学習ポイント）:
 *   1. copy_with_damage (v2): コンポジタは画面に変化が起きるまで copy を
 *      保留し、完了時に "damage" イベントで変化した矩形群を教えてくれる。
 *      静止画面ではフレームが届かない＝転送すべきデータが無い、を意味する。
 *   2. poll(2) ベースのイベントループ: wl_display_dispatch は届いた
 *      イベントが無いと永久に待つ。静止画面でも測定時間で確実に切り上げる
 *      ため、wayland の fd を poll でタイムアウト付きで監視する。
 *      この形はStage 3 でソケット fd と多重化するときの土台になる。
 *   3. バッファの再利用: 解像度が変わらない限り同じ wl_buffer を
 *      使い回し、フレーム毎の mmap/確保コストを避ける。
 */
#define _GNU_SOURCE  /* memfd_create のため */
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1.h"

#define MAX_OUTPUTS 8

struct state {
	/* グローバル */
	struct wl_shm *shm;
	struct zwlr_screencopy_manager_v1 *screencopy;

	/* 見つかった出力の一覧と、キャプチャ対象に選んだ1つ。
	 * 名前は wl_output v4 の name イベント（DP-5 等の接続名）で埋まる */
	struct wl_output *outputs[MAX_OUTPUTS];
	char output_names[MAX_OUTPUTS][64];
	int n_outputs;
	struct wl_output *output;

	/* コンポジタが提示してきたバッファ仕様（フレーム毎に届く） */
	uint32_t format, width, height, stride;
	int have_buffer_info;

	/* 再利用する共有メモリバッファと、作成時の仕様（変化検出用） */
	struct wl_buffer *buffer;
	void *shm_data;
	size_t shm_size;
	uint32_t buf_width, buf_height;

	/* 動作モード */
	int full_copy;  /* 1: 毎フレーム全面コピー / 0: copy_with_damage */

	/* 現在のフレームの damage 集計（ready 前に damage イベントで加算） */
	uint32_t frame_rects;
	uint64_t frame_damage_px;  /* 矩形面積の総和。矩形同士の重なりは
	                              考慮しないため上振れしうる（近似値） */

	/* フレーム完了フラグ */
	int done;
	int failed;
};

/* CLOCK_MONOTONIC の現在時刻をナノ秒で返す（測定の基準時計） */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* --------------------------------------------------------------- wl_output */

/* wl_output のイベント。必要なのは name だけだが、リスナー構造体は
 * 全メンバを埋める必要がある（NULL のままだと未実装イベント受信で落ちる） */
static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t phys_w, int32_t phys_h,
                            int32_t subpixel, const char *make, const char *model,
                            int32_t transform)
{
	(void)data; (void)output; (void)x; (void)y; (void)phys_w; (void)phys_h;
	(void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
	(void)data; (void)output; (void)flags; (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
	(void)data; (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
	(void)data; (void)output; (void)factor;
}

/* v4: "DP-5" のような接続名。data には名前の格納先を直接渡してある */
static void output_name(void *data, struct wl_output *output, const char *name)
{
	(void)output;
	snprintf((char *)data, sizeof(((struct state *)0)->output_names[0]),
	         "%s", name);
}

static void output_description(void *data, struct wl_output *output,
                               const char *description)
{
	(void)data; (void)output; (void)description;
}

static const struct wl_output_listener output_listener = {
	.geometry    = output_geometry,
	.mode        = output_mode,
	.done        = output_done,
	.scale       = output_scale,
	.name        = output_name,
	.description = output_description,
};

/* ---------------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
	struct state *st = data;

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		if (st->n_outputs < MAX_OUTPUTS) {
			int i = st->n_outputs++;
			/* name イベントは v4 から。それ未満なら番号だけで表示する */
			uint32_t v = version < 4 ? version : 4;
			st->outputs[i] = wl_registry_bind(registry, name,
			                                  &wl_output_interface, v);
			snprintf(st->output_names[i], sizeof st->output_names[i],
			         "(output %d)", i);
			if (v >= 4)
				wl_output_add_listener(st->outputs[i], &output_listener,
				                       st->output_names[i]);
		}
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		/* copy_with_damage は v2、buffer_done は v3 で導入 */
		uint32_t v = version < 3 ? version : 3;
		st->screencopy = wl_registry_bind(registry, name,
		                                  &zwlr_screencopy_manager_v1_interface, v);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
	(void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global        = registry_global,
	.global_remove = registry_global_remove,
};

/* ------------------------------------------------------------- shm バッファ */

static struct wl_buffer *create_shm_buffer(struct state *st)
{
	st->shm_size = (size_t)st->stride * st->height;

	int fd = memfd_create("wlrd-bench", 0);
	if (fd < 0 || ftruncate(fd, st->shm_size) < 0) {
		perror("memfd_create/ftruncate");
		return NULL;
	}
	st->shm_data = mmap(NULL, st->shm_size, PROT_READ | PROT_WRITE,
	                    MAP_SHARED, fd, 0);
	if (st->shm_data == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, st->shm_size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, st->width, st->height, st->stride, st->format);
	wl_shm_pool_destroy(pool);
	close(fd);

	st->buf_width  = st->width;
	st->buf_height = st->height;
	return buffer;
}

/* ------------------------------------------------------- screencopy frame */

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride)
{
	(void)frame;
	struct state *st = data;

	int preferred = (format == WL_SHM_FORMAT_XRGB8888 ||
	                 format == WL_SHM_FORMAT_ARGB8888);
	if (!st->have_buffer_info || preferred) {
		st->format = format;
		st->width  = width;
		st->height = height;
		st->stride = stride;
		st->have_buffer_info = 1;
	}
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags)
{
	(void)data; (void)frame; (void)flags;  /* 測定ではピクセルを見ないため不要 */
}

static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width, uint32_t height)
{
	(void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct state *st = data;

	if (!st->have_buffer_info) {
		fprintf(stderr, "エラー: バッファ形式が提示されなかった\n");
		st->failed = 1;
		return;
	}

	/* 初回のみバッファを作成し、以後は使い回す。解像度変更（モニタの
	 * 切り替え等）が起きたら Stage 2 では単純化のため打ち切る */
	if (!st->buffer) {
		st->buffer = create_shm_buffer(st);
		if (!st->buffer) {
			st->failed = 1;
			return;
		}
	} else if (st->width != st->buf_width || st->height != st->buf_height) {
		fprintf(stderr, "エラー: 測定中に解像度が変化した\n");
		st->failed = 1;
		return;
	}

	if (st->full_copy)
		zwlr_screencopy_frame_v1_copy(frame, st->buffer);
	else
		zwlr_screencopy_frame_v1_copy_with_damage(frame, st->buffer);
}

/* copy_with_damage 使用時のみ、ready の直前に変化矩形が 0 個以上届く */
static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	(void)frame; (void)x; (void)y;
	struct state *st = data;
	st->frame_rects++;
	st->frame_damage_px += (uint64_t)w * h;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
	(void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
	struct state *st = data;
	st->done = 1;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	(void)frame;
	struct state *st = data;
	fprintf(stderr, "エラー: コンポジタがキャプチャを拒否した\n");
	st->failed = 1;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer       = frame_buffer,
	.flags        = frame_flags,
	.ready        = frame_ready,
	.failed       = frame_failed,
	.damage       = frame_damage,
	.linux_dmabuf = frame_linux_dmabuf,
	.buffer_done  = frame_buffer_done,
};

/* --------------------------------------------------------- イベントループ */

/* deadline_ns まで wayland イベントを処理する。
 * 戻り値: 1 = done/failed で抜けた, 0 = タイムアウト, -1 = エラー
 *
 * wl_display_dispatch を直接呼ぶと「イベントが届くまで」ブロックする。
 * 静止画面 + copy_with_damage では永久に届かない可能性があるため、
 * fd を poll してタイムアウトを自前で管理する。 */
static int dispatch_until(struct wl_display *display, struct state *st,
                          uint64_t deadline_ns)
{
	int fd = wl_display_get_fd(display);

	while (!st->done && !st->failed) {
		/* 溜まっている送信要求をソケットへ書き出してから待つ */
		wl_display_flush(display);

		uint64_t now = now_ns();
		if (now >= deadline_ns)
			return 0;
		int timeout_ms = (int)((deadline_ns - now) / 1000000ull) + 1;

		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		int n = poll(&pfd, 1, timeout_ms);
		if (n < 0) {
			perror("poll");
			return -1;
		}
		if (n == 0)
			return 0;  /* タイムアウト */

		/* POLLIN が立っているので dispatch はブロックせず読める */
		if (wl_display_dispatch(display) < 0) {
			perror("wl_display_dispatch");
			return -1;
		}
	}
	return 1;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	int duration_s = 10;
	int full_copy = 0;
	int overlay_cursor = 0;
	int out_idx = 0;
	int list_only = 0;

	int opt;
	while ((opt = getopt(argc, argv, "t:fco:lh")) != -1) {
		switch (opt) {
		case 't': duration_s = atoi(optarg); break;
		case 'f': full_copy = 1; break;
		case 'c': overlay_cursor = 1; break;
		case 'o': out_idx = atoi(optarg); break;
		case 'l': list_only = 1; break;
		default:
			fprintf(stderr, "使い方: %s [-t 秒] [-f 全面コピー] "
			        "[-c カーソル込み] [-o 出力番号] [-l 出力一覧]\n",
			        argv[0]);
			return 2;
		}
	}
	if (duration_s <= 0) {
		fprintf(stderr, "エラー: -t は正の秒数を指定する\n");
		return 2;
	}

	struct state st = {0};
	st.full_copy = full_copy;

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "エラー: コンポジタに接続できない\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &st);
	/* 1回目: global イベント（bind）。2回目: bind した wl_output の
	 * name イベントを受け取るため */
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);

	if (list_only) {
		for (int i = 0; i < st.n_outputs; i++)
			printf("%d\t%s\n", i, st.output_names[i]);
		return 0;
	}

	if (!st.shm || st.n_outputs == 0 || !st.screencopy) {
		fprintf(stderr, "エラー: 必要なプロトコルが不足している\n");
		return 1;
	}
	if (out_idx < 0 || out_idx >= st.n_outputs) {
		fprintf(stderr, "エラー: 出力番号 %d は範囲外（0〜%d。-l で一覧）\n",
		        out_idx, st.n_outputs - 1);
		return 2;
	}
	st.output = st.outputs[out_idx];

	fprintf(stderr, "測定開始: %d 秒, 出力=%s, モード=%s\n",
	        duration_s, st.output_names[out_idx],
	        full_copy ? "全面コピー" : "copy_with_damage");
	printf("# idx\tt_ms\tdt_ms\trects\tdmg_px\tdmg_pct\n");

	uint64_t t_start = now_ns();
	uint64_t deadline = t_start + (uint64_t)duration_s * 1000000000ull;
	uint64_t t_prev = 0;       /* 直前フレームの ready 受信時刻 */
	uint64_t frames = 0;
	uint64_t total_damage_px = 0;

	while (!st.failed) {
		/* 1フレーム分の要求を出す */
		st.done = 0;
		st.frame_rects = 0;
		st.frame_damage_px = 0;
		struct zwlr_screencopy_frame_v1 *frame =
			zwlr_screencopy_manager_v1_capture_output(
				st.screencopy, overlay_cursor, st.output);
		zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &st);

		int r = dispatch_until(display, &st, deadline);
		if (r <= 0 || st.failed) {
			/* タイムアウト・エラー時は保留中の要求ごと破棄して終了 */
			zwlr_screencopy_frame_v1_destroy(frame);
			if (r < 0)
				return 1;
			break;
		}

		/* フレーム完了。統計を1行出力する */
		uint64_t t = now_ns();
		double t_ms  = (t - t_start) / 1e6;
		double dt_ms = t_prev ? (t - t_prev) / 1e6 : 0.0;
		double pct = 100.0 * st.frame_damage_px
		             / ((double)st.buf_width * st.buf_height);
		printf("%llu\t%.2f\t%.2f\t%u\t%llu\t%.2f\n",
		       (unsigned long long)frames, t_ms, dt_ms,
		       st.frame_rects,
		       (unsigned long long)st.frame_damage_px, pct);

		t_prev = t;
		frames++;
		total_damage_px += st.frame_damage_px;
		zwlr_screencopy_frame_v1_destroy(frame);

		if (t >= deadline)
			break;
	}

	/* ---- サマリ（stderr へ。stdout の TSV と混ざらないように） ---- */
	double elapsed = (now_ns() - t_start) / 1e9;
	double fps = frames / elapsed;
	/* 転送帯域の見積り: 32bpp 生ピクセルをそのまま送った場合 */
	double full_mbps   = (double)st.buf_width * st.buf_height * 4 * fps / 1e6;
	double damage_mbps = (double)total_damage_px * 4 / elapsed / 1e6;

	fprintf(stderr,
	        "---- サマリ ----\n"
	        "解像度        : %ux%u\n"
	        "フレーム数    : %llu (%.1f 秒)\n"
	        "実効 FPS      : %.2f\n",
	        st.buf_width, st.buf_height,
	        (unsigned long long)frames, elapsed, fps);
	if (!full_copy && frames > 0) {
		fprintf(stderr,
		        "平均 damage   : %.2f %% / フレーム\n"
		        "帯域見積(生)  : 全画面 %.1f MB/s → damage のみ %.1f MB/s (%.1f 分の 1)\n",
		        100.0 * total_damage_px
		            / ((double)st.buf_width * st.buf_height * frames),
		        full_mbps, damage_mbps,
		        damage_mbps > 0 ? full_mbps / damage_mbps : 0.0);
	} else {
		fprintf(stderr, "帯域見積(生)  : %.1f MB/s\n", full_mbps);
	}

	if (st.buffer)
		wl_buffer_destroy(st.buffer);
	if (st.shm_data)
		munmap(st.shm_data, st.shm_size);
	wl_display_disconnect(display);
	return 0;
}

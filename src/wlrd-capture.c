/*
 * wlrd-capture.c — Stage 1: 画面の1フレームをキャプチャし PPM(P6) で stdout に出力する
 *
 * 使い方:
 *   ./wlrd-capture > frame.ppm
 *   ./wlrd-capture | magick ppm:- frame.png
 *
 * 仕組み（zwlr_screencopy_manager_v1 プロトコル）:
 *   1. コンポジタに接続し、registry から wl_shm / wl_output /
 *      zwlr_screencopy_manager_v1 を bind する
 *   2. capture_output でフレームキャプチャを要求する
 *   3. コンポジタが "buffer" イベントで受け入れ可能なバッファ形式
 *      （フォーマット・幅・高さ・stride）を提示してくる
 *   4. "buffer_done"（v3以降）を受けたら、共有メモリ (wl_shm) 上に
 *      その形式のバッファを作り "copy" で渡す
 *   5. コンポジタがフレームを書き込み終えると "ready" が届く
 *   6. ピクセルを RGB に並べ替えて PPM で書き出す
 *
 * Wayland はイベント駆動であり、クライアントは「要求を送る → イベントを
 * 待つ」を繰り返す。この非同期性が X11 の同期的な XGetImage との
 * 本質的な違いである。
 */
#define _GNU_SOURCE  /* memfd_create のため */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1.h"

/* アプリケーション全体の状態。コールバック間で共有するため1つの構造体に
 * まとめ、リスナーの data ポインタとして引き回す */
struct state {
	/* registry で見つけたグローバル */
	struct wl_shm *shm;
	struct wl_output *output;                       /* 最初に見つけた出力 */
	struct zwlr_screencopy_manager_v1 *screencopy;

	/* コンポジタが "buffer" イベントで提示してきたバッファ仕様 */
	uint32_t format;   /* wl_shm のピクセルフォーマット enum */
	uint32_t width, height, stride;
	int have_buffer_info;

	/* 共有メモリバッファ */
	struct wl_buffer *buffer;
	void *shm_data;    /* mmap したピクセル領域 */
	size_t shm_size;

	/* "flags" イベント: y_invert なら上下反転して書かれている */
	int y_invert;

	/* イベントループの終了条件 */
	int done;    /* ready を受信した */
	int failed;  /* failed を受信した */
};

/* ---------------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
	struct state *st = data;

	if (strcmp(interface, wl_shm_interface.name) == 0) {
		st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		/* 複数出力があっても最初の1つだけ使う（Stage 1 の割り切り） */
		if (!st->output)
			st->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
		/* buffer_done イベントを使うため v3 を要求する。
		 * 実機は v3 を提供していることを確認済み */
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

/* コンポジタと共有するメモリ上に wl_buffer を作る。
 * memfd_create は名前付きファイルを /tmp 等に残さず fd だけを得られる
 * Linux 固有の仕組みで、shm_open のリンク消し忘れ問題がない */
static struct wl_buffer *create_shm_buffer(struct state *st)
{
	st->shm_size = (size_t)st->stride * st->height;

	int fd = memfd_create("wlrd-capture", 0);
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

	/* pool: 共有メモリ領域全体。その中の区画を wl_buffer として切り出す。
	 * fd はコンポジタ側に複製されるので、作成後は閉じてよい */
	struct wl_shm_pool *pool = wl_shm_create_pool(st->shm, fd, st->shm_size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(
		pool, 0, st->width, st->height, st->stride, st->format);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

/* ------------------------------------------------------- screencopy frame */

/* コンポジタが「この形式の shm バッファなら受け取れる」と提示してくる。
 * 複数フォーマットが提示されることがあるため、扱いやすい
 * XRGB8888/ARGB8888 を優先して選ぶ */
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
	(void)frame;
	struct state *st = data;
	st->y_invert = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}

/* v3: dmabuf 形式の提示イベント。Stage 1 では shm のみ使うので無視する
 * （Stage 5 の zero-copy GPU エンコードでここが主役になる） */
static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width, uint32_t height)
{
	(void)data; (void)frame; (void)format; (void)width; (void)height;
}

/* v3: 全フォーマットの提示が終わった合図。ここでバッファを作って copy を
 * 要求する。v2 以前にはこのイベントがなく buffer 直後に copy していた */
static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame)
{
	struct state *st = data;

	if (!st->have_buffer_info) {
		fprintf(stderr, "エラー: バッファ形式が提示されなかった\n");
		st->failed = 1;
		return;
	}
	st->buffer = create_shm_buffer(st);
	if (!st->buffer) {
		st->failed = 1;
		return;
	}
	zwlr_screencopy_frame_v1_copy(frame, st->buffer);
}

/* コンポジタの書き込み完了。引数はフレームのタイムスタンプ（Stage 2 の
 * FPS 測定で使う予定。今は使わない） */
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

/* v2: 前回 copy したバッファが次フレームに再利用できなくなった通知。
 * 単発キャプチャでは関係ない */
static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	(void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
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

/* -------------------------------------------------------------- PPM 出力 */

/* ピクセルを RGB 3バイト列に並べ替えて PPM(P6) を書く。
 * wl_shm のフォーマット名はリトルエンディアンの 32bit 値として読んだ
 * ときの並びを指す。メモリ上のバイト順は逆になる点に注意:
 *   XRGB8888 (0xXXRRGGBB) → メモリ上 [B, G, R, X]
 *   XBGR8888 (0xXXBBGGRR) → メモリ上 [R, G, B, X]                       */
static int write_ppm(struct state *st, FILE *out)
{
	int bgr;  /* メモリ上の並びが B,G,R,X か */
	switch (st->format) {
	case WL_SHM_FORMAT_XRGB8888:
	case WL_SHM_FORMAT_ARGB8888:
		bgr = 1;
		break;
	case WL_SHM_FORMAT_XBGR8888:
	case WL_SHM_FORMAT_ABGR8888:
		bgr = 0;
		break;
	default:
		fprintf(stderr, "エラー: 未対応フォーマット 0x%08x\n", st->format);
		return -1;
	}

	fprintf(out, "P6\n%u %u\n255\n", st->width, st->height);

	/* 1行分の RGB を組み立ててから書く（fwrite 呼び出し回数の削減） */
	uint8_t *row = malloc((size_t)st->width * 3);
	if (!row)
		return -1;

	for (uint32_t y = 0; y < st->height; y++) {
		/* y_invert なら下の行から読む */
		uint32_t src_y = st->y_invert ? st->height - 1 - y : y;
		const uint8_t *src = (const uint8_t *)st->shm_data
		                     + (size_t)src_y * st->stride;
		for (uint32_t x = 0; x < st->width; x++) {
			const uint8_t *px = src + (size_t)x * 4;
			row[x * 3 + 0] = bgr ? px[2] : px[0];  /* R */
			row[x * 3 + 1] = px[1];                /* G */
			row[x * 3 + 2] = bgr ? px[0] : px[2];  /* B */
		}
		fwrite(row, 3, st->width, out);
	}
	free(row);
	return 0;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	struct state st = {0};

	/* PPM はバイナリなので端末への垂れ流しを防ぐ */
	if (isatty(STDOUT_FILENO)) {
		fprintf(stderr, "使い方: wlrd-capture > frame.ppm\n"
		                "        wlrd-capture | magick ppm:- frame.png\n");
		return 2;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "エラー: コンポジタに接続できない\n");
		return 1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &st);
	/* roundtrip 1回目: global イベントを受け取り bind する */
	wl_display_roundtrip(display);

	if (!st.shm || !st.output || !st.screencopy) {
		fprintf(stderr, "エラー: 必要なプロトコルが不足 (shm=%p output=%p screencopy=%p)\n",
		        (void *)st.shm, (void *)st.output, (void *)st.screencopy);
		return 1;
	}

	/* キャプチャ要求。overlay_cursor=0 でカーソルは含めない */
	struct zwlr_screencopy_frame_v1 *frame =
		zwlr_screencopy_manager_v1_capture_output(st.screencopy, 0, st.output);
	zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &st);

	/* ready か failed が届くまでイベントを処理し続ける */
	while (!st.done && !st.failed) {
		if (wl_display_dispatch(display) < 0) {
			perror("wl_display_dispatch");
			return 1;
		}
	}

	int ret = 1;
	if (st.done) {
		fprintf(stderr, "キャプチャ完了: %ux%u stride=%u format=0x%08x%s\n",
		        st.width, st.height, st.stride, st.format,
		        st.y_invert ? " (y-inverted)" : "");
		ret = write_ppm(&st, stdout) == 0 ? 0 : 1;
	}

	/* 後始末（プロセス終了で解放されるが作法として明示する） */
	zwlr_screencopy_frame_v1_destroy(frame);
	if (st.buffer)
		wl_buffer_destroy(st.buffer);
	if (st.shm_data)
		munmap(st.shm_data, st.shm_size);
	wl_display_disconnect(display);
	return ret;
}

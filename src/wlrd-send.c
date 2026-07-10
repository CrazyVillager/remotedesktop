/*
 * wlrd-send.c — Stage 3+4: 画面を stdout へ配信し、stdin から入力を受ける
 *
 * 使い方:
 *   ./wlrd-send [-o 出力番号] [-l] [-r 最大fps] [-C] [-f] [-I]
 *     -o n   キャプチャする出力（-l で一覧）
 *     -r fps フレームレート上限（既定 30）
 *     -C     カーソルを合成しない（既定は合成する）
 *     -f     copy_with_damage を使わず毎フレーム送る（帯域測定用）
 *     -I     入力注入を無効にする（見るだけ配信）
 *
 * トランスポート非依存（UNIX哲学）。フレームは stdout、入力は stdin:
 *   ./wlrd-view ./wlrd-send -o 0                 # ローカル（view が配管する）
 *   ./wlrd-view ssh host wlrd-send               # SSH 経由（暗号化・認証込み）
 *   ./wlrd-send | ./wlrd-view                    # 見るだけ（stdin が端末なら
 *                                                  入力は自動で無効）
 *
 * ストリーム形式・入力メッセージ形式は proto.h を参照。
 *
 * 設計メモ:
 *   - copy_with_damage で「画面が変化したときだけ」フレームが完了する
 *     （Stage 2 で実証済み）。静止画面では何も送らない。
 *   - Hyprland は damage 矩形を常に全画面で報告するため（Stage 2 の発見）、
 *     矩形単位の差分送信はせず全フレームを送る。差分圧縮は後の Stage の課題。
 *   - レート制限は CLOCK_MONOTONIC の絶対時刻で行い、送信処理の所要時間に
 *     よらず一定周期を保つ。
 *   - Stage 4: wayland ソケットと stdin を poll(2) で多重化し、フレーム
 *     待ちの間も入力メッセージを遅延なく注入する（inject.c）。
 */
#define _GNU_SOURCE /* memfd_create のため */
#include "inject.h"
#include "outputs.h"
#include "proto.h"
#include "virtual-keyboard-unstable-v1.h"
#include "wlr-screencopy-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

/* コンポジタへ接続する。ssh 経由の非対話セッションには WAYLAND_DISPLAY が
 * 無い（リモートデスクトップのホスト側では典型的な状況）ため、未設定なら
 * XDG_RUNTIME_DIR の wayland-* ソケットを走査して自動検出する */
static struct wl_display *connect_display(void) {
  /* WAYLAND_DISPLAY が設定されていればそれに従う */
  if (getenv("WAYLAND_DISPLAY"))
    return wl_display_connect(NULL);

  const char *rt = getenv("XDG_RUNTIME_DIR");
  if (!rt) {
    fprintf(stderr, "エラー: XDG_RUNTIME_DIR が未設定\n");
    return NULL;
  }
  DIR *dir = opendir(rt);
  if (!dir)
    return NULL;

  struct wl_display *display = NULL;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "wayland-", 8) != 0)
      continue;
    if (strstr(ent->d_name, ".lock"))
      continue;
    display = wl_display_connect(ent->d_name);
    if (display) {
      fprintf(stderr, "コンポジタを自動検出: %s/%s\n", rt, ent->d_name);
      break;
    }
  }
  closedir(dir);
  return display;
}

struct state {
  /* グローバル */
  struct wl_shm *shm;
  struct zwlr_screencopy_manager_v1 *screencopy;
  struct wlrd_outputs outs;
  struct wl_output *output; /* 選択された出力 */

  /* 入力注入（vp_mgr / vk_mgr / seat は registry で直接ここに bind） */
  struct wlrd_inject inj;

  /* コンポジタが提示してきたバッファ仕様 */
  uint32_t format, width, height, stride;
  int have_buffer_info;

  /* 再利用する共有メモリバッファ */
  struct wl_buffer *buffer;
  void *shm_data;
  size_t shm_size;
  uint32_t buf_width, buf_height;

  /* 動作モード */
  int full_copy;

  /* フレームごとのフラグ */
  int y_invert;

  int done;
  int failed;
};

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---------------------------------------------------------------- registry */

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version) {
  struct state *st = data;

  if (strcmp(interface, wl_shm_interface.name) == 0) {
    st->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    wlrd_outputs_bind(&st->outs, registry, name, version);
  } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) ==
             0) {
    uint32_t v = version < 3 ? version : 3;
    st->screencopy = wl_registry_bind(registry, name,
                                      &zwlr_screencopy_manager_v1_interface, v);
  } else if (strcmp(interface,
                    zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    /* v2 の create_virtual_pointer_with_output を使いたい */
    st->inj.vp_mgr_version = version < 2 ? version : 2;
    st->inj.vp_mgr =
        wl_registry_bind(registry, name,
                         &zwlr_virtual_pointer_manager_v1_interface,
                         st->inj.vp_mgr_version);
  } else if (strcmp(interface,
                    zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
    st->inj.vk_mgr = wl_registry_bind(
        registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    uint32_t v = version < 4 ? version : 4;
    st->inj.seat = wl_registry_bind(registry, name, &wl_seat_interface, v);
  }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------ shm バッファ */

static struct wl_buffer *create_shm_buffer(struct state *st) {
  st->shm_size = (size_t)st->stride * st->height;

  int fd = memfd_create("wlrd-send", 0);
  if (fd < 0 || ftruncate(fd, st->shm_size) < 0) {
    perror("memfd_create/ftruncate");
    return NULL;
  }
  st->shm_data =
      mmap(NULL, st->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
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

  st->buf_width = st->width;
  st->buf_height = st->height;
  return buffer;
}

/* ------------------------------------------------------- screencopy frame */

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height,
                         uint32_t stride) {
  (void)frame;
  struct state *st = data;

  int preferred =
      (format == WL_SHM_FORMAT_XRGB8888 || format == WL_SHM_FORMAT_ARGB8888);
  if (!st->have_buffer_info || preferred) {
    st->format = format;
    st->width = width;
    st->height = height;
    st->stride = stride;
    st->have_buffer_info = 1;
  }
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags) {
  (void)frame;
  struct state *st = data;
  st->y_invert = !!(flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT);
}

static void frame_linux_dmabuf(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width,
                               uint32_t height) {
  (void)data;
  (void)frame;
  (void)format;
  (void)width;
  (void)height;
}

static void frame_buffer_done(void *data,
                              struct zwlr_screencopy_frame_v1 *frame) {
  struct state *st = data;

  if (!st->have_buffer_info) {
    fprintf(stderr, "エラー: バッファ形式が提示されなかった\n");
    st->failed = 1;
    return;
  }
  if (!st->buffer) {
    st->buffer = create_shm_buffer(st);
    if (!st->buffer) {
      st->failed = 1;
      return;
    }
  } else if (st->width != st->buf_width || st->height != st->buf_height) {
    /* 解像度変更への追従（再ネゴシエーション）は将来の課題 */
    fprintf(stderr, "エラー: 配信中に解像度が変化した\n");
    st->failed = 1;
    return;
  }

  if (st->full_copy)
    zwlr_screencopy_frame_v1_copy(frame, st->buffer);
  else
    zwlr_screencopy_frame_v1_copy_with_damage(frame, st->buffer);
}

static void frame_damage(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  /* Hyprland は常に全画面を報告するため矩形情報は使わない (Stage 2) */
  (void)data;
  (void)frame;
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                        uint32_t tv_nsec) {
  (void)frame;
  (void)tv_sec_hi;
  (void)tv_sec_lo;
  (void)tv_nsec;
  struct state *st = data;
  st->done = 1;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
  (void)frame;
  struct state *st = data;
  fprintf(stderr, "エラー: コンポジタがキャプチャを拒否した\n");
  st->failed = 1;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .damage = frame_damage,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

/* ---------------------------------------------------------------- 送信処理 */

/* 部分書き込み・シグナル割り込みに耐える write。
 * 戻り値 0 = 成功, -1 = 失敗（EPIPE = 受信側切断を含む） */
static int write_all(int fd, const void *buf, size_t len) {
  const uint8_t *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

/* shm 上のフレームを stride を除去した詰め詰めの列に整形して送る。
 * y_invert のときは行順を反転して正立に直す */
static int send_frame(struct state *st, uint8_t *pack) {
  uint32_t w = st->buf_width, h = st->buf_height;
  size_t row_bytes = (size_t)w * 4;

  for (uint32_t y = 0; y < h; y++) {
    uint32_t src_y = st->y_invert ? h - 1 - y : y;
    memcpy(pack + y * row_bytes,
           (const uint8_t *)st->shm_data + (size_t)src_y * st->stride,
           row_bytes);
  }

  uint8_t hdr[WLRD_FRAME_HDR_SIZE];
  wlrd_put_u32(hdr, (uint32_t)(row_bytes * h));
  wlrd_put_u64(hdr + 4, now_ns());

  if (write_all(STDOUT_FILENO, hdr, sizeof hdr) < 0 ||
      write_all(STDOUT_FILENO, pack, row_bytes * h) < 0)
    return -1;
  return 0;
}

/* ------------------------------------------------------- 入力チャネル処理 */

/* wayland ソケットと stdin（入力チャネル）を1回の poll で多重化する。
 * 入力メッセージは 16 バイト固定長だが、パイプは任意の位置で分割される
 * ため部分読みをバッファに溜めて組み立てる */
struct pump {
  int input_fd; /* -1 なら poll が無視する（入力無効） */
  uint8_t buf[WLRD_INPUT_MSG_SIZE];
  size_t got;
  int input_eof;
  int inject_ready; /* 仮想デバイス作成前に届いた入力は捨てる */
  struct wlrd_inject *inj;
};

/* 戻り値: 0 = 継続, -1 = エラー */
static int pump_once(struct wl_display *display, struct pump *p,
                     int timeout_ms) {
  /* 溜まった要求（frame() や copy 等）を書き出してから眠る */
  wl_display_flush(display);

  struct pollfd pfds[2] = {
      {.fd = wl_display_get_fd(display), .events = POLLIN},
      {.fd = p->input_fd, .events = POLLIN},
  };
  int n = poll(pfds, 2, timeout_ms);
  if (n < 0)
    return errno == EINTR ? 0 : -1;

  if (pfds[1].revents & (POLLIN | POLLHUP)) {
    for (;;) {
      ssize_t r =
          read(p->input_fd, p->buf + p->got, WLRD_INPUT_MSG_SIZE - p->got);
      if (r == 0) { /* EOF: ビューア側が終了した */
        p->input_eof = 1;
        break;
      }
      if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        if (errno == EINTR)
          continue;
        return -1;
      }
      p->got += (size_t)r;
      if (p->got == WLRD_INPUT_MSG_SIZE) {
        p->got = 0;
        if (p->inject_ready)
          wlrd_inject_msg(p->inj, p->buf);
      }
    }
  }

  if (pfds[0].revents & POLLIN) {
    if (wl_display_dispatch(display) < 0)
      return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
  int out_idx = 0;
  int list_only = 0;
  int max_fps = 30;
  int overlay_cursor = 1; /* リモートデスクトップではカーソルが見えてほしい */
  int full_copy = 0;
  int no_input = 0;

  int opt;
  while ((opt = getopt(argc, argv, "o:lr:CfIh")) != -1) {
    switch (opt) {
    case 'o':
      out_idx = atoi(optarg);
      break;
    case 'l':
      list_only = 1;
      break;
    case 'r':
      max_fps = atoi(optarg);
      break;
    case 'C':
      overlay_cursor = 0;
      break;
    case 'f':
      full_copy = 1;
      break;
    case 'I':
      no_input = 1;
      break;
    default:
      fprintf(stderr,
              "使い方: %s [-o 出力番号] [-l 一覧] [-r 最大fps] "
              "[-C カーソルなし] [-f 全フレーム送信] [-I 入力無効]\n",
              argv[0]);
      return 2;
    }
  }
  if (max_fps <= 0) {
    fprintf(stderr, "エラー: -r は正の値を指定する\n");
    return 2;
  }

  if (!list_only && isatty(STDOUT_FILENO)) {
    fprintf(stderr, "エラー: stdout がバイナリストリームになる。"
                    "パイプかリダイレクトで使う（例: wlrd-send | wlrd-view）\n");
    return 2;
  }

  /* 受信側が閉じたら write が EPIPE を返すようにし、シグナル死を避ける */
  signal(SIGPIPE, SIG_IGN);

  struct state st = {0};
  st.full_copy = full_copy;

  struct wl_display *display = connect_display();
  if (!display) {
    fprintf(stderr,
            "エラー: コンポジタに接続できない（Wayland セッション内か？ "
            "WAYLAND_DISPLAY / XDG_RUNTIME_DIR を確認）\n");
    return 1;
  }

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, &st);
  /* roundtrip 1回目: global の bind */
  wl_display_roundtrip(display);

  /* 入力を使うか決める。stdin が端末なら入力メッセージは来ない
   * （`wlrd-send | wlrd-view` の見るだけ用法）ので自動で無効化する */
  int input_enabled = !list_only && !no_input && !isatty(STDIN_FILENO);
  if (input_enabled &&
      (!st.inj.vp_mgr || !st.inj.vk_mgr || !st.inj.seat)) {
    fprintf(stderr, "注意: 仮想入力プロトコルが無いため入力注入を無効化\n");
    input_enabled = 0;
  }
  if (input_enabled)
    wlrd_inject_prepare(&st.inj);

  /* 2回目: wl_output の name と wl_seat の capabilities、
   * 3回目: 実キーボードの keymap（prepare が仕込んだもの） */
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);

  if (list_only) {
    for (int i = 0; i < st.outs.count; i++)
      printf("%d\t%s\n", i, st.outs.names[i]);
    return 0;
  }

  if (!st.shm || st.outs.count == 0 || !st.screencopy) {
    fprintf(stderr, "エラー: 必要なプロトコルが不足している\n");
    return 1;
  }
  if (out_idx < 0 || out_idx >= st.outs.count) {
    fprintf(stderr, "エラー: 出力番号 %d は範囲外（0〜%d。-l で一覧）\n",
            out_idx, st.outs.count - 1);
    return 2;
  }
  st.output = st.outs.outputs[out_idx];

  struct pump pc = {.input_fd = -1, .inj = &st.inj};
  if (input_enabled) {
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    pc.input_fd = STDIN_FILENO;
  }

  fprintf(stderr, "配信開始: 出力=%s, 上限 %d fps, カーソル%s, 入力%s\n",
          st.outs.names[out_idx], max_fps, overlay_cursor ? "あり" : "なし",
          input_enabled ? "有効" : "無効");

  uint8_t *pack = NULL;      /* stride 除去済みフレームの送信用バッファ */
  int sent_stream_hdr = 0;
  uint64_t frames = 0, bytes = 0;
  uint64_t t_start = now_ns(), t_log = t_start;
  const uint64_t interval_ns = 1000000000ull / (uint64_t)max_fps;
  uint64_t next_ns = now_ns();

  while (!st.failed && !pc.input_eof) {
    /* レート制限フェーズ: 次スロット (next_ns) まで待つ。眠っている間も
     * 入力メッセージは poll 経由で処理し続ける（注入遅延を作らない） */
    for (;;) {
      uint64_t now = now_ns();
      if (now >= next_ns || pc.input_eof || st.failed)
        break;
      int timeout_ms = (int)((next_ns - now) / 1000000ull) + 1;
      if (pump_once(display, &pc, timeout_ms) < 0) {
        perror("pump");
        st.failed = 1;
      }
    }
    if (st.failed || pc.input_eof)
      break;
    next_ns += interval_ns;
    if (next_ns < now_ns()) /* 遅れているなら周期を現在に引き直す */
      next_ns = now_ns() + interval_ns;

    /* フレーム要求フェーズ: copy_with_damage なら画面変化まで待つ。
     * この間も入力は処理される */
    st.done = 0;
    struct zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(st.screencopy,
                                                  overlay_cursor, st.output);
    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, &st);

    while (!st.done && !st.failed && !pc.input_eof) {
      if (pump_once(display, &pc, -1) < 0) {
        perror("pump");
        st.failed = 1;
      }
    }
    if (st.failed || pc.input_eof) {
      zwlr_screencopy_frame_v1_destroy(frame);
      break;
    }

    /* 初回のみ: 寸法が確定したのでストリームヘッダを送る */
    if (!sent_stream_hdr) {
      uint8_t hdr[WLRD_STREAM_HDR_SIZE];
      memcpy(hdr, WLRD_MAGIC, 4);
      wlrd_put_u32(hdr + 4, WLRD_VERSION);
      wlrd_put_u32(hdr + 8, st.buf_width);
      wlrd_put_u32(hdr + 12, st.buf_height);
      if (write_all(STDOUT_FILENO, hdr, sizeof hdr) < 0)
        break;
      pack = malloc((size_t)st.buf_width * st.buf_height * 4);
      if (!pack) {
        fprintf(stderr, "エラー: メモリ確保失敗\n");
        return 1;
      }
      sent_stream_hdr = 1;

      /* 解像度が確定したのでここで仮想入力デバイスを作る
       * （motion_absolute の座標範囲 = キャプチャ解像度） */
      if (input_enabled) {
        st.inj.output = st.output;
        st.inj.width = st.buf_width;
        st.inj.height = st.buf_height;
        if (wlrd_inject_create(&st.inj) == 0) {
          pc.inject_ready = 1;
          fprintf(stderr, "入力注入: 準備完了\n");
        } else {
          fprintf(stderr, "注意: 仮想入力デバイスの作成に失敗、入力無効\n");
        }
      }
    }

    if (send_frame(&st, pack) < 0) {
      fprintf(stderr, "受信側が切断した (%s)\n", strerror(errno));
      zwlr_screencopy_frame_v1_destroy(frame);
      break;
    }
    zwlr_screencopy_frame_v1_destroy(frame);
    frames++;
    bytes += (uint64_t)st.buf_width * st.buf_height * 4 + WLRD_FRAME_HDR_SIZE;

    /* 2秒ごとに稼働統計を stderr へ */
    uint64_t now = now_ns();
    if (now - t_log >= 2000000000ull) {
      double el = (now - t_start) / 1e9;
      fprintf(stderr, "%llu フレーム送信, 平均 %.1f fps, %.1f MB/s\n",
              (unsigned long long)frames, frames / el, bytes / el / 1e6);
      t_log = now;
    }
  }

  if (pc.input_eof)
    fprintf(stderr, "入力チャネルが閉じた（ビューア終了）、配信を終える\n");

  free(pack);
  if (st.buffer)
    wl_buffer_destroy(st.buffer);
  if (st.shm_data)
    munmap(st.shm_data, st.shm_size);
  wl_display_disconnect(display);
  return st.failed ? 1 : 0;
}

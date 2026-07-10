/*
 * wlrd-view.c — Stage 3: stdin のフレームストリームを SDL2 ウィンドウに表示する
 *
 * 使い方:
 *   ./wlrd-send | ./wlrd-view              # ローカル確認
 *   ssh host wlrd-send | ./wlrd-view       # リモート表示
 *   q または ESC、ウィンドウクローズで終了する。
 *
 * 設計メモ:
 *   - stdin は O_NONBLOCK にし、poll(2) で「SDL イベント処理」と
 *     「ストリーム読み込み」を1本のループで回す。フレームが来ない間も
 *     ウィンドウ操作（リサイズ・終了）に反応できる。
 *   - フレームは部分的に届く（パイプ/TCP は任意の位置で分割される）ため、
 *     「ヘッダ集め → ペイロード集め」の状態機械で組み立てる。
 *   - ピクセルは [B,G,R,X] 並び（proto.h）。これは SDL の
 *     SDL_PIXELFORMAT_ARGB8888（リトルエンディアンで同じメモリ配置）に
 *     一致するので変換なしでテクスチャに流し込める。
 */
#include "proto.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ちょうど len バイト読めるまでブロックして読む（ストリームヘッダ用）。
 * 戻り値 0 = 成功, -1 = EOF/エラー */
static int read_exact(int fd, void *buf, size_t len) {
  uint8_t *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n == 0)
      return -1; /* EOF */
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

int main(void) {
  /* ---- ストリームヘッダ（この時点ではブロッキング読みでよい） ---- */
  uint8_t shdr[WLRD_STREAM_HDR_SIZE];
  if (read_exact(STDIN_FILENO, shdr, sizeof shdr) < 0) {
    fprintf(stderr, "エラー: ストリームヘッダを読めない"
                    "（wlrd-send からパイプで繋いでいるか？）\n");
    return 1;
  }
  if (memcmp(shdr, WLRD_MAGIC, 4) != 0 ||
      wlrd_get_u32(shdr + 4) != WLRD_VERSION) {
    fprintf(stderr, "エラー: ストリーム形式が不正（magic/version 不一致）\n");
    return 1;
  }
  uint32_t width = wlrd_get_u32(shdr + 8);
  uint32_t height = wlrd_get_u32(shdr + 12);
  if (width == 0 || height == 0 || width > 16384 || height > 16384) {
    fprintf(stderr, "エラー: 解像度が異常 (%ux%u)\n", width, height);
    return 1;
  }
  const size_t frame_bytes = (size_t)width * height * 4;
  fprintf(stderr, "受信開始: %ux%u\n", width, height);

  /* ---- SDL 初期化 ---- */
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "エラー: SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *win = SDL_CreateWindow(
      "wlrd-view", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, (int)width,
      (int)height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
  if (!win || !ren) {
    fprintf(stderr, "エラー: SDL: %s\n", SDL_GetError());
    return 1;
  }
  /* 論理サイズを固定するとリサイズ時にアスペクト比を保って自動スケール
   * される（レターボックス） */
  SDL_RenderSetLogicalSize(ren, (int)width, (int)height);
  SDL_Texture *tex =
      SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                        SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
  if (!tex) {
    fprintf(stderr, "エラー: SDL_CreateTexture: %s\n", SDL_GetError());
    return 1;
  }

  /* ---- 受信状態機械 ---- */
  fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
  uint8_t fhdr[WLRD_FRAME_HDR_SIZE];
  uint8_t *payload = malloc(frame_bytes);
  if (!payload) {
    fprintf(stderr, "エラー: メモリ確保失敗\n");
    return 1;
  }
  size_t got = 0;      /* 現フェーズで読めたバイト数 */
  int in_payload = 0;  /* 0: fhdr 集め中, 1: payload 集め中 */
  uint64_t frame_ts = 0;

  uint64_t frames = 0;
  uint64_t t_title = now_ns();
  uint64_t frames_at_title = 0;
  double last_latency_ms = 0;

  int running = 1;
  int eof = 0;
  while (running) {
    /* ウィンドウイベント処理（フレームが来なくても回る） */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT ||
          (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q ||
                                     e.key.keysym.sym == SDLK_ESCAPE)))
        running = 0;
    }

    /* stdin が読めるようになるまで最大 15ms 待つ（その間も上の
     * イベント処理に定期的に戻ってくる） */
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN};
    int pr = poll(&pfd, 1, 15);
    if (pr <= 0 && !eof)
      continue;

    /* 読めるだけ読み、フレームが完成するたびに描画する */
    int drew = 0;
    for (;;) {
      uint8_t *dst = in_payload ? payload : fhdr;
      size_t want = in_payload ? frame_bytes : sizeof fhdr;
      ssize_t n = read(STDIN_FILENO, dst + got, want - got);
      if (n == 0) { /* EOF: 送信側が終了した */
        eof = 1;
        running = 0;
        break;
      }
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break; /* 今読める分は読み切った */
        if (errno == EINTR)
          continue;
        perror("read");
        running = 0;
        break;
      }
      got += (size_t)n;
      if (got < want)
        continue;

      /* フェーズ完了 */
      got = 0;
      if (!in_payload) {
        uint32_t size = wlrd_get_u32(fhdr);
        frame_ts = wlrd_get_u64(fhdr + 4);
        if (size != frame_bytes) {
          fprintf(stderr, "エラー: フレーム長不一致 (%u != %zu)\n", size,
                  frame_bytes);
          running = 0;
          break;
        }
        in_payload = 1;
      } else {
        in_payload = 0;
        SDL_UpdateTexture(tex, NULL, payload, (int)width * 4);
        frames++;
        /* 送信側と同一ホストのときだけ意味を持つ片道遅延
         * （時計 CLOCK_MONOTONIC が共有されているため） */
        last_latency_ms = (now_ns() - frame_ts) / 1e6;
        drew = 1;
      }
    }

    if (drew) {
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, NULL, NULL);
      SDL_RenderPresent(ren);
    }

    /* 1秒ごとにタイトルへ統計を出す */
    uint64_t now = now_ns();
    if (now - t_title >= 1000000000ull) {
      char title[128];
      snprintf(title, sizeof title, "wlrd-view %ux%u | %.1f fps | %.1f ms",
               width, height,
               (frames - frames_at_title) * 1e9 / (double)(now - t_title),
               last_latency_ms);
      SDL_SetWindowTitle(win, title);
      t_title = now;
      frames_at_title = frames;
    }
  }

  fprintf(stderr, "受信終了: %llu フレーム%s\n", (unsigned long long)frames,
          eof ? "（送信側が終了）" : "");
  free(payload);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  return 0;
}

/*
 * wlrd-view.c — Stage 3+4: フレームストリームを SDL2 で表示し、入力を送り返す
 *
 * 使い方:
 *   ./wlrd-view ./wlrd-send -o 0           # ローカル（send を子として起動）
 *   ./wlrd-view ssh host wlrd-send         # リモート（ssh が運び屋）
 *   ./wlrd-send | ./wlrd-view              # 見るだけ（入力は送れない）
 *
 * 引数を与えると、それをトランスポートコマンドとして起動し双方向パイプを
 * 張る（rsync -e ssh や git と同じ方式）。子の stdout がフレーム、
 * 子の stdin が入力メッセージの通り道になる。
 *
 * 終了はウィンドウクローズ（入力転送中は q/ESC もリモートへ送るため）。
 * 見るだけモードのときのみ q / ESC でも終了できる。
 *
 * 設計メモ:
 *   - 受信 fd は O_NONBLOCK にし、poll(2) で「SDL イベント処理」と
 *     「ストリーム読み込み」を1本のループで回す。フレームが来ない間も
 *     ウィンドウ操作（リサイズ・終了）に反応できる。
 *   - フレームは部分的に届く（パイプ/TCP は任意の位置で分割される）ため、
 *     「ヘッダ集め → ペイロード集め」の状態機械で組み立てる。
 *   - ピクセルは [B,G,R,X] 並び（proto.h）。これは SDL の
 *     SDL_PIXELFORMAT_ARGB8888（リトルエンディアンで同じメモリ配置）に
 *     一致するので変換なしでテクスチャに流し込める。
 *   - キーは SDL スキャンコード → evdev 変換表（keymap-sdl-evdev.h）で
 *     写像して送る。キーリピートは送らない（Wayland ではリピートは
 *     受信側クライアントが自前で行うため、押しっぱなしで自然に効く）。
 */
#include "keymap-sdl-evdev.h"
#include "proto.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

/* 入力メッセージを1件送る。失敗したら以後の入力転送を止める（*out_fd を
 * -1 にする）が、表示は続行する */
static void send_input(int *out_fd, uint8_t type, uint32_t a, uint32_t b) {
  if (*out_fd < 0)
    return;
  uint8_t msg[WLRD_INPUT_MSG_SIZE];
  wlrd_put_input(msg, type, a, b);
  if (write_all(*out_fd, msg, sizeof msg) < 0) {
    fprintf(stderr, "注意: 入力チャネルが閉じた、以後は表示のみ\n");
    *out_fd = -1;
  }
}

/* トランスポートコマンドを起動し双方向パイプを張る。
 * 戻り値: 子の pid。*in_fd = 子の stdout, *out_fd = 子の stdin */
static pid_t spawn_transport(char **argv, int *in_fd, int *out_fd) {
  int to_child[2], from_child[2];
  if (pipe(to_child) < 0 || pipe(from_child) < 0) {
    perror("pipe");
    exit(1);
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    exit(1);
  }
  if (pid == 0) { /* 子: パイプを stdin/stdout に付け替えて exec */
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[0]);
    close(to_child[1]);
    close(from_child[0]);
    close(from_child[1]);
    execvp(argv[0], argv);
    perror(argv[0]);
    _exit(127);
  }
  close(to_child[0]);
  close(from_child[1]);
  *in_fd = from_child[0];
  *out_fd = to_child[1];
  return pid;
}

int main(int argc, char **argv) {
  int in_fd = STDIN_FILENO;
  int out_fd = -1;
  pid_t child = -1;

  /* 子（トランスポート）への write で落ちないように */
  signal(SIGPIPE, SIG_IGN);

  if (argc > 1) {
    /* 引数 = トランスポートコマンド。双方向パイプで入力も送れる */
    child = spawn_transport(argv + 1, &in_fd, &out_fd);
  } else if (!isatty(STDOUT_FILENO)) {
    /* 上級用法: stdout がパイプなら入力メッセージをそこへ流す
     * （FIFO で自前配管する場合） */
    out_fd = STDOUT_FILENO;
  }

  /* ---- ストリームヘッダ（この時点ではブロッキング読みでよい） ---- */
  uint8_t shdr[WLRD_STREAM_HDR_SIZE];
  if (read_exact(in_fd, shdr, sizeof shdr) < 0) {
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
  fcntl(in_fd, F_SETFL, O_NONBLOCK);
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
    /* ウィンドウイベント処理（フレームが来なくても回る）。
     * 入力転送が有効なら、マウス・キーをメッセージにしてリモートへ送る */
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
      case SDL_QUIT:
        running = 0;
        break;

      case SDL_MOUSEMOTION:
        /* 論理サイズ (SDL_RenderSetLogicalSize) を設定してあるので
         * イベント座標は SDL がリモート解像度系に変換済み */
        if (e.motion.x >= 0 && e.motion.y >= 0)
          send_input(&out_fd, WLRD_INPUT_MOTION, (uint32_t)e.motion.x,
                     (uint32_t)e.motion.y);
        break;

      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        uint32_t btn = 0; /* evdev の BTN_* コード */
        switch (e.button.button) {
        case SDL_BUTTON_LEFT:
          btn = BTN_LEFT;
          break;
        case SDL_BUTTON_MIDDLE:
          btn = BTN_MIDDLE;
          break;
        case SDL_BUTTON_RIGHT:
          btn = BTN_RIGHT;
          break;
        case SDL_BUTTON_X1:
          btn = BTN_SIDE;
          break;
        case SDL_BUTTON_X2:
          btn = BTN_EXTRA;
          break;
        }
        if (btn)
          send_input(&out_fd, WLRD_INPUT_BUTTON, btn,
                     e.type == SDL_MOUSEBUTTONDOWN ? 1 : 0);
        break;
      }

      case SDL_MOUSEWHEEL: {
        /* SDL は上回転が +y、wl_pointer は上スクロールが負。符号を反転 */
        int32_t vy = -e.wheel.y;
        int32_t vx = e.wheel.x;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
          vy = -vy;
          vx = -vx;
        }
        if (vy)
          send_input(&out_fd, WLRD_INPUT_AXIS, 0, (uint32_t)vy);
        if (vx)
          send_input(&out_fd, WLRD_INPUT_AXIS, 1, (uint32_t)vx);
        break;
      }

      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        /* 見るだけモードのときだけ q/ESC をローカル終了に使う */
        if (out_fd < 0) {
          if (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_q ||
                                        e.key.keysym.sym == SDLK_ESCAPE))
            running = 0;
          break;
        }
        if (e.key.repeat)
          break; /* リピートは受信側クライアントが自前で行う */
        uint16_t code = 0;
        if (e.key.keysym.scancode < SDL_NUM_SCANCODES)
          code = wlrd_sdl_to_evdev[e.key.keysym.scancode];
        if (code)
          send_input(&out_fd, WLRD_INPUT_KEY, code,
                     e.type == SDL_KEYDOWN ? 1 : 0);
        break;
      }
      }
    }

    /* 受信 fd が読めるようになるまで最大 15ms 待つ（その間も上の
     * イベント処理に定期的に戻ってくる） */
    struct pollfd pfd = {.fd = in_fd, .events = POLLIN};
    int pr = poll(&pfd, 1, 15);
    if (pr <= 0 && !eof)
      continue;

    /* 読めるだけ読み、フレームが完成するたびに描画する */
    int drew = 0;
    for (;;) {
      uint8_t *dst = in_payload ? payload : fhdr;
      size_t want = in_payload ? frame_bytes : sizeof fhdr;
      ssize_t n = read(in_fd, dst + got, want - got);
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

  /* トランスポート子プロセスの後始末: 入力チャネルを閉じれば send 側が
   * EOF を検知して自発的に終了する（シグナル不要） */
  if (child > 0) {
    if (out_fd >= 0)
      close(out_fd);
    close(in_fd);
    waitpid(child, NULL, 0);
  }
  return 0;
}

/*
 * inject.c — 仮想ポインタ/キーボードによる入力注入の実装
 *
 * キーボードの設計判断:
 *   virtual-keyboard プロトコルではクライアントが keymap を提供する。
 *   ここでは「ホストの実キーボードの keymap を wl_seat 経由で取得して
 *   そのまま渡す」方式を採る。注入した evdev キーコードの解釈が物理
 *   キーボードと完全に一致するため、レイアウト（jp/us）の食い違いが
 *   原理的に起きない。実キーボードが無い場合は xkbcommon の既定
 *   （XKB_DEFAULT_LAYOUT 等の環境変数）にフォールバックする。
 *
 *   修飾キー状態は xkbcommon の xkb_state で追跡し、キーごとに
 *   modifiers リクエストで明示送信する（wtype と同方式）。コンポジタが
 *   キーコードから自前計算してくれる実装もあるが、プロトコル上は
 *   クライアントの責務である。
 *
 * ポインタの設計判断:
 *   virtual-pointer v2 の create_virtual_pointer_with_output で対象出力に
 *   紐付け、motion_absolute の座標系をキャプチャ解像度に一致させる。
 *   これにより多モニタ構成でも view のウィンドウ座標がそのまま使える。
 */
#define _GNU_SOURCE /* memfd_create のため */
#include "inject.h"
#include "proto.h"
#include "virtual-keyboard-unstable-v1.h"
#include "wlr-virtual-pointer-unstable-v1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

/* wl_keyboard キーコード → xkb キーコードのオフセット（歴史的経緯） */
#define EVDEV_TO_XKB_OFFSET 8

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---------------------------------------------- 実キーボードの keymap 取得 */

static void kbd_keymap(void *data, struct wl_keyboard *kbd, uint32_t format,
                       int32_t fd, uint32_t size) {
  (void)kbd;
  struct wlrd_inject *inj = data;

  if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && !inj->keymap_str) {
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map != MAP_FAILED) {
      /* NUL 終端込みで複製して保持する */
      inj->keymap_str = strndup(map, size);
      munmap(map, size);
    }
  }
  close(fd);
}

/* keymap 以外のキーボードイベントは不要（フォーカスを持たないので
 * 実際にはほぼ届かない） */
static void kbd_enter(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *su, struct wl_array *a) {
  (void)d;
  (void)k;
  (void)s;
  (void)su;
  (void)a;
}
static void kbd_leave(void *d, struct wl_keyboard *k, uint32_t s,
                      struct wl_surface *su) {
  (void)d;
  (void)k;
  (void)s;
  (void)su;
}
static void kbd_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t,
                    uint32_t key, uint32_t st) {
  (void)d;
  (void)k;
  (void)s;
  (void)t;
  (void)key;
  (void)st;
}
static void kbd_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
                          uint32_t dep, uint32_t lat, uint32_t loc,
                          uint32_t grp) {
  (void)d;
  (void)k;
  (void)s;
  (void)dep;
  (void)lat;
  (void)loc;
  (void)grp;
}
static void kbd_repeat_info(void *d, struct wl_keyboard *k, int32_t rate,
                            int32_t delay) {
  (void)d;
  (void)k;
  (void)rate;
  (void)delay;
}

static const struct wl_keyboard_listener kbd_listener = {
    .keymap = kbd_keymap,
    .enter = kbd_enter,
    .leave = kbd_leave,
    .key = kbd_key,
    .modifiers = kbd_modifiers,
    .repeat_info = kbd_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
                              uint32_t caps) {
  struct wlrd_inject *inj = data;
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !inj->real_kbd) {
    inj->real_kbd = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(inj->real_kbd, &kbd_listener, inj);
  }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
  (void)data;
  (void)seat;
  (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

void wlrd_inject_prepare(struct wlrd_inject *inj) {
  wl_seat_add_listener(inj->seat, &seat_listener, inj);
  /* この後、呼び出し側で roundtrip 2回:
   * 1回目で capabilities（→ get_keyboard）、2回目で keymap が届く */
}

/* ------------------------------------------------------- デバイスの作成 */

int wlrd_inject_create(struct wlrd_inject *inj) {
  /* --- 仮想ポインタ。v2 なら出力に紐付けて座標系を固定する --- */
  if (inj->vp_mgr_version >= 2)
    inj->vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
        inj->vp_mgr, NULL, inj->output);
  else
    inj->vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        inj->vp_mgr, NULL);

  /* --- keymap。実キーボードから取れなければ環境の既定で生成 --- */
  inj->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap = NULL;
  if (inj->keymap_str) {
    keymap = xkb_keymap_new_from_string(inj->xkb_ctx, inj->keymap_str,
                                        XKB_KEYMAP_FORMAT_TEXT_V1,
                                        XKB_KEYMAP_COMPILE_NO_FLAGS);
  }
  if (!keymap) {
    fprintf(stderr, "注意: 実キーボードの keymap が取れず、既定にフォールバック\n");
    keymap = xkb_keymap_new_from_names(inj->xkb_ctx, NULL,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    free(inj->keymap_str);
    inj->keymap_str =
        xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  }
  if (!keymap || !inj->keymap_str) {
    fprintf(stderr, "エラー: keymap を用意できない\n");
    return -1;
  }
  inj->xkb_state = xkb_state_new(keymap);

  /* --- 仮想キーボードを作り keymap を送る（memfd 経由） --- */
  inj->vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
      inj->vk_mgr, inj->seat);

  size_t len = strlen(inj->keymap_str) + 1; /* NUL 終端込みで渡す慣習 */
  int fd = memfd_create("wlrd-keymap", 0);
  if (fd < 0 || ftruncate(fd, len) < 0) {
    perror("memfd_create/ftruncate");
    return -1;
  }
  void *dst = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (dst == MAP_FAILED) {
    perror("mmap");
    close(fd);
    return -1;
  }
  memcpy(dst, inj->keymap_str, len);
  munmap(dst, len);
  zwp_virtual_keyboard_v1_keymap(inj->vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                                 (uint32_t)len);
  close(fd);

  xkb_keymap_unref(keymap);
  return 0;
}

/* ------------------------------------------------------------ 注入の実行 */

static void inject_key(struct wlrd_inject *inj, uint32_t keycode,
                       uint32_t pressed) {
  uint32_t t = now_ms();
  zwp_virtual_keyboard_v1_key(inj->vk, t, keycode,
                              pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                      : WL_KEYBOARD_KEY_STATE_RELEASED);

  /* xkb 状態を進め、修飾キーの現況を明示送信する */
  xkb_state_update_key(inj->xkb_state, keycode + EVDEV_TO_XKB_OFFSET,
                       pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
  zwp_virtual_keyboard_v1_modifiers(
      inj->vk,
      xkb_state_serialize_mods(inj->xkb_state, XKB_STATE_MODS_DEPRESSED),
      xkb_state_serialize_mods(inj->xkb_state, XKB_STATE_MODS_LATCHED),
      xkb_state_serialize_mods(inj->xkb_state, XKB_STATE_MODS_LOCKED),
      xkb_state_serialize_layout(inj->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE));
}

void wlrd_inject_msg(struct wlrd_inject *inj, const uint8_t *msg) {
  uint8_t type = msg[0];
  uint32_t a = wlrd_get_u32(msg + 4);
  uint32_t b = wlrd_get_u32(msg + 8);
  uint32_t t = now_ms();

  switch (type) {
  case WLRD_INPUT_MOTION:
    if (a >= inj->width)
      a = inj->width - 1;
    if (b >= inj->height)
      b = inj->height - 1;
    zwlr_virtual_pointer_v1_motion_absolute(inj->vp, t, a, b, inj->width,
                                            inj->height);
    zwlr_virtual_pointer_v1_frame(inj->vp);
    break;

  case WLRD_INPUT_BUTTON:
    zwlr_virtual_pointer_v1_button(inj->vp, t, a,
                                   b ? WL_POINTER_BUTTON_STATE_PRESSED
                                     : WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(inj->vp);
    break;

  case WLRD_INPUT_AXIS: {
    /* b はノッチ数（i32）。wl_pointer の慣習で 1 ノッチ = 15 単位 */
    int32_t steps = (int32_t)b;
    uint32_t axis = a ? WL_POINTER_AXIS_HORIZONTAL_SCROLL
                      : WL_POINTER_AXIS_VERTICAL_SCROLL;
    zwlr_virtual_pointer_v1_axis_source(inj->vp, WL_POINTER_AXIS_SOURCE_WHEEL);
    zwlr_virtual_pointer_v1_axis_discrete(
        inj->vp, t, axis, wl_fixed_from_int(steps * 15), steps);
    zwlr_virtual_pointer_v1_frame(inj->vp);
    break;
  }

  case WLRD_INPUT_KEY:
    inject_key(inj, a, b);
    break;

  default:
    fprintf(stderr, "警告: 未知の入力メッセージ type=%u\n", type);
    break;
  }
}

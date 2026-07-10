/*
 * inject.h — 仮想ポインタ/キーボードによる入力注入（wlrd-send 側）
 *
 * 使い方（wlrd-send から）:
 *   1. registry で vp_mgr / vk_mgr / seat をこの構造体に bind して詰める
 *   2. wlrd_inject_prepare() — seat のリスナーを張り、実キーボードの
 *      keymap 取得を仕込む（この後 roundtrip を2回すること）
 *   3. wlrd_inject_create() — 仮想デバイスを作り keymap を送る
 *   4. 入力メッセージが届くたび wlrd_inject_msg()
 */
#ifndef WLRD_INJECT_H
#define WLRD_INJECT_H

#include <stdint.h>
#include <wayland-client.h>

struct zwlr_virtual_pointer_manager_v1;
struct zwlr_virtual_pointer_v1;
struct zwp_virtual_keyboard_manager_v1;
struct zwp_virtual_keyboard_v1;
struct xkb_context;
struct xkb_state;

struct wlrd_inject {
  /* 呼び出し側が bind して詰めるもの */
  struct zwlr_virtual_pointer_manager_v1 *vp_mgr;
  uint32_t vp_mgr_version;
  struct zwp_virtual_keyboard_manager_v1 *vk_mgr;
  struct wl_seat *seat;
  struct wl_output *output; /* motion_absolute の座標系を張り付ける出力 */
  uint32_t width, height;   /* キャプチャ解像度 = view が送る座標の範囲 */

  /* 内部状態 */
  struct wl_keyboard *real_kbd; /* keymap 取得用の実キーボード */
  char *keymap_str;             /* 実キーボードの keymap 文字列 */
  struct zwlr_virtual_pointer_v1 *vp;
  struct zwp_virtual_keyboard_v1 *vk;
  struct xkb_context *xkb_ctx;
  struct xkb_state *xkb_state;
};

void wlrd_inject_prepare(struct wlrd_inject *inj);
int wlrd_inject_create(struct wlrd_inject *inj);
void wlrd_inject_msg(struct wlrd_inject *inj,
                     const uint8_t msg[/* WLRD_INPUT_MSG_SIZE */]);

#endif

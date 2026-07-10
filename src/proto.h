/*
 * proto.h — wlrd フレームストリーム形式 (version 1)
 *
 * wlrd-send の stdout / wlrd-view の stdin を流れるバイト列の定義。
 * トランスポートは規定しない（パイプ・ssh・nc 等、何で運んでもよい）。
 * 多バイト値はすべてリトルエンディアン。
 *
 * ストリームヘッダ（接続時に1回、16 bytes）:
 *   offset 0: char magic[4] = "WLRD"
 *   offset 4: u32 version   = 1
 *   offset 8: u32 width
 *   offset 12: u32 height
 *
 * フレーム（繰り返し、12 + width*height*4 bytes）:
 *   offset 0: u32 size   = width*height*4（将来の圧縮・可変長フレーム用）
 *   offset 4: u64 ts_ns  = 送信側 CLOCK_MONOTONIC ナノ秒。
 *                          同一ホストで送受した場合のみ遅延測定に使える
 *   offset 12: u8 pixels[size]
 *              行優先・stride なしの詰め詰め。1ピクセル4バイトで
 *              メモリ順 [B, G, R, X]（wl_shm XRGB8888/ARGB8888 と同じ）
 */
#ifndef WLRD_PROTO_H
#define WLRD_PROTO_H

#include <stdint.h>

#define WLRD_MAGIC "WLRD"
#define WLRD_VERSION 1u
#define WLRD_STREAM_HDR_SIZE 16
#define WLRD_FRAME_HDR_SIZE 12

/* エンコード/デコードはバイト単位で書き、ホストのエンディアンに
 * 依存させない（x86 以外に持って行っても壊れない） */
static inline void wlrd_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline uint32_t wlrd_get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline void wlrd_put_u64(uint8_t *p, uint64_t v) {
  wlrd_put_u32(p, (uint32_t)(v & 0xffffffffu));
  wlrd_put_u32(p + 4, (uint32_t)(v >> 32));
}

static inline uint64_t wlrd_get_u64(const uint8_t *p) {
  return (uint64_t)wlrd_get_u32(p) | ((uint64_t)wlrd_get_u32(p + 4) << 32);
}

/*
 * 入力メッセージ（view → send、逆方向チャネル）
 *
 * ssh の双方向パイプをそのまま使う: フレームは send の stdout を、
 * 入力は send の stdin を流れる。16 バイト固定長でパーサを単純に保つ。
 *
 *   offset 0: u8  type   (enum wlrd_input_type)
 *   offset 1: u8  pad[3] = 0
 *   offset 4: u32 a
 *   offset 8: u32 b
 *   offset 12: u32 reserved = 0（将来の拡張用）
 *
 * type ごとの a, b の意味:
 *   MOTION: a = x, b = y（リモート出力のピクセル座標）
 *   BUTTON: a = evdev BTN_*（BTN_LEFT=0x110 等）, b = 1 押下 / 0 解放
 *   AXIS  : a = 0 縦 / 1 横, b = (i32) ホイールのノッチ数
 *   KEY   : a = evdev KEY_*, b = 1 押下 / 0 解放
 */
#define WLRD_INPUT_MSG_SIZE 16

enum wlrd_input_type {
  WLRD_INPUT_MOTION = 1,
  WLRD_INPUT_BUTTON = 2,
  WLRD_INPUT_AXIS = 3,
  WLRD_INPUT_KEY = 4,
};

static inline void wlrd_put_input(uint8_t *p, uint8_t type, uint32_t a,
                                  uint32_t b) {
  p[0] = type;
  p[1] = p[2] = p[3] = 0;
  wlrd_put_u32(p + 4, a);
  wlrd_put_u32(p + 8, b);
  wlrd_put_u32(p + 12, 0);
}

#endif

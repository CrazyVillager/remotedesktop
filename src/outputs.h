/*
 * outputs.h — wl_output の列挙と接続名（DP-5 等）の取得
 *
 * 複数ツール（wlrd-capture / wlrd-send）で同じ「-o で出力を選ぶ」処理が
 * 要るため切り出した共通ヘルパ。使い方:
 *
 *   struct wlrd_outputs outs = {0};
 *   // registry の global イベントの wl_output 分岐で:
 *   wlrd_outputs_bind(&outs, registry, name, version);
 *   // roundtrip を2回（bind → name イベント受信）した後:
 *   struct wl_output *out = outs.outputs[idx];
 *
 * 名前は wl_output v4 の name イベントで埋まる。v3 以前のコンポジタでは
 * "(output N)" のままになる。
 */
#ifndef WLRD_OUTPUTS_H
#define WLRD_OUTPUTS_H

#include <stdio.h>
#include <wayland-client.h>

#define WLRD_MAX_OUTPUTS 8
#define WLRD_OUTPUT_NAME_LEN 64

struct wlrd_outputs {
  struct wl_output *outputs[WLRD_MAX_OUTPUTS];
  char names[WLRD_MAX_OUTPUTS][WLRD_OUTPUT_NAME_LEN];
  int count;
};

/* ---- wl_output リスナー。必要なのは name のみだが全メンバ必須 ---- */

static void wlrd_output_geometry_(void *data, struct wl_output *output,
                                  int32_t x, int32_t y, int32_t phys_w,
                                  int32_t phys_h, int32_t subpixel,
                                  const char *make, const char *model,
                                  int32_t transform) {
  (void)data;
  (void)output;
  (void)x;
  (void)y;
  (void)phys_w;
  (void)phys_h;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;
}

static void wlrd_output_mode_(void *data, struct wl_output *output,
                              uint32_t flags, int32_t width, int32_t height,
                              int32_t refresh) {
  (void)data;
  (void)output;
  (void)flags;
  (void)width;
  (void)height;
  (void)refresh;
}

static void wlrd_output_done_(void *data, struct wl_output *output) {
  (void)data;
  (void)output;
}

static void wlrd_output_scale_(void *data, struct wl_output *output,
                               int32_t factor) {
  (void)data;
  (void)output;
  (void)factor;
}

/* data には名前の格納先（names[i]）を直接渡してある */
static void wlrd_output_name_(void *data, struct wl_output *output,
                              const char *name) {
  (void)output;
  snprintf((char *)data, WLRD_OUTPUT_NAME_LEN, "%s", name);
}

static void wlrd_output_description_(void *data, struct wl_output *output,
                                     const char *description) {
  (void)data;
  (void)output;
  (void)description;
}

static const struct wl_output_listener wlrd_output_listener_ = {
    .geometry = wlrd_output_geometry_,
    .mode = wlrd_output_mode_,
    .done = wlrd_output_done_,
    .scale = wlrd_output_scale_,
    .name = wlrd_output_name_,
    .description = wlrd_output_description_,
};

/* registry の global イベント（interface == wl_output）から呼ぶ */
static inline void wlrd_outputs_bind(struct wlrd_outputs *o,
                                     struct wl_registry *registry,
                                     uint32_t name, uint32_t version) {
  if (o->count >= WLRD_MAX_OUTPUTS)
    return;
  int i = o->count++;
  uint32_t v = version < 4 ? version : 4; /* name イベントは v4 から */
  o->outputs[i] = (struct wl_output *)wl_registry_bind(
      registry, name, &wl_output_interface, v);
  snprintf(o->names[i], WLRD_OUTPUT_NAME_LEN, "(output %d)", i);
  if (v >= 4)
    wl_output_add_listener(o->outputs[i], &wlrd_output_listener_, o->names[i]);
}

#endif

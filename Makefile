# wlrd — Wayland リモートデスクトップ（学習用・Hyprland 対象）
#
# protocol/*.xml から wayland-scanner でヘッダとスタブを生成し、
# 各ツールとリンクする。生成物は gen/ に置く。

CC             ?= cc
CFLAGS         ?= -O2 -Wall -Wextra
WAYLAND_SCANNER = wayland-scanner

PKGS    = wayland-client
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Igen
LDLIBS  = $(shell pkg-config --libs $(PKGS))

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)
XKB_LIBS   = $(shell pkg-config --libs xkbcommon)

all: wlrd-capture wlrd-bench wlrd-send wlrd-view wlrd-globals

# --- プロトコルコード生成（パターンルール） -------------------------------
# client-header: クライアント用 API 宣言
# private-code : インターフェースのメタデータ実体（リンクに必要）

gen/%.h: protocol/%.xml
	@mkdir -p gen
	$(WAYLAND_SCANNER) client-header $< $@

gen/%.c: protocol/%.xml
	@mkdir -p gen
	$(WAYLAND_SCANNER) private-code $< $@

SCREENCOPY_GEN = gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
INPUT_GEN      = gen/wlr-virtual-pointer-unstable-v1.c gen/wlr-virtual-pointer-unstable-v1.h \
                 gen/virtual-keyboard-unstable-v1.c gen/virtual-keyboard-unstable-v1.h

# --- ツール -----------------------------------------------------------------

# Stage 1: 1フレームキャプチャ → PPM
wlrd-capture: src/wlrd-capture.c src/outputs.h $(SCREENCOPY_GEN)
	$(CC) $(CFLAGS) -o $@ src/wlrd-capture.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 2: 連続キャプチャの FPS / damage 測定
wlrd-bench: src/wlrd-bench.c $(SCREENCOPY_GEN)
	$(CC) $(CFLAGS) -o $@ src/wlrd-bench.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 3+4: 配信（stdout）と入力注入（stdin）
wlrd-send: src/wlrd-send.c src/inject.c src/inject.h src/proto.h src/outputs.h $(SCREENCOPY_GEN) $(INPUT_GEN)
	$(CC) $(CFLAGS) -o $@ src/wlrd-send.c src/inject.c \
		gen/wlr-screencopy-unstable-v1.c gen/wlr-virtual-pointer-unstable-v1.c \
		gen/virtual-keyboard-unstable-v1.c $(LDLIBS) $(XKB_LIBS)

# Stage 3+4: 受信（stdin）→ SDL2 表示、入力送信（stdout）
wlrd-view: src/wlrd-view.c src/proto.h src/keymap-sdl-evdev.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ src/wlrd-view.c $(SDL_LIBS)

# Stage 0: コンポジタのグローバル列挙
wlrd-globals: tools/globals.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf gen wlrd-capture wlrd-bench wlrd-send wlrd-view wlrd-globals

.PHONY: all clean

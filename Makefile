# wlrd — Wayland リモートデスクトップ（学習用・Hyprland 対象）
#
# プロトコル XML から wayland-scanner でヘッダとスタブを生成し、
# 各ツールとリンクする。生成物は gen/ に置く。

CC             ?= cc
CFLAGS         ?= -O2 -Wall -Wextra
WAYLAND_SCANNER = wayland-scanner

PKGS    = wayland-client
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Igen
LDLIBS  = $(shell pkg-config --libs $(PKGS))

SCREENCOPY_XML = protocol/wlr-screencopy-unstable-v1.xml

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

all: wlrd-capture wlrd-bench wlrd-send wlrd-view wlrd-globals

# --- プロトコルコード生成 --------------------------------------------------
# client-header: クライアント用 API 宣言
# private-code : インターフェースのメタデータ実体（リンクに必要）

gen/wlr-screencopy-unstable-v1.h: $(SCREENCOPY_XML)
	@mkdir -p gen
	$(WAYLAND_SCANNER) client-header $< $@

gen/wlr-screencopy-unstable-v1.c: $(SCREENCOPY_XML)
	@mkdir -p gen
	$(WAYLAND_SCANNER) private-code $< $@

# --- ツール -----------------------------------------------------------------

# Stage 1: 1フレームキャプチャ → PPM
wlrd-capture: src/wlrd-capture.c src/outputs.h gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
	$(CC) $(CFLAGS) -o $@ src/wlrd-capture.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 2: 連続キャプチャの FPS / damage 測定
wlrd-bench: src/wlrd-bench.c gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
	$(CC) $(CFLAGS) -o $@ src/wlrd-bench.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 3: 連続キャプチャ → フレームストリーム送信（stdout）
wlrd-send: src/wlrd-send.c src/proto.h src/outputs.h gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
	$(CC) $(CFLAGS) -o $@ src/wlrd-send.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 3: フレームストリーム受信（stdin）→ SDL2 表示
wlrd-view: src/wlrd-view.c src/proto.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ src/wlrd-view.c $(SDL_LIBS)

# Stage 0: コンポジタのグローバル列挙
wlrd-globals: tools/globals.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf gen wlrd-capture wlrd-bench wlrd-send wlrd-view wlrd-globals

.PHONY: all clean

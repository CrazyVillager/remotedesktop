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

all: wlrd-capture wlrd-bench wlrd-globals

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
wlrd-capture: src/wlrd-capture.c gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
	$(CC) $(CFLAGS) -o $@ src/wlrd-capture.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 2: 連続キャプチャの FPS / damage 測定
wlrd-bench: src/wlrd-bench.c gen/wlr-screencopy-unstable-v1.c gen/wlr-screencopy-unstable-v1.h
	$(CC) $(CFLAGS) -o $@ src/wlrd-bench.c gen/wlr-screencopy-unstable-v1.c $(LDLIBS)

# Stage 0: コンポジタのグローバル列挙
wlrd-globals: tools/globals.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf gen wlrd-capture wlrd-bench wlrd-globals

.PHONY: all clean

/*
 * globals.c — Wayland コンポジタが公開するグローバルオブジェクトを列挙する
 *
 * Wayland ではクライアントは wl_registry を通じて、コンポジタが提供する
 * インターフェース（グローバル）を発見する。リモートデスクトップ開発で
 * 必要になるプロトコル（screencopy, virtual-pointer 等）が実際に
 * 利用可能かを、開発前にこのツールで確認する。
 *
 * ビルド: gcc globals.c -o globals $(pkg-config --cflags --libs wayland-client)
 */
#include <stdio.h>
#include <wayland-client.h>

/* registry にグローバルが現れるたびに呼ばれるコールバック */
static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface, uint32_t version)
{
    (void)data; (void)registry; (void)name;
    printf("%-55s v%u\n", interface, version);
}

/* グローバル削除通知（今回は列挙のみなので何もしない） */
static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global        = handle_global,
    .global_remove = handle_global_remove,
};

int main(void)
{
    /* $WAYLAND_DISPLAY のソケットに接続 */
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "コンポジタへの接続に失敗\n");
        return 1;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);

    /* roundtrip: 送信済みリクエストへの応答を全て受け取るまで待つ。
     * これで global イベントが一通り届く。 */
    wl_display_roundtrip(display);

    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    return 0;
}

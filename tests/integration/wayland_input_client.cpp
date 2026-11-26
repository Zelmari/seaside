#include "input-method-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"

#include <linux/input-event-codes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *shell;
    struct xdg_toplevel_icon_manager_v1 *icons;
    struct zwp_text_input_manager_v3 *text_manager;
    struct zwp_input_method_manager_v2 *method_manager;
    struct zwp_virtual_keyboard_manager_v1 *virtual_manager;
    struct zwp_text_input_v3 *text, *secondary;
    struct zwp_input_method_v2 *method;
    struct zwp_input_popup_surface_v2 *method_popup;
    struct zwp_input_method_keyboard_grab_v2 *grab;
    struct zwp_virtual_keyboard_v1 *forwarder;
    struct wl_surface *surface, *popup_surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct wl_buffer *content, *icon, *popup;
    unsigned enters, leaves, activations, deactivations, method_serial, text_serial;
    unsigned keys, grabbed_keys;
    bool mapped, closed, unavailable, previewed, committed, deleted, popup_rectangle, keymap;
    bool surrounds, private_surrounds, leaked_private_surrounds, stale_commit, cleared_preedit;
};

static struct wl_buffer *make_buffer(struct client *client, int width, int height,
                                     uint32_t pixel) noexcept {
    const int size = width * height * 4;
    const int fd = memfd_create("seaside-wayland-input-test", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return NULL;
    }
    auto *image =
        static_cast<uint32_t *>(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (image == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int i = 0; i < width * height; ++i) {
        image[i] = pixel;
    }
    munmap(image, size);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
    close(fd);
    struct wl_buffer *buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    return buffer;
}

static void configure(void *data, struct xdg_surface *surface, uint32_t serial) noexcept {
    auto *client = static_cast<struct client *>(data);
    xdg_surface_ack_configure(surface, serial);
    if (!client->mapped) {
        wl_surface_attach(client->surface, client->content, 0, 0);
        wl_surface_damage(client->surface, 0, 0, 120, 72);
        wl_surface_commit(client->surface);
        client->mapped = true;
    }
}

static void toplevel_configure(void *data, struct xdg_toplevel *role, int32_t width, int32_t height,
                               struct wl_array *states) noexcept {
    (void)data;
    (void)role;
    (void)width;
    (void)height;
    (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *role) noexcept {
    (void)role;
    ((struct client *)data)->closed = true;
}

static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) noexcept {
    (void)data;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener shell_listener = {.ping = shell_ping};
static const struct xdg_surface_listener surface_listener = {.configure = configure};
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = nullptr,
    .wm_capabilities = nullptr,
};

static void keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd,
                   uint32_t size) noexcept {
    (void)data;
    (void)keyboard;
    (void)format;
    (void)size;
    close(fd);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys) noexcept {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface) noexcept {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state) noexcept {
    (void)keyboard;
    (void)serial;
    (void)time;
    (void)state;
    if (key == KEY_A) {
        ++((struct client *)data)->keys;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group) noexcept {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)depressed;
    (void)latched;
    (void)locked;
    (void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = nullptr,
};

static void grab_keymap(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t format,
                        int fd, uint32_t size) noexcept {
    (void)grab;
    auto *client = static_cast<struct client *>(data);
    if (!client->keymap && format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size) {
        zwp_virtual_keyboard_v1_keymap(client->forwarder, format, fd, size);
        client->keymap = true;
    }
    close(fd);
}

static void grab_key(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, uint32_t serial,
                     uint32_t time, uint32_t key, uint32_t state) noexcept {
    (void)grab;
    (void)serial;
    auto *client = static_cast<struct client *>(data);
    if (client->keymap) {
        zwp_virtual_keyboard_v1_key(client->forwarder, time, key, state);
        ++client->grabbed_keys;
        printf("physical grab key %u state %u\n", key, state);
    }
}

static void grab_modifiers(void *data, struct zwp_input_method_keyboard_grab_v2 *grab,
                           uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked,
                           uint32_t group) noexcept {
    (void)grab;
    (void)serial;
    auto *client = static_cast<struct client *>(data);
    if (client->keymap) {
        zwp_virtual_keyboard_v1_modifiers(client->forwarder, depressed, latched, locked, group);
    }
}

static void grab_repeat(void *data, struct zwp_input_method_keyboard_grab_v2 *grab, int32_t rate,
                        int32_t delay) noexcept {
    (void)data;
    (void)grab;
    (void)rate;
    (void)delay;
}

static const struct zwp_input_method_keyboard_grab_v2_listener grab_listener = {
    .keymap = grab_keymap,
    .key = grab_key,
    .modifiers = grab_modifiers,
    .repeat_info = grab_repeat,
};

static void text_enter(void *data, struct zwp_text_input_v3 *input,
                       struct wl_surface *surface) noexcept {
    (void)input;
    auto *client = static_cast<struct client *>(data);
    if (surface == client->surface) {
        ++client->enters;
    }
}

static void text_leave(void *data, struct zwp_text_input_v3 *input,
                       struct wl_surface *surface) noexcept {
    (void)input;
    auto *client = static_cast<struct client *>(data);
    if (surface == client->surface) {
        ++client->leaves;
        puts("text input focus left");
    }
}

static void text_preedit(void *data, struct zwp_text_input_v3 *input, const char *text,
                         int32_t begin, int32_t end) noexcept {
    auto *client = static_cast<struct client *>(data);
    client->cleared_preedit |= input == client->text && !text;
    client->previewed |=
        input == client->text && text && strcmp(text, "é") == 0 && begin == 2 && end == 2;
}

static void text_commit(void *data, struct zwp_text_input_v3 *input, const char *text) noexcept {
    auto *client = static_cast<struct client *>(data);
    client->stale_commit |= text && strcmp(text, "stale") == 0;
    client->committed |= input == client->text && text && strcmp(text, "界") == 0;
}

static void text_delete(void *data, struct zwp_text_input_v3 *input, uint32_t before,
                        uint32_t after) noexcept {
    auto *client = static_cast<struct client *>(data);
    client->deleted |= input == client->text && before == 1 && after == 0;
}

static void text_done(void *data, struct zwp_text_input_v3 *input, uint32_t serial) noexcept {
    auto *client = static_cast<struct client *>(data);
    if (input == client->text) {
        client->text_serial = serial;
    }
}

static const struct zwp_text_input_v3_listener text_listener = {
    .enter = text_enter,
    .leave = text_leave,
    .preedit_string = text_preedit,
    .commit_string = text_commit,
    .delete_surrounding_text = text_delete,
    .done = text_done,
    .action = nullptr,
    .language = nullptr,
    .preedit_hint = nullptr,
};

static void activate(void *data, struct zwp_input_method_v2 *method) noexcept {
    (void)method;
    ++((struct client *)data)->activations;
}

static void deactivate(void *data, struct zwp_input_method_v2 *method) noexcept {
    (void)method;
    ++((struct client *)data)->deactivations;
    puts("input method deactivated");
}

static void surrounding(void *data, struct zwp_input_method_v2 *method, const char *text,
                        uint32_t cursor, uint32_t anchor) noexcept {
    (void)method;
    auto *client = static_cast<struct client *>(data);
    client->surrounds |= strcmp(text, "abc") == 0 && cursor == 1 && anchor == 1;
    client->private_surrounds |= !*text && cursor == 0 && anchor == 0;
    client->leaked_private_surrounds |= strcmp(text, "private") == 0;
}

static void cause(void *data, struct zwp_input_method_v2 *method, uint32_t type) noexcept {
    (void)data;
    (void)method;
    (void)type;
}

static void content_type(void *data, struct zwp_input_method_v2 *method, uint32_t hint,
                         uint32_t purpose) noexcept {
    (void)data;
    (void)method;
    (void)hint;
    (void)purpose;
}

static void method_done(void *data, struct zwp_input_method_v2 *method) noexcept {
    (void)method;
    ++((struct client *)data)->method_serial;
}

static void unavailable(void *data, struct zwp_input_method_v2 *method) noexcept {
    (void)method;
    ((struct client *)data)->unavailable = true;
}

static const struct zwp_input_method_v2_listener method_listener = {
    .activate = activate,
    .deactivate = deactivate,
    .surrounding_text = surrounding,
    .text_change_cause = cause,
    .content_type = content_type,
    .done = method_done,
    .unavailable = unavailable,
};

static void popup_rectangle(void *data, struct zwp_input_popup_surface_v2 *popup, int32_t x,
                            int32_t y, int32_t width, int32_t height) noexcept {
    (void)popup;
    (void)x;
    (void)y;
    ((struct client *)data)->popup_rectangle |= width == 2 && height == 18;
}

static const struct zwp_input_popup_surface_v2_listener popup_listener = {
    .text_input_rectangle = popup_rectangle,
};

static void icon_size(void *data, struct xdg_toplevel_icon_manager_v1 *icons,
                      int32_t size) noexcept {
    (void)data;
    (void)icons;
    (void)size;
}

static void icon_sizes_done(void *data, struct xdg_toplevel_icon_manager_v1 *icons) noexcept {
    (void)data;
    (void)icons;
}

static const struct xdg_toplevel_icon_manager_v1_listener icon_listener = {
    .icon_size = icon_size,
    .done = icon_sizes_done,
};

static void registry_add(void *data, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version) noexcept {
    (void)version;
    auto *client = static_cast<struct client *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        client->seat =
            static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->shell =
            static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(client->shell, &shell_listener, client);
    } else if (strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0) {
        client->icons = static_cast<xdg_toplevel_icon_manager_v1 *>(
            wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1));
        xdg_toplevel_icon_manager_v1_add_listener(client->icons, &icon_listener, client);
    } else if (strcmp(interface, zwp_text_input_manager_v3_interface.name) == 0) {
        client->text_manager = static_cast<zwp_text_input_manager_v3 *>(
            wl_registry_bind(registry, name, &zwp_text_input_manager_v3_interface, 1));
    } else if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
        client->method_manager = static_cast<zwp_input_method_manager_v2 *>(
            wl_registry_bind(registry, name, &zwp_input_method_manager_v2_interface, 1));
    } else if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
        client->virtual_manager = static_cast<zwp_virtual_keyboard_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1));
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) noexcept {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_add,
    .global_remove = registry_remove,
};

static bool setup(struct client *client) noexcept {
    client->registry = wl_display_get_registry(client->display);
    if (!client->registry ||
        wl_registry_add_listener(client->registry, &registry_listener, client) < 0 ||
        wl_display_roundtrip(client->display) < 0 || !client->compositor || !client->shell ||
        !client->seat || !client->text_manager || !client->method_manager || !client->icons ||
        !client->virtual_manager || !client->shm) {
        return false;
    }
    client->keyboard = wl_seat_get_keyboard(client->seat);
    wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    client->content = make_buffer(client, 120, 72, 0xff1f5573);
    client->icon = make_buffer(client, 32, 32, 0xff35c57b);
    client->popup = make_buffer(client, 60, 24, 0xffd48840);
    if (!client->content || !client->icon || !client->popup) {
        return false;
    }
    client->text = zwp_text_input_manager_v3_get_text_input(client->text_manager, client->seat);
    client->secondary =
        zwp_text_input_manager_v3_get_text_input(client->text_manager, client->seat);
    zwp_text_input_v3_add_listener(client->text, &text_listener, client);
    zwp_text_input_v3_add_listener(client->secondary, &text_listener, client);
    client->forwarder = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        client->virtual_manager, client->seat);
    client->surface = wl_compositor_create_surface(client->compositor);
    client->xdg = xdg_wm_base_get_xdg_surface(client->shell, client->surface);
    xdg_surface_add_listener(client->xdg, &surface_listener, client);
    client->toplevel = xdg_surface_get_toplevel(client->xdg);
    xdg_toplevel_add_listener(client->toplevel, &toplevel_listener, client);
    xdg_toplevel_set_app_id(client->toplevel, "seaside-wayland-input-test");
    xdg_toplevel_set_title(client->toplevel, "Seaside Wayland input test");
    struct xdg_toplevel_icon_v1 *icon = xdg_toplevel_icon_manager_v1_create_icon(client->icons);
    xdg_toplevel_icon_v1_set_name(icon, "seaside-fixture");
    xdg_toplevel_icon_v1_add_buffer(icon, client->icon, 1);
    xdg_toplevel_icon_manager_v1_set_icon(client->icons, client->toplevel, icon);
    xdg_toplevel_icon_v1_destroy(icon);
    wl_surface_commit(client->surface);
    for (int i = 0; client->enters < 2 && i < 150; ++i) {
        if (wl_display_roundtrip(client->display) < 0) {
            return false;
        }
        usleep(20000);
    }
    if (client->enters != 2) {
        return false;
    }
    client->method =
        zwp_input_method_manager_v2_get_input_method(client->method_manager, client->seat);
    zwp_input_method_v2_add_listener(client->method, &method_listener, client);
    return wl_display_roundtrip(client->display) >= 0 && !client->unavailable;
}

static bool roundtrip(struct client *client) noexcept {
    return wl_display_roundtrip(client->display) >= 0;
}

static bool run(struct client *client) noexcept {
    zwp_text_input_v3_enable(client->text);
    zwp_text_input_v3_set_surrounding_text(client->text, "abc", 1, 1);
    zwp_text_input_v3_set_cursor_rectangle(client->text, 20, 16, 2, 18);
    zwp_text_input_v3_commit(client->text);
    zwp_text_input_v3_enable(client->secondary);
    zwp_text_input_v3_set_surrounding_text(client->secondary, "wrong text input", 1, 1);
    zwp_text_input_v3_commit(client->secondary);
    if (!roundtrip(client) || client->activations != 1 || client->method_serial != 1 ||
        !client->surrounds) {
        fputs("initial activation or duplicate text input failed\n", stderr);
        return false;
    }

    client->popup_surface = wl_compositor_create_surface(client->compositor);
    client->method_popup =
        zwp_input_method_v2_get_input_popup_surface(client->method, client->popup_surface);
    zwp_input_popup_surface_v2_add_listener(client->method_popup, &popup_listener, client);
    wl_surface_attach(client->popup_surface, client->popup, 0, 0);
    wl_surface_damage(client->popup_surface, 0, 0, 60, 24);
    wl_surface_commit(client->popup_surface);
    client->grab = zwp_input_method_v2_grab_keyboard(client->method);
    zwp_input_method_keyboard_grab_v2_add_listener(client->grab, &grab_listener, client);
    zwp_input_method_v2_commit_string(client->method, "stale");
    zwp_input_method_v2_commit(client->method, client->method_serial - 1);
    zwp_input_method_v2_set_preedit_string(client->method, "é", 2, 2);
    zwp_input_method_v2_commit_string(client->method, "界");
    zwp_input_method_v2_delete_surrounding_text(client->method, 1, 0);
    zwp_input_method_v2_commit(client->method, client->method_serial);
    if (!roundtrip(client) || !client->keymap || !client->popup_rectangle || !client->previewed ||
        !client->committed || !client->deleted || client->stale_commit ||
        client->text_serial != 1) {
        fputs("composition, popup placement, or serial handling failed\n", stderr);
        return false;
    }

    zwp_virtual_keyboard_v1_key(client->forwarder, 1, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(client->forwarder, 1, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    // Switch to a different editor while keeping the same surface and object.
    zwp_text_input_v3_enable(client->text);
    zwp_text_input_v3_set_content_type(client->text, 0, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD);
    zwp_text_input_v3_set_surrounding_text(client->text, "private", 4, 4);
    zwp_text_input_v3_commit(client->text);
    if (!roundtrip(client) || client->keys != 2 || !client->private_surrounds ||
        client->leaked_private_surrounds || client->grabbed_keys != 0) {
        fputs("virtual keyboard forwarding or private surrounding text failed\n", stderr);
        return false;
    }
    zwp_text_input_v3_disable(client->text);
    zwp_text_input_v3_commit(client->text);
    if (!roundtrip(client) || client->deactivations != 1) {
        fputs("text input disable failed\n", stderr);
        return false;
    }
    zwp_text_input_v3_enable(client->text);
    zwp_text_input_v3_set_surrounding_text(client->text, "abc", 1, 1);
    zwp_text_input_v3_set_cursor_rectangle(client->text, 20, 16, 2, 18);
    zwp_text_input_v3_commit(client->text);
    return roundtrip(client) && client->activations == 2;
}

static bool verify_focus(struct client *client) noexcept {
    while (client->enters < 4 || client->leaves < 2) {
        if (wl_display_dispatch(client->display) < 0) {
            return false;
        }
    }
    const unsigned before = client->method_serial;
    zwp_text_input_v3_commit(client->text);
    if (!roundtrip(client) || client->method_serial != before || client->deactivations != 2 ||
        client->keys != 4 || client->grabbed_keys != 2) {
        fputs("focus loss, physical grab, or focus return failed\n", stderr);
        return false;
    }
    zwp_text_input_v3_enable(client->text);
    zwp_text_input_v3_commit(client->text);
    if (!roundtrip(client) || client->activations != 3) {
        return false;
    }
    client->cleared_preedit = false;
    zwp_input_method_v2_destroy(client->method);
    client->method = nullptr;
    if (!roundtrip(client) || !client->cleared_preedit) {
        fputs("input method disconnect did not clear composition\n", stderr);
        return false;
    }
    client->method =
        zwp_input_method_manager_v2_get_input_method(client->method_manager, client->seat);
    zwp_input_method_v2_add_listener(client->method, &method_listener, client);
    return roundtrip(client) && client->activations == 4 && !client->unavailable;
}

static void disconnect(struct client &client) noexcept {
    // Free client-side proxies while leaving server resources to socket teardown.
    // The input method's first popup and grab may already be destroyed on the server.
    struct wl_proxy *proxies[] = {
        reinterpret_cast<wl_proxy *>(client.grab),
        reinterpret_cast<wl_proxy *>(client.method_popup),
        reinterpret_cast<wl_proxy *>(client.popup_surface),
        reinterpret_cast<wl_proxy *>(client.method),
        reinterpret_cast<wl_proxy *>(client.forwarder),
        reinterpret_cast<wl_proxy *>(client.secondary),
        reinterpret_cast<wl_proxy *>(client.text),
        reinterpret_cast<wl_proxy *>(client.toplevel),
        reinterpret_cast<wl_proxy *>(client.xdg),
        reinterpret_cast<wl_proxy *>(client.surface),
        reinterpret_cast<wl_proxy *>(client.popup),
        reinterpret_cast<wl_proxy *>(client.icon),
        reinterpret_cast<wl_proxy *>(client.content),
        reinterpret_cast<wl_proxy *>(client.keyboard),
        reinterpret_cast<wl_proxy *>(client.virtual_manager),
        reinterpret_cast<wl_proxy *>(client.method_manager),
        reinterpret_cast<wl_proxy *>(client.text_manager),
        reinterpret_cast<wl_proxy *>(client.icons),
        reinterpret_cast<wl_proxy *>(client.shell),
        reinterpret_cast<wl_proxy *>(client.seat),
        reinterpret_cast<wl_proxy *>(client.shm),
        reinterpret_cast<wl_proxy *>(client.compositor),
        reinterpret_cast<wl_proxy *>(client.registry),
    };
    for (auto *proxy : proxies) {
        if (proxy) {
            wl_proxy_destroy(proxy);
        }
    }
    wl_display_disconnect(client.display);
}

int main(int argc, char **argv) {
    const bool hold = argc == 2 && strcmp(argv[1], "--hold") == 0;
    const bool focus = argc == 2 && strcmp(argv[1], "--verify-focus") == 0;
    if (argc != (hold || focus ? 2 : 1)) {
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct client client{};
    client.display = wl_display_connect(nullptr);
    if (!client.display) {
        fputs("could not connect to the compositor\n", stderr);
        return EXIT_FAILURE;
    }
    bool passed = setup(&client);
    if (passed) {
        passed = run(&client);
    } else {
        fputs("required global or both text input focus events missing\n", stderr);
    }
    if (passed) {
        puts("Wayland input composition, popup, privacy, and key forwarding passed");
    }
    if (passed && focus) {
        passed = verify_focus(&client);
    }
    while (passed && hold && !client.closed && wl_display_dispatch(client.display) >= 0) {
    }
    disconnect(client);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

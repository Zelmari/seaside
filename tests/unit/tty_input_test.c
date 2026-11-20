#define _POSIX_C_SOURCE 200809L
#include "tty_input.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_compositor.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

struct observation {
    unsigned motions;
    unsigned exits;
    unsigned launches;
    unsigned configured_shortcuts;
    unsigned vt_switches;
    bool absolute;
    double x, y;
};

static void record_motion(void *data, bool absolute, double x, double y) {
    struct observation *observation = data;
    ++observation->motions;
    observation->absolute = absolute;
    observation->x = x;
    observation->y = y;
}

static void record_exit(void *data) {
    struct observation *observation = data;
    ++observation->exits;
}

static void record_launch(void *data) {
    struct observation *observation = data;
    ++observation->launches;
}

static bool record_shortcut(void *data, uint8_t modifiers, uint32_t keysym) {
    struct observation *observation = data;
    if (modifiers == 2 && keysym == XKB_KEY_Return) {
        ++observation->configured_shortcuts;
        return true;
    }
    if (keysym == XKB_KEY_XF86Switch_VT_2) {
        ++observation->vt_switches;
        return true;
    }
    return false;
}

static const struct wlr_backend_impl backend_impl = {0};
static const struct wlr_keyboard_impl keyboard_impl = {.name = "test"};
static const struct wlr_pointer_impl pointer_impl = {.name = "test"};
static const struct seaside_tty_input_callbacks callbacks = {
    .motion = record_motion,
    .exit_requested = record_exit,
};

static void exercise(bool backend_first) {
    struct wlr_backend backend = {0};
    wlr_backend_init(&backend, &backend_impl);
    struct observation observation = {0};
    struct seaside_tty_input *input = seaside_tty_input_create(&backend, &callbacks, &observation);
    check(input != NULL, "cannot create input bridge");

    struct wlr_keyboard keyboard = {0};
    wlr_keyboard_init(&keyboard, &keyboard_impl, "test keyboard");
    wl_signal_emit_mutable(&backend.events.new_input, &keyboard.base);
    struct wlr_pointer pointer = {0};
    wlr_pointer_init(&pointer, &pointer_impl, "test pointer");
    wl_signal_emit_mutable(&backend.events.new_input, &pointer.base);

    struct seaside_tty_input_snapshot snapshot = seaside_tty_input_get_snapshot(input);
    check(snapshot.pointers == 1 && snapshot.keyboards == 1 && !snapshot.failed,
          "input discovery did not register both devices");
    struct wlr_pointer_motion_event relative = {
        .pointer = &pointer,
        .delta_x = 10.5,
        .delta_y = -4,
        .unaccel_dx = 7,
        .unaccel_dy = -2,
    };
    wl_signal_emit_mutable(&pointer.events.motion, &relative);
    check(observation.motions == 1 && !observation.absolute && observation.x == 10.5 &&
              observation.y == -4,
          "relative pointer did not report the accelerated deltas");
    struct wlr_pointer_motion_absolute_event absolute = {.pointer = &pointer, .x = 0.25, .y = 0.75};
    wl_signal_emit_mutable(&pointer.events.motion_absolute, &absolute);
    check(observation.motions == 2 && observation.absolute && observation.x == 0.25 &&
              observation.y == 0.75,
          "absolute pointer did not report the normalized coordinates");

    struct wlr_keyboard_key_event event = {.keycode = KEY_ENTER,
                                           .state = WL_KEYBOARD_KEY_STATE_PRESSED};
    wlr_keyboard_notify_key(&keyboard, &event);
    event.keycode = KEY_ESC;
    event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(&keyboard, &event);
    check(observation.exits == 0, "non-Escape press or Escape release requested shutdown");
    event.state = WL_KEYBOARD_KEY_STATE_PRESSED;
    wlr_keyboard_notify_key(&keyboard, &event);
    check(observation.exits == 1, "Escape press did not request shutdown");

    wlr_keyboard_finish(&keyboard);
    snapshot = seaside_tty_input_get_snapshot(input);
    check(snapshot.pointers == 1 && snapshot.keyboards == 0 && !snapshot.failed,
          "keyboard removal did not detach the input device");

    /* Leave the pointer alive to test both owner destruction orders. */
    if (backend_first) {
        wlr_backend_finish(&backend);
        snapshot = seaside_tty_input_get_snapshot(input);
        check(snapshot.pointers == 0 && snapshot.keyboards == 0 && !snapshot.failed,
              "backend destruction did not detach surviving input devices");
        wl_signal_emit_mutable(&pointer.events.motion, &relative);
        check(observation.motions == 2, "pointer still dispatched after backend destruction");
        seaside_tty_input_destroy(input);
    } else {
        seaside_tty_input_destroy(input);
        wl_signal_emit_mutable(&pointer.events.motion_absolute, &absolute);
        check(observation.motions == 2, "pointer still dispatched after bridge destruction");
        wlr_backend_finish(&backend);
    }
    wlr_pointer_finish(&pointer);
}

struct keyboard_client {
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    uint32_t capabilities;
    unsigned keymaps;
    unsigned repeat_info;
    unsigned enters;
    unsigned key_events;
    uint32_t key;
    uint32_t key_state;
    uint32_t depressed;
    bool data_device_manager;
    bool escaped;
    bool entered_with_return;
};

static void client_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int fd,
                          uint32_t size) {
    (void)keyboard;
    struct keyboard_client *client = data;
    check(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0, "missing XKB keymap");
    close(fd);
    ++client->keymaps;
}

static void client_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         struct wl_surface *surface, struct wl_array *keys) {
    (void)keyboard;
    (void)serial;
    (void)surface;
    struct keyboard_client *client = data;
    ++client->enters;
    const uint32_t *key;
    wl_array_for_each(key, keys) {
        if (*key == KEY_ENTER) {
            client->entered_with_return = true;
        }
    }
}

static void client_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         struct wl_surface *surface) {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void client_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                       uint32_t key, uint32_t state) {
    (void)keyboard;
    (void)serial;
    (void)time;
    struct keyboard_client *client = data;
    ++client->key_events;
    client->key = key;
    client->key_state = state;
    if (key == KEY_ESC && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        client->escaped = true;
    }
}

static void client_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                             uint32_t depressed, uint32_t latched, uint32_t locked,
                             uint32_t group) {
    (void)keyboard;
    (void)serial;
    (void)latched;
    (void)locked;
    (void)group;
    struct keyboard_client *client = data;
    client->depressed = depressed;
}

static void client_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay) {
    (void)keyboard;
    struct keyboard_client *client = data;
    check(rate > 0 && delay > 0, "missing keyboard repeat parameters");
    ++client->repeat_info;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = client_keymap,
    .enter = client_enter,
    .leave = client_leave,
    .key = client_key,
    .modifiers = client_modifiers,
    .repeat_info = client_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    struct keyboard_client *client = data;
    client->capabilities = capabilities;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !client->keyboard) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data;
    (void)seat;
    check(strcmp(name, "seat0") == 0, "unexpected seat name");
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct keyboard_client *client = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        client->seat = wl_registry_bind(registry, name, &wl_seat_interface, 5);
        wl_seat_add_listener(client->seat, &seat_listener, client);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        client->data_device_manager = version >= 3;
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void mark_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}

static void round_trip(struct wl_display *server, struct wl_display *client) {
    static const struct wl_callback_listener listener = {.done = mark_done};
    bool done = false;
    struct wl_callback *sync = wl_display_sync(client);
    wl_callback_add_listener(sync, &listener, &done);
    check(wl_display_flush(client) >= 0, "flush failed");
    check(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) == 0, "dispatch failed");
    wl_display_flush_clients(server);
    while (!done) {
        check(wl_display_dispatch(client) >= 0, "client dispatch failed");
    }
}

static void key(struct wlr_keyboard *keyboard, uint32_t keycode, enum wl_keyboard_key_state state) {
    struct wlr_keyboard_key_event event = {
        .keycode = keycode,
        .state = state,
        .update_state = true,
    };
    wlr_keyboard_notify_key(keyboard, &event);
}

static void exercise_wayland_keyboard(void) {
    check(setenv("XKB_DEFAULT_RULES", "evdev", 1) == 0 &&
              setenv("XKB_DEFAULT_MODEL", "pc105", 1) == 0 &&
              setenv("XKB_DEFAULT_LAYOUT", "us", 1) == 0 &&
              setenv("XKB_DEFAULT_VARIANT", "", 1) == 0 &&
              setenv("XKB_DEFAULT_OPTIONS", "", 1) == 0,
          "could not isolate test keymap from the user's XKB choices");
    struct wl_display *server = wl_display_create();
    check(server && wlr_compositor_create(server, 6, NULL), "Wayland compositor setup failed");
    struct wlr_backend backend = {0};
    wlr_backend_init(&backend, &backend_impl);
    struct observation observation = {0};
    const struct seaside_tty_input_callbacks terminal_callbacks = {
        .launch_requested = record_launch,
        .shortcut_pressed = record_shortcut,
    };
    struct seaside_tty_input *input =
        seaside_tty_input_create(&backend, &terminal_callbacks, &observation);
    check(input && seaside_tty_input_enable_keyboard(input, server), "keyboard seat setup failed");
    struct wlr_keyboard first = {0}, second = {0};
    wlr_keyboard_init(&first, &keyboard_impl, "keyboard one");
    wlr_keyboard_init(&second, &keyboard_impl, "keyboard two");
    wl_signal_emit_mutable(&backend.events.new_input, &first.base);
    wl_signal_emit_mutable(&backend.events.new_input, &second.base);
    check(seaside_tty_input_get_snapshot(input).keyboards == 2, "keyboard group missing a device");

    int ends[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) == 0, "socketpair failed");
    struct wl_client *peer = wl_client_create(server, ends[0]);
    struct wl_display *client = wl_display_connect_to_fd(ends[1]);
    check(peer && client, "client connect failed");
    struct keyboard_client keyboard_client = {0};
    struct wl_registry *registry = wl_display_get_registry(client);
    wl_registry_add_listener(registry, &registry_listener, &keyboard_client);
    round_trip(server, client);
    round_trip(server, client);
    check(keyboard_client.compositor && keyboard_client.seat && keyboard_client.keyboard &&
              keyboard_client.data_device_manager &&
              keyboard_client.capabilities == WL_SEAT_CAPABILITY_KEYBOARD,
          "missing required globals or advertising unimplemented input capabilities");
    struct wl_surface *surface = wl_compositor_create_surface(keyboard_client.compositor);
    round_trip(server, client);
    check(keyboard_client.enters == 0 && keyboard_client.keymaps > 0 &&
              keyboard_client.repeat_info > 0,
          "keyboard did not receive its keymap and repeat info before focus");
    struct wl_resource *resource =
        wl_client_get_object(peer, wl_proxy_get_id((struct wl_proxy *)surface));
    check(resource != NULL, "missing Wayland surface resource");
    struct wlr_surface *server_surface = wlr_surface_from_resource(resource);

    key(&first, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&first, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    check(observation.launches == 1, "grouped Super+Return did not launch exactly once");
    seaside_tty_input_set_focus(input, server_surface);
    round_trip(server, client);
    check(keyboard_client.enters == 1 && keyboard_client.depressed != 0 &&
              !keyboard_client.entered_with_return,
          "focus lost its held modifier or leaked a consumed shortcut");

    struct wl_keyboard *late_keyboard = wl_seat_get_keyboard(keyboard_client.seat);
    wl_keyboard_add_listener(late_keyboard, &keyboard_listener, &keyboard_client);
    round_trip(server, client);
    check(keyboard_client.enters == 2 && !keyboard_client.entered_with_return,
          "late keyboard binding leaked held Return");
    wl_keyboard_release(late_keyboard);
    key(&first, KEY_ENTER, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&first, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&first, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED);
    check(observation.launches == 1, "shortcut repeated or matched Super+Shift+Return");
    wlr_keyboard_finish(&first);
    round_trip(server, client);
    check(keyboard_client.depressed == 0, "unplugging keyboard left Super stuck");
    check(keyboard_client.key_events == 5 && keyboard_client.key == KEY_LEFTMETA &&
              keyboard_client.key_state == WL_KEYBOARD_KEY_STATE_RELEASED,
          "shortcut release leaked or a regular key event was lost");

    key(&second, KEY_ESC, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ESC, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client);
    const struct seaside_tty_input_snapshot received = seaside_tty_input_get_snapshot(input);
    check(received.has_keyboard_focus && received.forwarded_key_events == 7 &&
              received.forwarded_key_presses == 3 && received.launch_requests == 1 &&
              observation.exits == 0 && keyboard_client.escaped,
          "terminal Escape or keyboard delivery counters failed");
    wl_surface_destroy(surface);
    round_trip(server, client);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    const struct seaside_tty_input_snapshot unfocused = seaside_tty_input_get_snapshot(input);
    check(!unfocused.has_keyboard_focus &&
              unfocused.forwarded_key_events == received.forwarded_key_events,
          "surface destruction left stale keyboard focus");

    key(&second, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_F2, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_F2, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_LEFTALT, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_LEFTCTRL, WL_KEYBOARD_KEY_STATE_RELEASED);
    check(observation.configured_shortcuts == 1 && observation.vt_switches == 1,
          "configured Ctrl+Return or standard VT keysym did not reach the compositor");

    wl_keyboard_release(keyboard_client.keyboard);
    wl_seat_release(keyboard_client.seat);
    wl_compositor_destroy(keyboard_client.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(client);
    wl_display_destroy_clients(server);
    wl_display_destroy(server);
    wlr_backend_finish(&backend);
    seaside_tty_input_destroy(input);
    wlr_keyboard_finish(&second);
}

int main(void) {
    exercise(true);
    exercise(false);
    exercise_wayland_keyboard();
    return EXIT_SUCCESS;
}

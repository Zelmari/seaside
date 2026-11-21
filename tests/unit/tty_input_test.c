#define _GNU_SOURCE
#include "tty_input.h"

#include "pointer-constraints-client-protocol.h"
#include "primary-selection-client-protocol.h"
#include "relative-pointer-client-protocol.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_shm.h>
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
    unsigned digit_shortcuts;
    unsigned explicit_symbol_shortcuts;
    unsigned shadowed_digit_shortcuts;
    unsigned alternate_case_shortcuts;
    unsigned exact_case_shortcuts;
    unsigned shadowed_case_shortcuts;
    unsigned modifier_changes;
    uint8_t modifiers;
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
    if (modifiers == 1) {
        switch (keysym) {
        case XKB_KEY_D:
            ++observation->alternate_case_shortcuts;
            return true;
        case XKB_KEY_e:
            ++observation->exact_case_shortcuts;
            return true;
        case XKB_KEY_E:
            ++observation->shadowed_case_shortcuts;
            return true;
        }
    }
    if (modifiers == (1 | 8)) {
        switch (keysym) {
        case XKB_KEY_2:
            ++observation->digit_shortcuts;
            return true;
        case XKB_KEY_dollar:
            ++observation->explicit_symbol_shortcuts;
            return true;
        case XKB_KEY_4:
            ++observation->shadowed_digit_shortcuts;
            return true;
        }
    }
    return false;
}

static void record_modifiers(void *data, uint8_t modifiers) {
    struct observation *observation = data;
    ++observation->modifier_changes;
    observation->modifiers = modifiers;
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
    struct wl_pointer *pointer;
    struct wl_shm *shm;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative_pointer_manager;
    uint32_t capabilities;
    unsigned keymaps;
    unsigned repeat_info;
    unsigned enters;
    unsigned key_events;
    uint32_t key;
    uint32_t key_state;
    uint32_t key_serial, button_serial;
    uint32_t depressed;
    bool data_device_manager;
    uint32_t data_manager_name, primary_manager_name;
    bool data_control;
    bool escaped;
    bool entered_with_return;
    unsigned pointer_enters;
    unsigned pointer_leaves;
    unsigned pointer_motions;
    unsigned pointer_buttons;
    unsigned pointer_axes;
    unsigned pointer_frames;
    unsigned relative_motions;
    unsigned locks;
    unsigned unlocks;
    unsigned confines;
    unsigned unconfines;
    double relative_dx, unaccelerated_dx;
    double pointer_x, pointer_y;
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
    (void)time;
    struct keyboard_client *client = data;
    ++client->key_events;
    client->key = key;
    client->key_state = state;
    client->key_serial = serial;
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

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer;
    (void)serial;
    (void)surface;
    struct keyboard_client *client = data;
    ++client->pointer_enters;
    client->pointer_x = wl_fixed_to_double(sx);
    client->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) {
    (void)pointer;
    (void)serial;
    (void)surface;
    ++((struct keyboard_client *)data)->pointer_leaves;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx,
                           wl_fixed_t sy) {
    (void)pointer;
    (void)time;
    struct keyboard_client *client = data;
    ++client->pointer_motions;
    client->pointer_x = wl_fixed_to_double(sx);
    client->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
    (void)pointer;
    (void)time;
    (void)button;
    struct keyboard_client *client = data;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        client->button_serial = serial;
    }
    ++client->pointer_buttons;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
    (void)pointer;
    (void)time;
    check(axis == WL_POINTER_AXIS_VERTICAL_SCROLL && wl_fixed_to_double(value) == 3,
          "axis event was changed in the input bridge");
    ++((struct keyboard_client *)data)->pointer_axes;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)pointer;
    ++((struct keyboard_client *)data)->pointer_frames;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
    (void)data;
    (void)pointer;
    check(source == WL_POINTER_AXIS_SOURCE_FINGER, "wrong axis source");
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis) {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    check(false, "unexpected axis stop");
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete) {
    (void)data;
    (void)pointer;
    (void)axis;
    (void)discrete;
    check(false, "unexpected discrete axis event for finger source");
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    struct keyboard_client *client = data;
    client->capabilities = capabilities;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !client->keyboard) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER && !client->pointer) {
        client->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
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

static void shm_format(void *data, struct wl_shm *shm, uint32_t format) {
    (void)data;
    (void)shm;
    (void)format;
}

static const struct wl_shm_listener shm_listener = {.format = shm_format};

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
        client->data_manager_name = name;
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        client->primary_manager_name = name;
    } else if (strcmp(interface, "ext_data_control_manager_v1") == 0 ||
               strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        client->data_control = true;
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(client->shm, &shm_listener, client);
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        client->constraints =
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1);
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        client->relative_pointer_manager =
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1);
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
        .modifiers_changed = record_modifiers,
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

    key(&second, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_D, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_D, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_E, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_E, WL_KEYBOARD_KEY_STATE_RELEASED);
    check(observation.alternate_case_shortcuts == 1 && observation.exact_case_shortcuts == 1 &&
              observation.shadowed_case_shortcuts == 0,
          "an unshifted letter did not dispatch its case alias or lost exact case precedence");
    key(&second, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_4, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_4, WL_KEYBOARD_KEY_STATE_RELEASED);
    key(&second, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client);
    check(observation.digit_shortcuts == 1 && observation.explicit_symbol_shortcuts == 1 &&
              observation.shadowed_digit_shortcuts == 0 && keyboard_client.key_events == 10,
          "Super+Shift+digit failed to dispatch or shadowed the effective symbol binding");
    key(&second, KEY_2, WL_KEYBOARD_KEY_STATE_PRESSED);
    round_trip(server, client);
    check(keyboard_client.key_events == 11 && keyboard_client.key == KEY_2 &&
              keyboard_client.key_state == WL_KEYBOARD_KEY_STATE_PRESSED,
          "the compositor consumed an unbound Shift+digit press");
    key(&second, KEY_2, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client);
    check(keyboard_client.key_events == 12 && keyboard_client.key == KEY_2 &&
              keyboard_client.key_state == WL_KEYBOARD_KEY_STATE_RELEASED,
          "the compositor consumed an unbound Shift+digit release");
    key(&second, KEY_LEFTSHIFT, WL_KEYBOARD_KEY_STATE_RELEASED);
    const unsigned previous_modifier_changes = observation.modifier_changes;
    const uint64_t previous_forwarded = seaside_tty_input_get_snapshot(input).forwarded_key_events;

    wl_surface_destroy(surface);
    round_trip(server, client);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    const struct seaside_tty_input_snapshot unfocused = seaside_tty_input_get_snapshot(input);
    check(!unfocused.has_keyboard_focus && unfocused.forwarded_key_events == previous_forwarded,
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
    check(previous_modifier_changes == 8 && observation.modifier_changes == 12 &&
              observation.modifiers == 0,
          "compositor modifier tracking missed release or unplug transitions");

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

struct pointer_scene {
    struct seaside_tty_input *input;
    struct wlr_surface *surfaces[2];
    double x, y;
    unsigned move_calls, clicks, consumed_releases, cancelled;
    unsigned drag_icons;
    bool locked;
};

static void scene_motion(void *data, bool absolute, double x, double y) {
    struct pointer_scene *scene = data;
    ++scene->move_calls;
    scene->x = absolute ? 200 * x : scene->x + x;
    scene->y = absolute ? 100 * y : scene->y + y;
}

static bool scene_absolute_delta(void *data, double x, double y, double *dx, double *dy) {
    const struct pointer_scene *scene = data;
    *dx = 200 * x - scene->x;
    *dy = 100 * y - scene->y;
    return true;
}

static bool scene_target(void *data, struct seaside_tty_pointer_target *target) {
    const struct pointer_scene *scene = data;
    size_t slot;
    if (target->surface) {
        slot = target->surface == scene->surfaces[0] ? 0 : 1;
        if (target->surface != scene->surfaces[slot]) {
            return false;
        }
    } else {
        if (scene->x < 0 || scene->x >= 200 || scene->y < 0 || scene->y >= 100) {
            return false;
        }
        slot = scene->x < 100 ? 0 : 1;
    }
    target->surface = scene->surfaces[slot];
    target->window_id = slot + 1;
    target->sx = scene->x - (slot == 0 ? 0.0 : 100.0);
    target->sy = scene->y;
    return target->surface != NULL;
}

static bool scene_button(void *data, uint64_t window, uint32_t button, bool pressed,
                         uint8_t modifiers) {
    struct pointer_scene *scene = data;
    if (button == BTN_RIGHT && modifiers == 1) {
        if (!pressed) {
            ++scene->consumed_releases;
        }
        return true;
    }
    if (pressed && window > 0 && window <= 2) {
        ++scene->clicks;
        seaside_tty_input_set_focus(scene->input, scene->surfaces[window - 1]);
    }
    return false;
}

static void scene_locked(void *data, bool locked) {
    ((struct pointer_scene *)data)->locked = locked;
}

static void scene_cancelled(void *data) {
    ++((struct pointer_scene *)data)->cancelled;
}

static void scene_drag_icon(void *data, struct wlr_drag_icon *icon) {
    check(icon && icon->surface, "missing requested drag icon");
    ++((struct pointer_scene *)data)->drag_icons;
}

struct pointer_client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_surface *surface;
    struct wl_buffer *buffer;
    struct zwp_relative_pointer_v1 *relative;
    struct keyboard_client events;
};

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *relative, uint32_t hi,
                            uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t unaccel_dx,
                            wl_fixed_t unaccel_dy) {
    (void)relative;
    (void)hi;
    (void)lo;
    (void)dy;
    (void)unaccel_dy;
    struct keyboard_client *client = data;
    ++client->relative_motions;
    client->relative_dx = wl_fixed_to_double(dx);
    client->unaccelerated_dx = wl_fixed_to_double(unaccel_dx);
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {
    .relative_motion = relative_motion,
};

static void pointer_locked(void *data, struct zwp_locked_pointer_v1 *lock) {
    (void)lock;
    ++((struct keyboard_client *)data)->locks;
}

static void pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *lock) {
    (void)lock;
    ++((struct keyboard_client *)data)->unlocks;
}

static const struct zwp_locked_pointer_v1_listener lock_listener = {
    .locked = pointer_locked,
    .unlocked = pointer_unlocked,
};

static void pointer_confined(void *data, struct zwp_confined_pointer_v1 *confine) {
    (void)confine;
    ++((struct keyboard_client *)data)->confines;
}

static void pointer_unconfined(void *data, struct zwp_confined_pointer_v1 *confine) {
    (void)confine;
    ++((struct keyboard_client *)data)->unconfines;
}

static const struct zwp_confined_pointer_v1_listener confine_listener = {
    .confined = pointer_confined,
    .unconfined = pointer_unconfined,
};

static struct wlr_surface *connect_pointer_client(struct wl_display *server,
                                                  struct pointer_client *client) {
    int ends[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) == 0, "socketpair failed");
    struct wl_client *peer = wl_client_create(server, ends[0]);
    client->display = wl_display_connect_to_fd(ends[1]);
    check(peer && client->display, "pointer client connect failed");
    client->registry = wl_display_get_registry(client->display);
    wl_registry_add_listener(client->registry, &registry_listener, &client->events);
    round_trip(server, client->display);
    round_trip(server, client->display);
    struct keyboard_client *events = &client->events;
    check(events->compositor && events->shm && events->keyboard && events->pointer &&
              events->constraints && events->relative_pointer_manager &&
              events->capabilities == (WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER),
          "seat did not advertise pointer capability and protocols");

    const int side = 100, stride = side * 4;
    int fd = memfd_create("seaside-pointer-test", MFD_CLOEXEC);
    check(fd >= 0 && ftruncate(fd, side * stride) == 0, "could not allocate pointer test buffer");
    struct wl_shm_pool *pool = wl_shm_create_pool(events->shm, fd, side * stride);
    client->buffer = wl_shm_pool_create_buffer(pool, 0, side, side, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    client->surface = wl_compositor_create_surface(events->compositor);
    wl_surface_attach(client->surface, client->buffer, 0, 0);
    wl_surface_commit(client->surface);
    client->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(
        events->relative_pointer_manager, events->pointer);
    zwp_relative_pointer_v1_add_listener(client->relative, &relative_listener, events);
    round_trip(server, client->display);
    struct wl_resource *resource =
        wl_client_get_object(peer, wl_proxy_get_id((struct wl_proxy *)client->surface));
    check(resource != NULL, "missing pointer test surface resource");
    return wlr_surface_from_resource(resource);
}

static void disconnect_pointer_client(struct wl_display *server, struct pointer_client *client) {
    if (client->surface) {
        wl_surface_destroy(client->surface);
    }
    wl_buffer_destroy(client->buffer);
    zwp_relative_pointer_v1_destroy(client->relative);
    zwp_relative_pointer_manager_v1_destroy(client->events.relative_pointer_manager);
    zwp_pointer_constraints_v1_destroy(client->events.constraints);
    wl_pointer_release(client->events.pointer);
    wl_keyboard_release(client->events.keyboard);
    wl_seat_release(client->events.seat);
    wl_shm_destroy(client->events.shm);
    wl_compositor_destroy(client->events.compositor);
    wl_registry_destroy(client->registry);
    round_trip(server, client->display);
    wl_display_disconnect(client->display);
}

static void relative(struct wlr_pointer *pointer, double dx, double raw_dx) {
    struct wlr_pointer_motion_event motion = {
        .pointer = pointer, .time_msec = 11, .delta_x = dx, .unaccel_dx = raw_dx};
    wl_signal_emit_mutable(&pointer->events.motion, &motion);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

static void button(struct wlr_pointer *pointer, uint32_t code, bool pressed) {
    struct wlr_pointer_button_event event = {
        .pointer = pointer,
        .time_msec = 12,
        .button = code,
        .state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
    };
    wlr_pointer_notify_button(pointer, &event);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

static void axis(struct wlr_pointer *pointer) {
    struct wlr_pointer_axis_event scroll = {
        .pointer = pointer,
        .time_msec = 13,
        .source = WL_POINTER_AXIS_SOURCE_FINGER,
        .orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
        .delta = 3,
    };
    wl_signal_emit_mutable(&pointer->events.axis, &scroll);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

static void create_pointer_seat(struct wl_display *server, struct wlr_backend *backend,
                                struct wlr_keyboard *keyboard, struct wlr_pointer *pointer,
                                struct pointer_scene *scene) {
    const uint32_t formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
    check(server && wlr_compositor_create(server, 6, NULL) &&
              wlr_shm_create(server, 1, formats, sizeof(formats) / sizeof(formats[0])),
          "pointer test compositor setup failed");
    wlr_backend_init(backend, &backend_impl);
    wlr_keyboard_init(keyboard, &keyboard_impl, "pointer test keyboard");
    wlr_pointer_init(pointer, &pointer_impl, "pointer test pointer");
    const struct seaside_tty_input_callbacks seat_callbacks = {
        .motion = scene_motion,
        .pointer_target = scene_target,
        .pointer_button = scene_button,
        .pointer_locked = scene_locked,
        .pointer_absolute_delta = scene_absolute_delta,
        .pointer_cancelled = scene_cancelled,
        .drag_icon = scene_drag_icon,
    };
    scene->input = seaside_tty_input_create(backend, &seat_callbacks, scene);
    check(scene->input && seaside_tty_input_enable_keyboard(scene->input, server) &&
              seaside_tty_input_get_seat(scene->input),
          "pointer test seat setup failed");
    wl_signal_emit_mutable(&backend->events.new_input, &keyboard->base);
    wl_signal_emit_mutable(&backend->events.new_input, &pointer->base);
}

static void exercise_wayland_pointer(void) {
    struct wl_display *server = wl_display_create();
    struct wlr_backend backend = {0};
    struct wlr_keyboard keyboard = {0};
    struct wlr_pointer pointer = {0};
    struct pointer_scene scene = {.x = 10, .y = 10};
    create_pointer_seat(server, &backend, &keyboard, &pointer, &scene);

    struct pointer_client left = {0}, right = {0};
    scene.surfaces[0] = connect_pointer_client(server, &left);
    scene.surfaces[1] = connect_pointer_client(server, &right);
    seaside_tty_input_set_focus(scene.input, scene.surfaces[0]);
    seaside_tty_input_refresh_pointer(scene.input);
    relative(&pointer, 100, 90);
    axis(&pointer);
    round_trip(server, left.display);
    round_trip(server, right.display);
    check(left.events.enters == 1 && left.events.pointer_enters == 1 &&
              left.events.pointer_leaves == 1 && left.events.pointer_axes == 0 &&
              right.events.enters == 0 && right.events.pointer_enters == 1 &&
              right.events.pointer_axes == 1 && right.events.pointer_frames > 0 &&
              right.events.pointer_x == 10 && right.events.pointer_y == 10,
          "hover or background scrolling changed keyboard focus or reached the wrong surface");

    struct zwp_locked_pointer_v1 *lock = zwp_pointer_constraints_v1_lock_pointer(
        right.events.constraints, right.surface, right.events.pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    zwp_locked_pointer_v1_add_listener(lock, &lock_listener, &right.events);
    round_trip(server, right.display);
    check(right.events.locks == 0 && !scene.locked, "background surface acquired a pointer lock");

    button(&pointer, BTN_LEFT, true);
    round_trip(server, right.display);
    check(right.events.enters == 1 && right.events.pointer_buttons == 1 && scene.clicks == 1 &&
              right.events.locks == 1 && scene.locked,
          "click did not focus the hover target and activate its pointer lock");
    const unsigned old_regular = right.events.pointer_motions;
    const unsigned old_relative = right.events.relative_motions;
    relative(&pointer, -125, -111);
    axis(&pointer);
    button(&pointer, BTN_LEFT, false);
    round_trip(server, right.display);
    check(scene.x == 110 && right.events.pointer_motions == old_regular &&
              right.events.relative_motions == old_relative + 1 &&
              right.events.relative_dx == -125 && right.events.unaccelerated_dx == -111 &&
              right.events.pointer_axes == 2 && right.events.pointer_buttons == 2,
          "pointer lock did not suppress motion or failed to preserve raw deltas/axis/buttons");
    seaside_tty_input_set_focus(scene.input, scene.surfaces[0]);
    round_trip(server, right.display);
    check(right.events.unlocks == 1 && !scene.locked,
          "pointer lock survived the focus change to another client");
    zwp_locked_pointer_v1_destroy(lock);

    seaside_tty_input_set_focus(scene.input, scene.surfaces[1]);
    lock = zwp_pointer_constraints_v1_lock_pointer(right.events.constraints, right.surface,
                                                   right.events.pointer, NULL,
                                                   ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    zwp_locked_pointer_v1_add_listener(lock, &lock_listener, &right.events);
    round_trip(server, right.display);
    check(right.events.locks == 2 && scene.locked, "second lock did not activate");
    struct wl_region *outside = wl_compositor_create_region(right.events.compositor);
    wl_region_add(outside, 50, 0, 50, 100);
    zwp_locked_pointer_v1_set_region(lock, outside);
    wl_region_destroy(outside);
    wl_surface_commit(right.surface);
    round_trip(server, right.display);
    check(right.events.unlocks == 2 && !scene.locked,
          "committing a new region failed to safely deactivate a one-shot lock");
    zwp_locked_pointer_v1_destroy(lock);

    // A normal client button grab stays with the original surface until release.
    button(&pointer, BTN_LEFT, true);
    relative(&pointer, -100, -100);
    round_trip(server, left.display);
    round_trip(server, right.display);
    check(left.events.pointer_enters == 1 && right.events.pointer_x == -90,
          "pointer changed surfaces while a client button was held");
    button(&pointer, BTN_LEFT, false);
    round_trip(server, left.display);
    round_trip(server, right.display);
    check(left.events.pointer_enters == 2 && right.events.pointer_buttons == 4,
          "the press and release were delivered to different surfaces");

    // A compositor gesture must not leak its press or release to either client.
    key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
    button(&pointer, BTN_RIGHT, true);
    relative(&pointer, 100, 100);
    button(&pointer, BTN_RIGHT, false);
    key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, left.display);
    round_trip(server, right.display);
    check(left.events.pointer_buttons == 0 && right.events.pointer_buttons == 4 &&
              scene.consumed_releases == 1 && right.events.pointer_enters == 2,
          "a consumed pointer gesture leaked a button or failed to restore hover");

    struct wl_region *region = wl_compositor_create_region(right.events.compositor);
    wl_region_add(region, 0, 0, 30, 100);
    struct zwp_confined_pointer_v1 *confine = zwp_pointer_constraints_v1_confine_pointer(
        right.events.constraints, right.surface, right.events.pointer, region,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_confined_pointer_v1_add_listener(confine, &confine_listener, &right.events);
    wl_region_destroy(region);
    round_trip(server, right.display);
    check(right.events.confines == 1, "eligible confinement did not activate");
    relative(&pointer, 80, 80);
    check(scene.x > 125 && scene.x < 130, "relative motion escaped pointer confinement");
    struct wlr_pointer_motion_absolute_event absolute = {
        .pointer = &pointer, .time_msec = 16, .x = 0.1, .y = 0.1};
    wl_signal_emit_mutable(&pointer.events.motion_absolute, &absolute);
    wl_signal_emit_mutable(&pointer.events.frame, &pointer);
    round_trip(server, right.display);
    check(scene.x >= 100 && scene.x < 105 && right.events.unconfines == 0,
          "absolute motion escaped pointer confinement");
    zwp_confined_pointer_v1_destroy(confine);
    round_trip(server, right.display);
    relative(&pointer, -60, -60);
    round_trip(server, left.display);
    check(left.events.pointer_enters == 3, "destroying constraint left pointer focus stuck");

    // Losing the initiating pointer cancels a gesture even if a second
    // pointer is still connected and coincidentally holding the same button.
    struct wlr_pointer second = {0};
    wlr_pointer_init(&second, &pointer_impl, "remaining pointer");
    wl_signal_emit_mutable(&backend.events.new_input, &second.base);
    key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
    button(&pointer, BTN_RIGHT, true);
    button(&second, BTN_RIGHT, false);
    button(&second, BTN_RIGHT, true);
    wlr_pointer_finish(&pointer);
    button(&second, BTN_RIGHT, false);
    key(&keyboard, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_RELEASED);
    axis(&second);
    round_trip(server, left.display);
    check(scene.cancelled == 1 && scene.consumed_releases == 1 &&
              left.events.pointer_buttons == 0 && left.events.pointer_axes == 1,
          "device removal left its gesture active or leaked a consumed button pair");
    wlr_pointer_finish(&second);
    round_trip(server, left.display);
    check(left.events.capabilities == WL_SEAT_CAPABILITY_KEYBOARD &&
              left.events.pointer_leaves == 4,
          "pointer unplug did not clear hover and retire pointer capability");
    disconnect_pointer_client(server, &left);
    disconnect_pointer_client(server, &right);
    wl_display_destroy_clients(server);
    seaside_tty_input_destroy(scene.input);
    wlr_backend_finish(&backend);
    wlr_keyboard_finish(&keyboard);
    wl_display_destroy(server);
}

static const char mime[] = "text/plain;charset=utf-8";

struct data_client {
    struct pointer_client pointer;
    struct wl_data_device_manager *manager;
    struct wl_data_device *device;
    struct zwp_primary_selection_device_manager_v1 *primary_manager;
    struct zwp_primary_selection_device_v1 *primary_device;
    struct wl_data_offer *selection, *drag_offer;
    struct zwp_primary_selection_offer_v1 *primary;
    unsigned clipboard_mimes, primary_mimes, enters, motions, drops, leaves;
    uint32_t enter_serial;
};

struct test_source {
    const char *payload;
    unsigned sends, cancelled, dropped, finished;
};

static void write_payload(struct test_source *source, const char *type, int32_t fd) {
    const size_t size = strlen(source->payload);
    check(strcmp(type, mime) == 0 && write(fd, source->payload, size) == (ssize_t)size,
          "selection source could not write the negotiated MIME payload");
    close(fd);
    ++source->sends;
}

static void source_target(void *data, struct wl_data_source *source, const char *type) {
    (void)data;
    (void)source;
    (void)type;
}

static void source_send(void *data, struct wl_data_source *source, const char *type, int32_t fd) {
    (void)source;
    write_payload(data, type, fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
    (void)source;
    ++((struct test_source *)data)->cancelled;
}

static void source_dropped(void *data, struct wl_data_source *source) {
    (void)source;
    ++((struct test_source *)data)->dropped;
}

static void source_finished(void *data, struct wl_data_source *source) {
    (void)source;
    ++((struct test_source *)data)->finished;
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action) {
    (void)data;
    (void)source;
    check(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE ||
              action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
          "unexpected source drag action");
}

static struct wl_data_source *create_data_source(struct data_client *client,
                                                 struct test_source *observation) {
    static const struct wl_data_source_listener listener = {
        .target = source_target,
        .send = source_send,
        .cancelled = source_cancelled,
        .dnd_drop_performed = source_dropped,
        .dnd_finished = source_finished,
        .action = source_action,
    };
    struct wl_data_source *source = wl_data_device_manager_create_data_source(client->manager);
    wl_data_source_add_listener(source, &listener, observation);
    wl_data_source_offer(source, mime);
    return source;
}

static void primary_source_send(void *data, struct zwp_primary_selection_source_v1 *source,
                                const char *type, int32_t fd) {
    (void)source;
    write_payload(data, type, fd);
}

static void primary_source_cancelled(void *data, struct zwp_primary_selection_source_v1 *source) {
    (void)source;
    ++((struct test_source *)data)->cancelled;
}

static struct zwp_primary_selection_source_v1 *
create_primary_source(struct data_client *client, struct test_source *observation) {
    static const struct zwp_primary_selection_source_v1_listener listener = {
        .send = primary_source_send, .cancelled = primary_source_cancelled};
    struct zwp_primary_selection_source_v1 *source =
        zwp_primary_selection_device_manager_v1_create_source(client->primary_manager);
    zwp_primary_selection_source_v1_add_listener(source, &listener, observation);
    zwp_primary_selection_source_v1_offer(source, mime);
    return source;
}

static void data_offer_type(void *data, struct wl_data_offer *offer, const char *type) {
    (void)offer;
    check(strcmp(type, mime) == 0, "unexpected data offer MIME type");
    ++((struct data_client *)data)->clipboard_mimes;
}

static void data_offer_actions(void *data, struct wl_data_offer *offer, uint32_t actions) {
    (void)data;
    (void)offer;
    check(actions == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, "unexpected offered drag action");
}

static void data_offer_action(void *data, struct wl_data_offer *offer, uint32_t action) {
    (void)data;
    (void)offer;
    check(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE ||
              action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
          "unexpected negotiated drag action");
}

static void data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    (void)device;
    static const struct wl_data_offer_listener listener = {.offer = data_offer_type,
                                                           .source_actions = data_offer_actions,
                                                           .action = data_offer_action};
    wl_data_offer_add_listener(offer, &listener, data);
}

static void data_enter(void *data, struct wl_data_device *device, uint32_t serial,
                       struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *offer) {
    (void)device;
    (void)x;
    (void)y;
    struct data_client *client = data;
    check(surface == client->pointer.surface && offer && !client->drag_offer,
          "drag entered an unexpected target");
    client->drag_offer = offer;
    client->enter_serial = serial;
    ++client->enters;
}

static void data_leave(void *data, struct wl_data_device *device) {
    (void)device;
    struct data_client *client = data;
    if (client->drag_offer && client->drops == 0) {
        wl_data_offer_destroy(client->drag_offer);
        client->drag_offer = NULL;
    }
    ++client->leaves;
}

static void data_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x,
                        wl_fixed_t y) {
    (void)device;
    (void)time;
    (void)x;
    (void)y;
    ++((struct data_client *)data)->motions;
}

static void data_drop(void *data, struct wl_data_device *device) {
    (void)device;
    ++((struct data_client *)data)->drops;
}

static void data_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    (void)device;
    struct data_client *client = data;
    if (client->selection) {
        wl_data_offer_destroy(client->selection);
    }
    client->selection = offer;
}

static void primary_offer_type(void *data, struct zwp_primary_selection_offer_v1 *offer,
                               const char *type) {
    (void)offer;
    check(strcmp(type, mime) == 0, "unexpected primary offer MIME type");
    ++((struct data_client *)data)->primary_mimes;
}

static void primary_offer(void *data, struct zwp_primary_selection_device_v1 *device,
                          struct zwp_primary_selection_offer_v1 *offer) {
    (void)device;
    static const struct zwp_primary_selection_offer_v1_listener listener = {.offer =
                                                                                primary_offer_type};
    zwp_primary_selection_offer_v1_add_listener(offer, &listener, data);
}

static void primary_selection(void *data, struct zwp_primary_selection_device_v1 *device,
                              struct zwp_primary_selection_offer_v1 *offer) {
    (void)device;
    struct data_client *client = data;
    if (client->primary) {
        zwp_primary_selection_offer_v1_destroy(client->primary);
    }
    client->primary = offer;
}

static struct wlr_surface *connect_data_client(struct wl_display *server,
                                               struct data_client *client) {
    static const struct wl_data_device_listener device_listener = {
        .data_offer = data_offer,
        .enter = data_enter,
        .leave = data_leave,
        .motion = data_motion,
        .drop = data_drop,
        .selection = data_selection,
    };
    static const struct zwp_primary_selection_device_v1_listener primary_listener = {
        .data_offer = primary_offer, .selection = primary_selection};
    struct wlr_surface *surface = connect_pointer_client(server, &client->pointer);
    struct keyboard_client *events = &client->pointer.events;
    check(events->data_device_manager && events->primary_manager_name && !events->data_control,
          "unexpected clipboard protocol globals");
    client->manager = wl_registry_bind(client->pointer.registry, events->data_manager_name,
                                       &wl_data_device_manager_interface, 3);
    client->device = wl_data_device_manager_get_data_device(client->manager, events->seat);
    wl_data_device_add_listener(client->device, &device_listener, client);
    client->primary_manager =
        wl_registry_bind(client->pointer.registry, events->primary_manager_name,
                         &zwp_primary_selection_device_manager_v1_interface, 1);
    client->primary_device =
        zwp_primary_selection_device_manager_v1_get_device(client->primary_manager, events->seat);
    zwp_primary_selection_device_v1_add_listener(client->primary_device, &primary_listener, client);
    round_trip(server, client->pointer.display);
    return surface;
}

static void disconnect_data_client(struct wl_display *server, struct data_client *client) {
    if (client->selection) {
        wl_data_offer_destroy(client->selection);
    }
    if (client->drag_offer) {
        wl_data_offer_destroy(client->drag_offer);
    }
    if (client->primary) {
        zwp_primary_selection_offer_v1_destroy(client->primary);
    }
    wl_data_device_release(client->device);
    wl_data_device_manager_destroy(client->manager);
    zwp_primary_selection_device_v1_destroy(client->primary_device);
    zwp_primary_selection_device_manager_v1_destroy(client->primary_manager);
    disconnect_pointer_client(server, &client->pointer);
}

static uint32_t data_key_serial(struct wl_display *server, struct data_client *client,
                                struct wlr_keyboard *keyboard) {
    key(keyboard, KEY_C, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(keyboard, KEY_C, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client->pointer.display);
    check(client->pointer.events.key_serial != 0, "missing keyboard event serial");
    return client->pointer.events.key_serial;
}

static void expect_payload(struct wl_display *server, struct data_client *writer,
                           struct data_client *reader, struct wl_data_offer *offer,
                           struct zwp_primary_selection_offer_v1 *primary, const char *payload) {
    int pipe_ends[2];
    check(pipe2(pipe_ends, O_CLOEXEC | O_NONBLOCK) == 0, "could not allocate selection pipe");
    check((offer != NULL) != (primary != NULL), "missing selection or drag offer");
    if (offer) {
        wl_data_offer_receive(offer, mime, pipe_ends[1]);
    } else {
        zwp_primary_selection_offer_v1_receive(primary, mime, pipe_ends[1]);
    }
    close(pipe_ends[1]);
    round_trip(server, reader->pointer.display);
    round_trip(server, writer->pointer.display);
    char bytes[64];
    ssize_t length = read(pipe_ends[0], bytes, sizeof(bytes));
    check(length == (ssize_t)strlen(payload) && memcmp(bytes, payload, (size_t)length) == 0,
          "selection bytes differed from the source payload");
    close(pipe_ends[0]);
}

static struct wl_data_source *start_data_drag(struct wl_display *server, struct data_client *client,
                                              struct test_source *drag, struct wlr_pointer *pointer,
                                              struct wl_surface *icon) {
    struct wl_data_source *source = create_data_source(client, drag);
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    button(pointer, BTN_LEFT, true);
    round_trip(server, client->pointer.display);
    wl_data_device_start_drag(client->device, source, client->pointer.surface, icon,
                              client->pointer.events.button_serial);
    round_trip(server, client->pointer.display);
    return source;
}

static void exercise_data_device(void) {
    struct wl_display *server = wl_display_create();
    struct wlr_backend backend = {0};
    struct wlr_keyboard keyboard = {0};
    struct wlr_pointer pointer = {0};
    struct pointer_scene scene = {.x = 10, .y = 10};
    create_pointer_seat(server, &backend, &keyboard, &pointer, &scene);
    struct wlr_seat *seat = seaside_tty_input_get_seat(scene.input);
    struct data_client left = {0}, right = {0};
    scene.surfaces[0] = connect_data_client(server, &left);
    scene.surfaces[1] = connect_data_client(server, &right);
    struct wl_display *sender = left.pointer.display, *receiver = right.pointer.display;

    seaside_tty_input_set_focus(scene.input, scene.surfaces[0]);
    const uint32_t left_serial = data_key_serial(server, &left, &keyboard);
    struct test_source clipboard = {.payload = "clipboard"};
    struct test_source primary = {.payload = "primary"};
    struct wl_data_source *clipboard_source = create_data_source(&left, &clipboard);
    struct zwp_primary_selection_source_v1 *primary_source = create_primary_source(&left, &primary);
    wl_data_device_set_selection(left.device, clipboard_source, left_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, primary_source, left_serial);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(left.selection && left.primary && !right.selection && !right.primary,
          "unfocused client received a clipboard or primary offer");

    const uint32_t delayed_serial = data_key_serial(server, &left, &keyboard);
    seaside_tty_input_set_focus(scene.input, scene.surfaces[1]);
    data_key_serial(server, &right, &keyboard);
    check(right.clipboard_mimes == 1 && right.primary_mimes == 1,
          "focused client did not receive the selection MIME offers");
    wl_data_device_set_selection(right.device, NULL, left_serial);
    zwp_primary_selection_device_v1_set_selection(right.primary_device, NULL, left_serial);
    round_trip(server, receiver);
    check(right.selection && right.primary, "foreign serial cleared a focused selection");

    expect_payload(server, &left, &right, right.selection, NULL, clipboard.payload);
    expect_payload(server, &left, &right, NULL, right.primary, primary.payload);
    check(clipboard.sends == 1 && primary.sends == 1, "selection sources were not each read once");

    // X11 MIME negotiation can finish after the user already changed focus.
    struct test_source later_clipboard = {.payload = "delayed clipboard"};
    struct test_source later_primary = {.payload = "delayed primary"};
    struct wl_data_source *delayed_clipboard = create_data_source(&left, &later_clipboard);
    struct zwp_primary_selection_source_v1 *delayed_primary =
        create_primary_source(&left, &later_primary);
    wl_data_device_set_selection(left.device, delayed_clipboard, delayed_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, delayed_primary,
                                                  delayed_serial);
    wl_data_device_set_selection(left.device, NULL, left_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, NULL, left_serial);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(seat->keyboard_state.focused_surface == scene.surfaces[1] && clipboard.cancelled == 1 &&
              primary.cancelled == 1,
          "delayed selection did not replace its predecessor with focus preserved");
    expect_payload(server, &left, &right, right.selection, NULL, later_clipboard.payload);
    expect_payload(server, &left, &right, NULL, right.primary, later_primary.payload);

    wl_data_source_destroy(clipboard_source);
    zwp_primary_selection_source_v1_destroy(primary_source);
    wl_data_source_destroy(delayed_clipboard);
    zwp_primary_selection_source_v1_destroy(delayed_primary);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(!right.selection && !right.primary, "source teardown left a cached selection");

    seaside_tty_input_refresh_pointer(scene.input);
    struct test_source drag = {.payload = "dropped"};
    struct wl_data_source *drag_source = create_data_source(&left, &drag);
    const uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    wl_data_source_set_actions(drag_source, copy);
    wl_data_device_start_drag(left.device, drag_source, left.pointer.surface, NULL, left_serial);
    round_trip(server, sender);
    check(drag.cancelled == 1 && !seat->drag, "non-button serial started a drag");
    wl_data_source_destroy(drag_source);

    struct wl_surface *icon = wl_compositor_create_surface(left.pointer.events.compositor);
    drag_source = start_data_drag(server, &left, &drag, &pointer, icon);
    check(seat->drag && scene.drag_icons == 1, "pointer serial failed to start drag with icon");

    relative(&pointer, 110, 110);
    round_trip(server, receiver);
    check(right.drag_offer && right.enters == 1 && right.motions > 0,
          "drag did not enter or move over its target");
    wl_data_offer_accept(right.drag_offer, right.enter_serial, mime);
    wl_data_offer_set_actions(right.drag_offer, copy, copy);
    round_trip(server, receiver);
    struct wlr_pointer other = {0};
    wlr_pointer_init(&other, &pointer_impl, "second drag test pointer");
    wl_signal_emit_mutable(&backend.events.new_input, &other.base);
    button(&other, BTN_LEFT, true);
    button(&pointer, BTN_LEFT, false);
    round_trip(server, receiver);
    check(!seat->drag && right.drops == 1 && right.leaves == 1 && right.drag_offer,
          "drop did not finish the grab and retain a transfer offer");
    button(&other, BTN_LEFT, false);
    expect_payload(server, &left, &right, right.drag_offer, NULL, drag.payload);
    wl_data_offer_finish(right.drag_offer);
    wl_data_offer_destroy(right.drag_offer);
    right.drag_offer = NULL;
    right.drops = 0;
    round_trip(server, receiver);
    round_trip(server, sender);
    check(drag.dropped == 1 && drag.finished == 1, "source did not observe drop completion");
    const unsigned previous_cancels = drag.cancelled;
    wl_surface_destroy(icon);
    wl_data_source_destroy(drag_source);

    relative(&pointer, -110, -110);
    drag_source = start_data_drag(server, &left, &drag, &pointer, NULL);
    relative(&pointer, 110, 110);
    round_trip(server, receiver);
    check(right.drag_offer && seat->drag, "second drag could not reach target");
    wl_data_offer_accept(right.drag_offer, right.enter_serial, mime);
    wl_data_offer_set_actions(right.drag_offer, copy, copy);
    round_trip(server, receiver);
    wlr_pointer_finish(&pointer);
    round_trip(server, receiver);
    round_trip(server, sender);
    check(!seat->drag && !right.drag_offer && right.drops == 0 &&
              drag.cancelled == previous_cancels + 1,
          "unplug emitted a drop or left the client drag active");
    wl_data_source_destroy(drag_source);

    relative(&other, -110, -110);
    drag_source = start_data_drag(server, &left, &drag, &other, NULL);
    relative(&other, 110, 110);
    round_trip(server, receiver);
    check(right.drag_offer && seat->drag && seat->drag->focus == scene.surfaces[1],
          "third drag did not focus the target about to be destroyed");
    wl_surface_destroy(right.pointer.surface);
    right.pointer.surface = NULL;
    scene.surfaces[1] = NULL;
    round_trip(server, receiver);
    check(!right.drag_offer && seat->drag && !seat->drag->focus,
          "destroyed target kept its drag offer or focus");
    key(&keyboard, KEY_ESC, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&keyboard, KEY_ESC, WL_KEYBOARD_KEY_STATE_RELEASED);
    button(&other, BTN_LEFT, false);
    round_trip(server, sender);
    check(!seat->drag && drag.cancelled == previous_cancels + 2 && !left.pointer.events.escaped &&
              seat->keyboard_state.focused_surface == scene.surfaces[0],
          "Escape did not cancel the drag and restore input focus cleanly");
    wl_data_source_destroy(drag_source);
    wlr_pointer_finish(&other);

    disconnect_data_client(server, &left);
    disconnect_data_client(server, &right);
    wl_display_destroy_clients(server);
    seaside_tty_input_destroy(scene.input);
    wlr_backend_finish(&backend);
    wlr_keyboard_finish(&keyboard);
    wl_display_destroy(server);
}

int main(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "keyboard") == 0)) {
        exercise(true);
        exercise(false);
        exercise_wayland_keyboard();
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "pointer") == 0)) {
        exercise_wayland_pointer();
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "data-device") == 0)) {
        exercise_data_device();
    }
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "keyboard") != 0 &&
                     strcmp(argv[1], "pointer") != 0 && strcmp(argv[1], "data-device") != 0)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

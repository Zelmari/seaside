#include "tty_input.hpp"
#include "wl_array_span.hpp"
#include "wlr_compat.hpp"

#include "cursor-shape-v1-client-protocol.h"
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

extern "C" {
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_shm.h>
}
#include <xkbcommon/xkbcommon-keysyms.h>

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

struct observation : seaside::TtyInputObserver {
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
    enum seaside_pointer_motion kind;
    double x, y;

    void motion(enum seaside_pointer_motion kind, double x, double y) noexcept override {
        ++motions;
        this->kind = kind;
        this->x = x;
        this->y = y;
    }

    void exit_requested() noexcept override { ++exits; }

    void launch_requested() noexcept override { ++launches; }

    bool shortcut_pressed(uint8_t modifiers, uint32_t keysym) noexcept override {
        if (modifiers == 2 && keysym == XKB_KEY_Return) {
            ++configured_shortcuts;
            return true;
        }
        if (keysym == XKB_KEY_XF86Switch_VT_2) {
            ++vt_switches;
            return true;
        }
        if (modifiers == 1) {
            switch (keysym) {
            case XKB_KEY_D:
                ++alternate_case_shortcuts;
                return true;
            case XKB_KEY_e:
                ++exact_case_shortcuts;
                return true;
            case XKB_KEY_E:
                ++shadowed_case_shortcuts;
                return true;
            }
        }
        if (modifiers == (1 | 8)) {
            switch (keysym) {
            case XKB_KEY_2:
                ++digit_shortcuts;
                return true;
            case XKB_KEY_dollar:
                ++explicit_symbol_shortcuts;
                return true;
            case XKB_KEY_4:
                ++shadowed_digit_shortcuts;
                return true;
            }
        }
        return false;
    }

    void modifiers_changed(uint8_t modifiers) noexcept override {
        ++modifier_changes;
        this->modifiers = modifiers;
    }
};

struct KeyboardObservation : observation {
    unsigned keyboard_activity = 0;

    void input_activity(bool pointer) noexcept override {
        check(!pointer, "unexpected pointer activity in the keyboard fixture");
        ++keyboard_activity;
    }
};

static const struct wlr_backend_impl backend_impl = {};
static constexpr wlr_keyboard_impl keyboard_impl{.name = "test", .led_update = nullptr};
static const struct wlr_pointer_impl pointer_impl = {.name = "test"};

static void exercise(bool backend_first) noexcept {
    struct wlr_backend backend = {};
    wlr_backend_init(&backend, &backend_impl);
    struct observation observation = {};
    auto created = seaside::TtyInput::create(&backend, observation);
    check(created.has_value(), "cannot create input bridge");
    auto input = std::move(*created);

    struct wlr_keyboard keyboard = {};
    wlr_keyboard_init(&keyboard, &keyboard_impl, "test keyboard");
    wl_signal_emit_mutable(&backend.events.new_input, &keyboard.base);
    struct wlr_pointer pointer = {};
    wlr_pointer_init(&pointer, &pointer_impl, "test pointer");
    wl_signal_emit_mutable(&backend.events.new_input, &pointer.base);

    seaside::TtyInputSnapshot snapshot = input->snapshot();
    check(snapshot.pointers == 1 && snapshot.keyboards == 1 && !snapshot.failed,
          "input discovery did not register both devices");
    struct wlr_pointer_motion_event relative = {
        .pointer = &pointer,
        .time_msec = 0,
        .delta_x = 10.5,
        .delta_y = -4,
        .unaccel_dx = 7,
        .unaccel_dy = -2,
    };
    wl_signal_emit_mutable(&pointer.events.motion, &relative);
    check(observation.motions == 1 && observation.kind == SEASIDE_POINTER_RELATIVE &&
              observation.x == 10.5 && observation.y == -4,
          "relative pointer did not report the accelerated deltas");
    wlr_pointer_motion_absolute_event absolute{
        .pointer = &pointer, .time_msec = 0, .x = 0.25, .y = 0.75};
    wl_signal_emit_mutable(&pointer.events.motion_absolute, &absolute);
    check(observation.motions == 2 && observation.kind == SEASIDE_POINTER_ABSOLUTE &&
              observation.x == 0.25 && observation.y == 0.75,
          "absolute pointer did not report the normalized coordinates");

    wlr_keyboard_key_event event{.time_msec = 0,
                                 .keycode = KEY_ENTER,
                                 .update_state = false,
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
    snapshot = input->snapshot();
    check(snapshot.pointers == 1 && snapshot.keyboards == 0 && !snapshot.failed,
          "keyboard removal did not detach the input device");

    /* Leave the pointer alive to test both owner destruction orders. */
    if (backend_first) {
        wlr_backend_finish(&backend);
        snapshot = input->snapshot();
        check(snapshot.pointers == 0 && snapshot.keyboards == 0 && !snapshot.failed,
              "backend destruction did not detach surviving input devices");
        wl_signal_emit_mutable(&pointer.events.motion, &relative);
        check(observation.motions == 2, "pointer still dispatched after backend destruction");
        input.reset();
    } else {
        input.reset();
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
    struct wp_cursor_shape_manager_v1 *cursor_shapes;
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
    uint32_t pointer_serial;
    uint32_t depressed;
    bool data_device_manager;
    uint32_t data_manager_name, primary_manager_name;
    bool ext_data_control, wlr_data_control;
    bool escaped;
    bool entered_with_return;
    unsigned pointer_enters;
    unsigned pointer_leaves;
    unsigned pointer_motions;
    unsigned pointer_buttons;
    unsigned pointer_axes;
    double axis_delta;
    uint32_t axis_orientation, axis_source;
    int32_t axis_discrete;
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
                          uint32_t size) noexcept {
    (void)keyboard;
    auto *client = static_cast<keyboard_client *>(data);
    check(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0, "missing XKB keymap");
    close(fd);
    ++client->keymaps;
}

static void client_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         struct wl_surface *surface, struct wl_array *keys) noexcept {
    (void)keyboard;
    (void)serial;
    (void)surface;
    auto *client = static_cast<keyboard_client *>(data);
    ++client->enters;
    for (const uint32_t key : wl_array_span<uint32_t>(keys)) {
        if (key == KEY_ENTER) {
            client->entered_with_return = true;
        }
    }
}

static void client_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         struct wl_surface *surface) noexcept {
    (void)data;
    (void)keyboard;
    (void)serial;
    (void)surface;
}

static void client_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                       uint32_t key, uint32_t state) noexcept {
    (void)keyboard;
    (void)time;
    auto *client = static_cast<keyboard_client *>(data);
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
                             uint32_t group) noexcept {
    (void)keyboard;
    (void)serial;
    (void)latched;
    (void)locked;
    (void)group;
    auto *client = static_cast<keyboard_client *>(data);
    client->depressed = depressed;
}

static void client_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate,
                          int32_t delay) noexcept {
    (void)keyboard;
    auto *client = static_cast<keyboard_client *>(data);
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
                          struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) noexcept {
    (void)pointer;
    (void)surface;
    auto *client = static_cast<keyboard_client *>(data);
    client->pointer_serial = serial;
    ++client->pointer_enters;
    client->pointer_x = wl_fixed_to_double(sx);
    client->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface) noexcept {
    (void)pointer;
    (void)serial;
    (void)surface;
    ++static_cast<keyboard_client *>(data)->pointer_leaves;
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t sx,
                           wl_fixed_t sy) noexcept {
    (void)pointer;
    (void)time;
    auto *client = static_cast<keyboard_client *>(data);
    ++client->pointer_motions;
    client->pointer_x = wl_fixed_to_double(sx);
    client->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) noexcept {
    (void)pointer;
    (void)time;
    (void)button;
    auto *client = static_cast<keyboard_client *>(data);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        client->button_serial = serial;
    }
    ++client->pointer_buttons;
}

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value) noexcept {
    (void)pointer;
    (void)time;
    auto *client = static_cast<keyboard_client *>(data);
    ++client->pointer_axes;
    client->axis_orientation = axis;
    client->axis_delta = wl_fixed_to_double(value);
}

static void pointer_frame(void *data, struct wl_pointer *pointer) noexcept {
    (void)pointer;
    ++static_cast<keyboard_client *>(data)->pointer_frames;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) noexcept {
    (void)pointer;
    static_cast<keyboard_client *>(data)->axis_source = source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                              uint32_t axis) noexcept {
    (void)data;
    (void)pointer;
    (void)time;
    (void)axis;
    check(false, "unexpected axis stop");
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                  int32_t discrete) noexcept {
    (void)pointer;
    (void)axis;
    static_cast<keyboard_client *>(data)->axis_discrete = discrete;
}

static constexpr auto pointer_listener = [] {
    wl_pointer_listener result{};
    result.enter = pointer_enter;
    result.leave = pointer_leave;
    result.motion = pointer_motion;
    result.button = pointer_button;
    result.axis = pointer_axis;
    result.frame = pointer_frame;
    result.axis_source = pointer_axis_source;
    result.axis_stop = pointer_axis_stop;
    result.axis_discrete = pointer_axis_discrete;
    return result;
}();

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) noexcept {
    auto *client = static_cast<keyboard_client *>(data);
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

static void seat_name(void *data, struct wl_seat *seat, const char *name) noexcept {
    (void)data;
    (void)seat;
    check(strcmp(name, "seat0") == 0, "unexpected seat name");
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void shm_format(void *data, struct wl_shm *shm, uint32_t format) noexcept {
    (void)data;
    (void)shm;
    (void)format;
}

static const struct wl_shm_listener shm_listener = {.format = shm_format};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) noexcept {
    auto *client = static_cast<keyboard_client *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        client->seat =
            static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
        wl_seat_add_listener(client->seat, &seat_listener, client);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        client->data_device_manager = version >= 3;
        client->data_manager_name = name;
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        client->primary_manager_name = name;
    } else if (strcmp(interface, "ext_data_control_manager_v1") == 0) {
        client->ext_data_control = version >= 1;
    } else if (strcmp(interface, "zwlr_data_control_manager_v1") == 0) {
        client->wlr_data_control = version >= 2;
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(client->shm, &shm_listener, client);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        client->cursor_shapes = static_cast<wp_cursor_shape_manager_v1 *>(
            wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1));
    } else if (strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        client->constraints = static_cast<zwp_pointer_constraints_v1 *>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        client->relative_pointer_manager = static_cast<zwp_relative_pointer_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) noexcept {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void mark_done(void *data, struct wl_callback *callback, uint32_t serial) noexcept {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}

static void round_trip(struct wl_display *server, struct wl_display *client) noexcept {
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
        .time_msec = 0,
        .keycode = keycode,
        .update_state = true,
        .state = state,
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
    check(server && wlr_compositor_create(server, 6, nullptr), "Wayland compositor setup failed");
    struct wlr_backend backend = {};
    wlr_backend_init(&backend, &backend_impl);
    KeyboardObservation observation{};
    auto created = seaside::TtyInput::create(&backend, observation,
                                             {.track_modifiers = true, .fixture_launch = true});
    check(created.has_value(), "cannot create keyboard input bridge");
    auto input = std::move(*created);
    check(input->enable_keyboard(server), "keyboard seat setup failed");
    struct wlr_keyboard first = {}, second = {};
    wlr_keyboard_init(&first, &keyboard_impl, "keyboard one");
    wlr_keyboard_init(&second, &keyboard_impl, "keyboard two");
    wl_signal_emit_mutable(&backend.events.new_input, &first.base);
    wl_signal_emit_mutable(&backend.events.new_input, &second.base);
    check(input->snapshot().keyboards == 2, "keyboard group missing a device");

    int ends[2];
    check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ends) == 0, "socketpair failed");
    struct wl_client *peer = wl_client_create(server, ends[0]);
    struct wl_display *client = wl_display_connect_to_fd(ends[1]);
    check(peer && client, "client connect failed");
    struct keyboard_client keyboard_client = {};
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
    check(resource != nullptr, "missing Wayland surface resource");
    struct wlr_surface *server_surface = wlr_surface_from_resource(resource);

    key(&first, KEY_LEFTMETA, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&first, KEY_ENTER, WL_KEYBOARD_KEY_STATE_PRESSED);
    check(observation.launches == 1, "grouped Super+Return did not launch exactly once");
    input->set_focus(server_surface);
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
    const unsigned activity_before_removal = observation.keyboard_activity;
    wlr_keyboard_finish(&first);
    round_trip(server, client);
    check(observation.keyboard_activity == activity_before_removal,
          "unplugging a keyboard with a held key counted as idle activity");
    check(keyboard_client.depressed == 0, "unplugging keyboard left Super stuck");
    check(keyboard_client.key_events == 5 && keyboard_client.key == KEY_LEFTMETA &&
              keyboard_client.key_state == WL_KEYBOARD_KEY_STATE_RELEASED,
          "shortcut release leaked or a regular key event was lost");

    key(&second, KEY_ESC, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_ESC, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client);
    check(observation.keyboard_activity == activity_before_removal + 2,
          "a physical key press or release did not count as idle activity");
    const seaside::TtyInputSnapshot received = input->snapshot();
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
    round_trip(server, client);

    struct wl_keyboard *bound_keyboard = keyboard_client.keyboard;
    const unsigned before_resume = keyboard_client.key_events;
    wlr_keyboard_finish(&second);
    round_trip(server, client);
    const seaside::TtyInputSnapshot suspended = input->snapshot();
    check(suspended.keyboards == 0 && suspended.has_keyboard_focus &&
              keyboard_client.keyboard == bound_keyboard &&
              keyboard_client.capabilities == WL_SEAT_CAPABILITY_KEYBOARD,
          "physical keyboard suspension removed the focused logical keyboard");
    wlr_keyboard_init(&second, &keyboard_impl, "resumed keyboard");
    wl_signal_emit_mutable(&backend.events.new_input, &second.base);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    round_trip(server, client);
    check(keyboard_client.key_events == before_resume + 2 && keyboard_client.key == KEY_A,
          "the first key after physical keyboard resume did not reach the existing client");

    const unsigned previous_modifier_changes = observation.modifier_changes;
    const uint64_t previous_forwarded = input->snapshot().forwarded_key_events;

    wl_surface_destroy(surface);
    round_trip(server, client);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    key(&second, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    const seaside::TtyInputSnapshot unfocused = input->snapshot();
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
    input.reset();
    wlr_keyboard_finish(&second);
}

struct pointer_scene : seaside::TtyInputObserver {
    std::unique_ptr<seaside::TtyInput> input;
    struct wlr_surface *surfaces[2];
    double x, y;
    unsigned move_calls, clicks, consumed_releases, cancelled;
    enum seaside_pointer_motion motion_kind;
    unsigned drag_icons;
    bool locked;
    const char *last_cursor_shape;
    struct wlr_surface *last_cursor_surface;
    unsigned cursor_requests;
    int32_t hotspot_x, hotspot_y;

    void motion(enum seaside_pointer_motion kind, double x, double y) noexcept override {
        ++move_calls;
        motion_kind = kind;
        this->x = kind == SEASIDE_POINTER_ABSOLUTE ? 200 * x : this->x + x;
        this->y = kind == SEASIDE_POINTER_ABSOLUTE ? 100 * y : this->y + y;
    }

    bool pointer_absolute_delta(double x, double y, double &dx, double &dy) noexcept override {
        dx = 200 * x - this->x;
        dy = 100 * y - this->y;
        return true;
    }

    bool pointer_target(seaside::TtyPointerTarget &target) noexcept override {
        size_t slot;
        if (target.surface) {
            slot = target.surface == surfaces[0] ? 0 : 1;
            if (target.surface != surfaces[slot]) {
                return false;
            }
        } else {
            if (x < 0 || x >= 200 || y < 0 || y >= 100) {
                return false;
            }
            slot = x < 100 ? 0 : 1;
        }
        target.surface = surfaces[slot];
        target.window_id = slot + 1;
        target.sx = x - (slot == 0 ? 0.0 : 100.0);
        target.sy = y;
        return target.surface != nullptr;
    }

    bool pointer_button(uint64_t window, uint32_t button, bool pressed,
                        uint8_t modifiers) noexcept override {
        if (button == BTN_RIGHT && modifiers == 1) {
            if (!pressed) {
                ++consumed_releases;
            }
            return true;
        }
        if (pressed && window > 0 && window <= 2) {
            ++clicks;
            input->set_focus(surfaces[window - 1]);
        }
        return false;
    }

    void pointer_locked(bool locked) noexcept override { this->locked = locked; }

    void cursor_shape(const char *shape) noexcept override {
        last_cursor_shape = shape;
        last_cursor_surface = nullptr;
        ++cursor_requests;
    }

    void cursor_surface(struct wlr_surface *surface, int32_t x, int32_t y) noexcept override {
        last_cursor_shape = nullptr;
        last_cursor_surface = surface;
        hotspot_x = x;
        hotspot_y = y;
        ++cursor_requests;
    }

    void pointer_cancelled() noexcept override { ++cancelled; }

    void drag_icon(struct wlr_drag_icon *icon) noexcept override {
        check(icon && icon->surface, "missing requested drag icon");
        ++drag_icons;
    }
};

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
                            wl_fixed_t unaccel_dy) noexcept {
    (void)relative;
    (void)hi;
    (void)lo;
    (void)dy;
    (void)unaccel_dy;
    auto *client = static_cast<keyboard_client *>(data);
    ++client->relative_motions;
    client->relative_dx = wl_fixed_to_double(dx);
    client->unaccelerated_dx = wl_fixed_to_double(unaccel_dx);
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {
    .relative_motion = relative_motion,
};

static void pointer_locked(void *data, struct zwp_locked_pointer_v1 *lock) noexcept {
    (void)lock;
    ++static_cast<keyboard_client *>(data)->locks;
}

static void pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *lock) noexcept {
    (void)lock;
    ++static_cast<keyboard_client *>(data)->unlocks;
}

static const struct zwp_locked_pointer_v1_listener lock_listener = {
    .locked = pointer_locked,
    .unlocked = pointer_unlocked,
};

static void pointer_confined(void *data, struct zwp_confined_pointer_v1 *confine) noexcept {
    (void)confine;
    ++static_cast<keyboard_client *>(data)->confines;
}

static void pointer_unconfined(void *data, struct zwp_confined_pointer_v1 *confine) noexcept {
    (void)confine;
    ++static_cast<keyboard_client *>(data)->unconfines;
}

static const struct zwp_confined_pointer_v1_listener confine_listener = {
    .confined = pointer_confined,
    .unconfined = pointer_unconfined,
};

static struct wlr_surface *connect_pointer_client(struct wl_display *server,
                                                  struct pointer_client *client) noexcept {
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
    check(resource != nullptr, "missing pointer test surface resource");
    return wlr_surface_from_resource(resource);
}

static void disconnect_pointer_client(struct wl_display *server, struct pointer_client *client) {
    if (client->surface) {
        wl_surface_destroy(client->surface);
    }
    wl_buffer_destroy(client->buffer);
    wp_cursor_shape_manager_v1_destroy(client->events.cursor_shapes);
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
    struct wlr_pointer_motion_event motion = {.pointer = pointer,
                                              .time_msec = 11,
                                              .delta_x = dx,
                                              .delta_y = 0,
                                              .unaccel_dx = raw_dx,
                                              .unaccel_dy = 0};
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
        .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
        .delta = 3,
        .delta_discrete = 0,
    };
    wl_signal_emit_mutable(&pointer->events.axis, &scroll);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
}

static void create_pointer_seat(struct wl_display *server, struct wlr_backend *backend,
                                struct wlr_keyboard *keyboard, struct wlr_pointer *pointer,
                                struct pointer_scene *scene) {
    const uint32_t formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
    check(server && wlr_compositor_create(server, 6, nullptr) &&
              wlr_shm_create(server, 1, formats, sizeof(formats) / sizeof(formats[0])),
          "pointer test compositor setup failed");
    wlr_backend_init(backend, &backend_impl);
    wlr_keyboard_init(keyboard, &keyboard_impl, "pointer test keyboard");
    wlr_pointer_init(pointer, &pointer_impl, "pointer test pointer");
    auto created = seaside::TtyInput::create(backend, *scene,
                                             {.pointer_targets = true, .client_cursor = true});
    check(created.has_value(), "cannot create pointer input bridge");
    scene->input = std::move(*created);
    check(scene->input->enable_keyboard(server) && scene->input->seat(),
          "pointer test seat setup failed");
    wl_signal_emit_mutable(&backend->events.new_input, &keyboard->base);
    wl_signal_emit_mutable(&backend->events.new_input, &pointer->base);
}

static void exercise_wayland_cursor(void) {
    struct wl_display *server = wl_display_create();
    struct wlr_backend backend = {};
    struct wlr_keyboard keyboard = {};
    struct wlr_pointer pointer = {};
    pointer_scene scene{};
    scene.x = scene.y = 10;
    create_pointer_seat(server, &backend, &keyboard, &pointer, &scene);

    struct pointer_client left = {}, right = {};
    scene.surfaces[0] = connect_pointer_client(server, &left);
    scene.surfaces[1] = connect_pointer_client(server, &right);
    check(left.events.cursor_shapes && right.events.cursor_shapes, "missing cursor shape protocol");
    const struct keyboard_client *left_seat = &left.events;
    const struct keyboard_client *right_seat = &right.events;
    struct wp_cursor_shape_device_v1 *left_device =
        wp_cursor_shape_manager_v1_get_pointer(left_seat->cursor_shapes, left_seat->pointer);
    struct wp_cursor_shape_device_v1 *right_device =
        wp_cursor_shape_manager_v1_get_pointer(right_seat->cursor_shapes, right_seat->pointer);

    scene.input->refresh_pointer();
    round_trip(server, left.display);
    check(left_seat->pointer_serial != 0, "missing cursor focus serial");
    const unsigned defaults = scene.cursor_requests;
    wp_cursor_shape_device_v1_set_shape(left_device, left_seat->pointer_serial - 1,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    round_trip(server, left.display);
    check(scene.cursor_requests == defaults, "named cursor accepted with an invalid serial");
    wp_cursor_shape_device_v1_set_shape(left_device, left_seat->pointer_serial,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
    round_trip(server, left.display);
    check(scene.cursor_requests > defaults && scene.last_cursor_shape &&
              strcmp(scene.last_cursor_shape, "text") == 0,
          "pointer focus did not set the named text cursor");

    struct wl_surface *cursor = wl_compositor_create_surface(left_seat->compositor);
    wl_pointer_set_cursor(left_seat->pointer, left_seat->pointer_serial, cursor, 5, 6);
    round_trip(server, left.display);
    check(scene.last_cursor_surface && scene.hotspot_x == 5 && scene.hotspot_y == 6,
          "focused pointer did not accept a cursor surface");
    const unsigned first_cursor = scene.cursor_requests;
    wl_surface_attach(cursor, left.buffer, 2, 3);
    wl_surface_commit(cursor);
    round_trip(server, left.display);
    check(scene.cursor_requests == first_cursor + 1 && scene.last_cursor_surface &&
              scene.hotspot_x == 3 && scene.hotspot_y == 3,
          "cursor surface commit did not preserve its hotspot relative to the new image");

    const unsigned committed_cursor = scene.cursor_requests;
    wl_pointer_set_cursor(right_seat->pointer, left_seat->pointer_serial, nullptr, 0, 0);
    round_trip(server, right.display);
    check(scene.cursor_requests == committed_cursor && scene.last_cursor_surface,
          "unfocused client could change the pointer cursor");

    relative(&pointer, 100, 100);
    round_trip(server, right.display);
    check(scene.cursor_requests > committed_cursor && !scene.last_cursor_shape &&
              !scene.last_cursor_surface,
          "leaving a cursor surface's client did not restore the desktop arrow");
    const unsigned focus_change = scene.cursor_requests;
    wl_surface_commit(cursor);
    round_trip(server, left.display);
    check(scene.cursor_requests == focus_change,
          "a previous cursor surface still changed the cursor after losing focus");
    wp_cursor_shape_device_v1_set_shape(right_device, right_seat->pointer_serial,
                                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
    round_trip(server, right.display);
    check(scene.cursor_requests == focus_change + 1 && scene.last_cursor_shape &&
              strcmp(scene.last_cursor_shape, "ew-resize") == 0,
          "new pointer focus did not set the resize cursor");
    wl_pointer_set_cursor(right_seat->pointer, right_seat->pointer_serial, nullptr, 4, 2);
    round_trip(server, right.display);
    check(!scene.last_cursor_shape && !scene.last_cursor_surface && scene.hotspot_x == 4 &&
              scene.hotspot_y == 2,
          "focused client could not hide its cursor");

    relative(&pointer, -100, -100);
    round_trip(server, left.display);
    wl_pointer_set_cursor(left_seat->pointer, left_seat->pointer_serial, cursor, 8, 7);
    round_trip(server, left.display);
    check(scene.last_cursor_surface, "cursor surface could not be restored after refocus");
    wl_surface_destroy(cursor);
    round_trip(server, left.display);
    check(!scene.last_cursor_surface && !scene.last_cursor_shape,
          "destroying the active cursor surface did not restore the desktop arrow");

    wp_cursor_shape_device_v1_destroy(left_device);
    wp_cursor_shape_device_v1_destroy(right_device);
    disconnect_pointer_client(server, &left);
    disconnect_pointer_client(server, &right);
    wl_display_destroy_clients(server);
    scene.input.reset();
    wlr_pointer_finish(&pointer);
    wlr_keyboard_finish(&keyboard);
    wlr_backend_finish(&backend);
    wl_display_destroy(server);
}

static void exercise_wayland_pointer(void) {
    struct wl_display *server = wl_display_create();
    struct wlr_backend backend = {};
    struct wlr_keyboard keyboard = {};
    struct wlr_pointer pointer = {};
    pointer_scene scene{};
    scene.x = scene.y = 10;
    create_pointer_seat(server, &backend, &keyboard, &pointer, &scene);

    struct pointer_client left = {}, right = {};
    scene.surfaces[0] = connect_pointer_client(server, &left);
    scene.surfaces[1] = connect_pointer_client(server, &right);
    scene.input->set_focus(scene.surfaces[0]);
    scene.input->refresh_pointer();
    relative(&pointer, 100, 90);
    axis(&pointer);
    round_trip(server, left.display);
    round_trip(server, right.display);
    check(left.events.enters == 1 && left.events.pointer_enters == 1 &&
              left.events.pointer_leaves == 1 && left.events.pointer_axes == 0 &&
              right.events.enters == 0 && right.events.pointer_enters == 1 &&
              right.events.pointer_axes == 1 && right.events.pointer_frames > 0 &&
              right.events.axis_source == WL_POINTER_AXIS_SOURCE_FINGER &&
              right.events.axis_orientation == WL_POINTER_AXIS_VERTICAL_SCROLL &&
              right.events.axis_delta == 3 && right.events.axis_discrete == 0 &&
              right.events.pointer_x == 10 && right.events.pointer_y == 10,
          "hover or background scrolling changed keyboard focus or reached the wrong surface");

    struct zwp_locked_pointer_v1 *lock = zwp_pointer_constraints_v1_lock_pointer(
        right.events.constraints, right.surface, right.events.pointer, nullptr,
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
    scene.input->set_inverse_scroll(true);
    axis(&pointer);
    button(&pointer, BTN_LEFT, false);
    round_trip(server, right.display);
    check(scene.x == 110 && right.events.pointer_motions == old_regular &&
              right.events.relative_motions == old_relative + 1 &&
              right.events.relative_dx == -125 && right.events.unaccelerated_dx == -111 &&
              right.events.pointer_axes == 2 && right.events.axis_delta == -3 &&
              right.events.pointer_buttons == 2,
          "pointer lock did not suppress motion or failed to preserve raw deltas/axis/buttons");
    struct wlr_pointer_axis_event wheel = {
        .pointer = &pointer,
        .time_msec = 14,
        .source = WL_POINTER_AXIS_SOURCE_WHEEL,
        .orientation = WL_POINTER_AXIS_HORIZONTAL_SCROLL,
        .relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
        .delta = 9,
        .delta_discrete = WLR_POINTER_AXIS_DISCRETE_STEP,
    };
    wl_signal_emit_mutable(&pointer.events.axis, &wheel);
    wl_signal_emit_mutable(&pointer.events.frame, &pointer);
    round_trip(server, right.display);
    check(right.events.axis_source == WL_POINTER_AXIS_SOURCE_WHEEL &&
              right.events.axis_orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL &&
              right.events.axis_delta == -9 && right.events.axis_discrete == -1,
          "inverse wheel scrolling did not invert both axis values");
    scene.input->set_inverse_scroll(false);
    axis(&pointer);
    round_trip(server, right.display);
    check(right.events.axis_delta == 3, "normal scrolling did not restore the original direction");
    scene.input->set_focus(scene.surfaces[0]);
    round_trip(server, right.display);
    check(right.events.unlocks == 1 && !scene.locked,
          "pointer lock survived the focus change to another client");
    zwp_locked_pointer_v1_destroy(lock);

    scene.input->set_focus(scene.surfaces[1]);
    lock = zwp_pointer_constraints_v1_lock_pointer(right.events.constraints, right.surface,
                                                   right.events.pointer, nullptr,
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
    check(scene.x > 125 && scene.x < 130 && scene.motion_kind == SEASIDE_POINTER_CONFINED,
          "relative motion escaped pointer confinement");
    struct wlr_pointer_motion_absolute_event absolute = {
        .pointer = &pointer, .time_msec = 16, .x = 0.1, .y = 0.1};
    wl_signal_emit_mutable(&pointer.events.motion_absolute, &absolute);
    wl_signal_emit_mutable(&pointer.events.frame, &pointer);
    round_trip(server, right.display);
    check(scene.x >= 100 && scene.x < 105 && right.events.unconfines == 0 &&
              scene.motion_kind == SEASIDE_POINTER_CONFINED,
          "absolute motion escaped pointer confinement");

    struct wl_region *empty = wl_compositor_create_region(right.events.compositor);
    zwp_confined_pointer_v1_set_region(confine, empty);
    round_trip(server, right.display);
    check(right.events.unconfines == 0, "uncommitted empty region deactivated confinement");
    wl_surface_commit(right.surface);
    round_trip(server, right.display);
    check(right.events.unconfines == 1, "committed empty region fell back to the input region");
    zwp_confined_pointer_v1_set_region(confine, nullptr);
    round_trip(server, right.display);
    check(right.events.confines == 1, "uncommitted null region reactivated confinement");
    wl_surface_commit(right.surface);
    round_trip(server, right.display);
    check(right.events.confines == 2, "committed null region did not restore confinement");

    zwp_confined_pointer_v1_destroy(confine);
    confine = zwp_pointer_constraints_v1_confine_pointer(
        right.events.constraints, right.surface, right.events.pointer, empty,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_confined_pointer_v1_add_listener(confine, &confine_listener, &right.events);
    wl_region_destroy(empty);
    round_trip(server, right.display);
    check(right.events.confines == 2, "initially empty region activated confinement");
    zwp_confined_pointer_v1_destroy(confine);
    round_trip(server, right.display);
    relative(&pointer, -60, -60);
    round_trip(server, left.display);
    check(left.events.pointer_enters == 3, "destroying constraint left pointer focus stuck");

    // Losing the initiating pointer cancels a gesture even if a second
    // pointer is still connected and coincidentally holding the same button.
    struct wlr_pointer second = {};
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
    scene.input.reset();
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

static void source_target(void *data, struct wl_data_source *source, const char *type) noexcept {
    (void)data;
    (void)source;
    (void)type;
}

static void source_send(void *data, struct wl_data_source *source, const char *type,
                        int32_t fd) noexcept {
    (void)source;
    write_payload(static_cast<test_source *>(data), type, fd);
}

static void source_cancelled(void *data, struct wl_data_source *source) noexcept {
    (void)source;
    ++static_cast<test_source *>(data)->cancelled;
}

static void source_dropped(void *data, struct wl_data_source *source) noexcept {
    (void)source;
    ++static_cast<test_source *>(data)->dropped;
}

static void source_finished(void *data, struct wl_data_source *source) noexcept {
    (void)source;
    ++static_cast<test_source *>(data)->finished;
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action) noexcept {
    (void)data;
    (void)source;
    check(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE ||
              action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
          "unexpected source drag action");
}

static struct wl_data_source *create_data_source(struct data_client *client,
                                                 struct test_source *observation) noexcept {
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
                                const char *type, int32_t fd) noexcept {
    (void)source;
    write_payload(static_cast<test_source *>(data), type, fd);
}

static void primary_source_cancelled(void *data,
                                     struct zwp_primary_selection_source_v1 *source) noexcept {
    (void)source;
    ++static_cast<test_source *>(data)->cancelled;
}

static struct zwp_primary_selection_source_v1 *
create_primary_source(struct data_client *client, struct test_source *observation) noexcept {
    static const struct zwp_primary_selection_source_v1_listener listener = {
        .send = primary_source_send, .cancelled = primary_source_cancelled};
    struct zwp_primary_selection_source_v1 *source =
        zwp_primary_selection_device_manager_v1_create_source(client->primary_manager);
    zwp_primary_selection_source_v1_add_listener(source, &listener, observation);
    zwp_primary_selection_source_v1_offer(source, mime);
    return source;
}

static void data_offer_type(void *data, struct wl_data_offer *offer, const char *type) noexcept {
    (void)offer;
    check(strcmp(type, mime) == 0, "unexpected data offer MIME type");
    ++static_cast<data_client *>(data)->clipboard_mimes;
}

static void data_offer_actions(void *data, struct wl_data_offer *offer, uint32_t actions) noexcept {
    (void)data;
    (void)offer;
    check(actions == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, "unexpected offered drag action");
}

static void data_offer_action(void *data, struct wl_data_offer *offer, uint32_t action) noexcept {
    (void)data;
    (void)offer;
    check(action == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE ||
              action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
          "unexpected negotiated drag action");
}

static void data_offer(void *data, struct wl_data_device *device,
                       struct wl_data_offer *offer) noexcept {
    (void)device;
    static const struct wl_data_offer_listener listener = {.offer = data_offer_type,
                                                           .source_actions = data_offer_actions,
                                                           .action = data_offer_action};
    wl_data_offer_add_listener(offer, &listener, data);
}

static void data_enter(void *data, struct wl_data_device *device, uint32_t serial,
                       struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *offer) noexcept {
    (void)device;
    (void)x;
    (void)y;
    auto *client = static_cast<data_client *>(data);
    check(surface == client->pointer.surface && offer && !client->drag_offer,
          "drag entered an unexpected target");
    client->drag_offer = offer;
    client->enter_serial = serial;
    ++client->enters;
}

static void data_leave(void *data, struct wl_data_device *device) noexcept {
    (void)device;
    auto *client = static_cast<data_client *>(data);
    if (client->drag_offer && client->drops == 0) {
        wl_data_offer_destroy(client->drag_offer);
        client->drag_offer = nullptr;
    }
    ++client->leaves;
}

static void data_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x,
                        wl_fixed_t y) noexcept {
    (void)device;
    (void)time;
    (void)x;
    (void)y;
    ++static_cast<data_client *>(data)->motions;
}

static void data_drop(void *data, struct wl_data_device *device) noexcept {
    (void)device;
    ++static_cast<data_client *>(data)->drops;
}

static void data_selection(void *data, struct wl_data_device *device,
                           struct wl_data_offer *offer) noexcept {
    (void)device;
    auto *client = static_cast<data_client *>(data);
    if (client->selection) {
        wl_data_offer_destroy(client->selection);
    }
    client->selection = offer;
}

static void primary_offer_type(void *data, struct zwp_primary_selection_offer_v1 *offer,
                               const char *type) noexcept {
    (void)offer;
    check(strcmp(type, mime) == 0, "unexpected primary offer MIME type");
    ++static_cast<data_client *>(data)->primary_mimes;
}

static void primary_offer(void *data, struct zwp_primary_selection_device_v1 *device,
                          struct zwp_primary_selection_offer_v1 *offer) noexcept {
    (void)device;
    static const struct zwp_primary_selection_offer_v1_listener listener = {.offer =
                                                                                primary_offer_type};
    zwp_primary_selection_offer_v1_add_listener(offer, &listener, data);
}

static void primary_selection(void *data, struct zwp_primary_selection_device_v1 *device,
                              struct zwp_primary_selection_offer_v1 *offer) noexcept {
    (void)device;
    auto *client = static_cast<data_client *>(data);
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
    check(events->data_device_manager && events->primary_manager_name && events->ext_data_control &&
              events->wlr_data_control,
          "unexpected clipboard protocol globals");
    client->manager = static_cast<wl_data_device_manager *>(wl_registry_bind(
        client->pointer.registry, events->data_manager_name, &wl_data_device_manager_interface, 3));
    client->device = wl_data_device_manager_get_data_device(client->manager, events->seat);
    wl_data_device_add_listener(client->device, &device_listener, client);
    client->primary_manager = static_cast<zwp_primary_selection_device_manager_v1 *>(
        wl_registry_bind(client->pointer.registry, events->primary_manager_name,
                         &zwp_primary_selection_device_manager_v1_interface, 1));
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
    check((offer != nullptr) != (primary != nullptr), "missing selection or drag offer");
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
    struct wlr_backend backend = {};
    struct wlr_keyboard keyboard = {};
    struct wlr_pointer pointer = {};
    pointer_scene scene{};
    scene.x = scene.y = 10;
    create_pointer_seat(server, &backend, &keyboard, &pointer, &scene);
    struct wlr_seat *seat = scene.input->seat();
    struct data_client left = {}, right = {};
    scene.surfaces[0] = connect_data_client(server, &left);
    scene.surfaces[1] = connect_data_client(server, &right);
    struct wl_display *sender = left.pointer.display, *receiver = right.pointer.display;

    scene.input->set_focus(scene.surfaces[0]);
    const uint32_t left_serial = data_key_serial(server, &left, &keyboard);
    test_source clipboard{};
    clipboard.payload = "clipboard";
    test_source primary{};
    primary.payload = "primary";
    struct wl_data_source *clipboard_source = create_data_source(&left, &clipboard);
    struct zwp_primary_selection_source_v1 *primary_source = create_primary_source(&left, &primary);
    wl_data_device_set_selection(left.device, clipboard_source, left_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, primary_source, left_serial);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(left.selection && left.primary && !right.selection && !right.primary,
          "unfocused client received a clipboard or primary offer");

    const uint32_t delayed_serial = data_key_serial(server, &left, &keyboard);
    scene.input->set_focus(scene.surfaces[1]);
    data_key_serial(server, &right, &keyboard);
    check(right.clipboard_mimes == 1 && right.primary_mimes == 1,
          "focused client did not receive the selection MIME offers");
    wl_data_device_set_selection(right.device, nullptr, left_serial);
    zwp_primary_selection_device_v1_set_selection(right.primary_device, nullptr, left_serial);
    round_trip(server, receiver);
    check(right.selection && right.primary, "foreign serial cleared a focused selection");

    expect_payload(server, &left, &right, right.selection, nullptr, clipboard.payload);
    expect_payload(server, &left, &right, nullptr, right.primary, primary.payload);
    check(clipboard.sends == 1 && primary.sends == 1, "selection sources were not each read once");

    // X11 MIME negotiation can finish after the user already changed focus.
    test_source later_clipboard{};
    later_clipboard.payload = "delayed clipboard";
    test_source later_primary{};
    later_primary.payload = "delayed primary";
    struct wl_data_source *delayed_clipboard = create_data_source(&left, &later_clipboard);
    struct zwp_primary_selection_source_v1 *delayed_primary =
        create_primary_source(&left, &later_primary);
    wl_data_device_set_selection(left.device, delayed_clipboard, delayed_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, delayed_primary,
                                                  delayed_serial);
    wl_data_device_set_selection(left.device, nullptr, left_serial);
    zwp_primary_selection_device_v1_set_selection(left.primary_device, nullptr, left_serial);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(seat->keyboard_state.focused_surface == scene.surfaces[1] && clipboard.cancelled == 1 &&
              primary.cancelled == 1,
          "delayed selection did not replace its predecessor with focus preserved");
    expect_payload(server, &left, &right, right.selection, nullptr, later_clipboard.payload);
    expect_payload(server, &left, &right, nullptr, right.primary, later_primary.payload);

    wl_data_source_destroy(clipboard_source);
    zwp_primary_selection_source_v1_destroy(primary_source);
    wl_data_source_destroy(delayed_clipboard);
    zwp_primary_selection_source_v1_destroy(delayed_primary);
    round_trip(server, sender);
    round_trip(server, receiver);
    check(!right.selection && !right.primary, "source teardown left a cached selection");

    scene.input->refresh_pointer();
    test_source drag{};
    drag.payload = "dropped";
    struct wl_data_source *drag_source = create_data_source(&left, &drag);
    const uint32_t copy = WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
    wl_data_source_set_actions(drag_source, copy);
    wl_data_device_start_drag(left.device, drag_source, left.pointer.surface, nullptr, left_serial);
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
    struct wlr_pointer other = {};
    wlr_pointer_init(&other, &pointer_impl, "second drag test pointer");
    wl_signal_emit_mutable(&backend.events.new_input, &other.base);
    button(&other, BTN_LEFT, true);
    button(&pointer, BTN_LEFT, false);
    round_trip(server, receiver);
    check(!seat->drag && right.drops == 1 && right.leaves == 1 && right.drag_offer,
          "drop did not finish the grab and retain a transfer offer");
    button(&other, BTN_LEFT, false);
    expect_payload(server, &left, &right, right.drag_offer, nullptr, drag.payload);
    wl_data_offer_finish(right.drag_offer);
    wl_data_offer_destroy(right.drag_offer);
    right.drag_offer = nullptr;
    right.drops = 0;
    round_trip(server, receiver);
    round_trip(server, sender);
    check(drag.dropped == 1 && drag.finished == 1, "source did not observe drop completion");
    const unsigned previous_cancels = drag.cancelled;
    wl_surface_destroy(icon);
    wl_data_source_destroy(drag_source);

    relative(&pointer, -110, -110);
    drag_source = start_data_drag(server, &left, &drag, &pointer, nullptr);
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
    drag_source = start_data_drag(server, &left, &drag, &other, nullptr);
    relative(&other, 110, 110);
    round_trip(server, receiver);
    check(right.drag_offer && seat->drag && seat->drag->focus == scene.surfaces[1],
          "third drag did not focus the target about to be destroyed");
    wl_surface_destroy(right.pointer.surface);
    right.pointer.surface = nullptr;
    scene.surfaces[1] = nullptr;
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
    scene.input.reset();
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
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "cursor") == 0)) {
        exercise_wayland_cursor();
    }
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "data-device") == 0)) {
        exercise_data_device();
    }
    if (argc > 2 ||
        (argc == 2 && strcmp(argv[1], "keyboard") != 0 && strcmp(argv[1], "pointer") != 0 &&
         strcmp(argv[1], "cursor") != 0 && strcmp(argv[1], "data-device") != 0)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

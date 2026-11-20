#include "tty_input.h"

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

struct observation {
    unsigned motions;
    unsigned exits;
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

int main(void) {
    exercise(true);
    exercise(false);
    return EXIT_SUCCESS;
}

#define _POSIX_C_SOURCE 200809L
#include "tty_input.h"

#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <xkbcommon/xkbcommon-keysyms.h>

struct seaside_tty_input {
    struct seaside_tty_input_callbacks callbacks;
    struct seaside_tty_input_snapshot snapshot;
    void *data;
    struct wl_list devices;
    struct wl_listener new_input;
    struct wl_listener backend_destroy;
    bool attached;
    struct wlr_seat *seat;
    struct wlr_keyboard_group *group;
    struct xkb_state *shortcut_state;
    /* wlroots uses the seat keyboard's key list for late wl_keyboard binds.
     * Keep it separate so a held, consumed shortcut never enters that list. */
    struct wlr_keyboard client_keyboard;
    struct wlr_surface *focus;
    struct wl_listener seat_destroy;
    struct wl_listener focus_destroy;
    struct wl_listener group_key;
    struct wl_listener group_modifiers;
    struct wl_listener group_enter;
    struct wl_listener group_leave;
    bool consumed[KEY_CNT];
};

struct tty_device {
    struct seaside_tty_input *input;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener event;
    struct wl_listener absolute_motion;
    struct wlr_keyboard *keyboard;
    bool direct_key;
};

static void refresh_focus(struct seaside_tty_input *input) {
    if (!input->seat) {
        return;
    }
    if (!input->focus || input->snapshot.keyboards == 0) {
        wlr_seat_keyboard_notify_clear_focus(input->seat);
        return;
    }
    struct wlr_keyboard *keyboard = &input->client_keyboard;
    wlr_seat_keyboard_notify_enter(input->seat, input->focus, keyboard->keycodes,
                                   keyboard->num_keycodes, &keyboard->modifiers);
}

void seaside_tty_input_set_focus(struct seaside_tty_input *input, struct wlr_surface *surface) {
    if (!input || !input->seat || input->focus == surface) {
        return;
    }
    wl_list_remove(&input->focus_destroy.link);
    wl_list_init(&input->focus_destroy.link);
    input->focus = surface;
    if (surface) {
        wl_signal_add(&surface->events.destroy, &input->focus_destroy);
    }
    refresh_focus(input);
}

static void handle_focus_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, focus_destroy);
    seaside_tty_input_set_focus(input, NULL);
}

static void refresh_keyboard_capability(struct seaside_tty_input *input) {
    if (!input->seat) {
        return;
    }
    if (input->snapshot.keyboards > 0) {
        wlr_seat_set_keyboard(input->seat, &input->client_keyboard);
        wlr_seat_set_capabilities(input->seat, WL_SEAT_CAPABILITY_KEYBOARD);
        refresh_focus(input);
    } else {
        refresh_focus(input);
        wlr_seat_set_capabilities(input->seat, 0);
        wlr_seat_set_keyboard(input->seat, NULL);
    }
}

static void remove_device(struct tty_device *device) {
    struct seaside_tty_input *input = device->input;
    wl_list_remove(&device->link);
    wl_list_remove(&device->destroy.link);
    if (!device->keyboard) {
        wl_list_remove(&device->event.link);
        wl_list_remove(&device->absolute_motion.link);
        --input->snapshot.pointers;
    } else {
        if (device->direct_key) {
            wl_list_remove(&device->event.link);
        } else if (device->keyboard->group == input->group) {
            wlr_keyboard_group_remove_keyboard(input->group, device->keyboard);
        }
        if (--input->snapshot.keyboards == 0) {
            refresh_keyboard_capability(input);
        }
    }
    free(device);
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct tty_device *device = wl_container_of(listener, device, destroy);
    remove_device(device);
}

static void handle_relative_motion(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, event);
    const struct wlr_pointer_motion_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->callbacks.motion) {
        input->callbacks.motion(input->data, false, event->delta_x, event->delta_y);
    }
}

static void handle_absolute_motion(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, absolute_motion);
    const struct wlr_pointer_motion_absolute_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->callbacks.motion) {
        input->callbacks.motion(input->data, true, event->x, event->y);
    }
}

static void handle_cursor_fixture_key(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, event);
    const struct wlr_keyboard_key_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->callbacks.exit_requested && event->keycode == KEY_ESC &&
        event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        input->callbacks.exit_requested(input->data);
    }
}

static bool launch_shortcut(struct seaside_tty_input *input, uint32_t keycode) {
    struct wlr_keyboard *keyboard = &input->group->keyboard;
    return input->callbacks.launch_requested &&
           wlr_keyboard_get_modifiers(keyboard) == WLR_MODIFIER_LOGO &&
           xkb_state_key_get_one_sym(keyboard->xkb_state, keycode + 8) == XKB_KEY_Return;
}

static bool configured_shortcut(struct seaside_tty_input *input, uint32_t keycode) {
    if (!input->shortcut_state) {
        return false;
    }
    const struct wlr_keyboard *keyboard = &input->group->keyboard;
    const xkb_mod_mask_t active =
        xkb_state_serialize_mods(input->shortcut_state, XKB_STATE_MODS_DEPRESSED) |
        xkb_state_serialize_mods(input->shortcut_state, XKB_STATE_MODS_LATCHED);
    uint32_t state = 0;
    for (size_t i = 0; i < WLR_MODIFIER_COUNT; ++i) {
        const xkb_mod_index_t index = keyboard->mod_indexes[i];
        if (index < sizeof(active) * CHAR_BIT && (active & (1u << index)) != 0) {
            state |= 1u << i;
        }
    }
    if ((state & (WLR_MODIFIER_MOD3 | WLR_MODIFIER_MOD5)) != 0) {
        return false;
    }
    const uint8_t modifiers =
        ((state & WLR_MODIFIER_LOGO) ? 1 : 0) | ((state & WLR_MODIFIER_CTRL) ? 2 : 0) |
        ((state & WLR_MODIFIER_ALT) ? 4 : 0) | ((state & WLR_MODIFIER_SHIFT) ? 8 : 0);
    /* Ignore lock modifier bits while using the effective XKB keysym. */
    const uint32_t keysym = xkb_state_key_get_one_sym(input->shortcut_state, keycode + 8);
    return input->callbacks.shortcut_pressed(input->data, modifiers, keysym);
}

static void deliver_key(struct seaside_tty_input *input, struct wlr_keyboard_key_event event) {
    if (event.keycode >= KEY_CNT) {
        return;
    }
    const bool pressed = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (input->consumed[event.keycode]) {
        input->consumed[event.keycode] = pressed;
        return;
    }
    struct wlr_keyboard *keyboard = &input->client_keyboard;
    bool already_down = false;
    for (size_t i = 0; i < keyboard->num_keycodes; ++i) {
        if (keyboard->keycodes[i] == event.keycode) {
            already_down = true;
            break;
        }
    }
    if (pressed == already_down || (pressed && keyboard->num_keycodes == WLR_KEYBOARD_KEYS_CAP)) {
        return;
    }
    /* XKB state comes from the hardware group, while the seat keyboard needs
     * the visible pressed keys for focus changes and late keyboard binds. */
    event.update_state = false;
    wlr_keyboard_notify_key(keyboard, &event);
    if (input->seat && input->seat->keyboard_state.focused_client &&
        !wl_list_empty(&input->seat->keyboard_state.focused_client->keyboards)) {
        wlr_seat_keyboard_notify_key(input->seat, event.time_msec, event.keycode, event.state);
        ++input->snapshot.forwarded_key_events;
        if (pressed) {
            ++input->snapshot.forwarded_key_presses;
        }
    }
}

static void handle_group_key(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, group_key);
    const struct wlr_keyboard_key_event *event = data;
    if (event->keycode >= KEY_CNT) {
        return;
    }
    const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    const bool handled =
        pressed && !input->consumed[event->keycode] && configured_shortcut(input, event->keycode);
    if (input->shortcut_state) {
        /* Device modifier masks can outlive a removed group member. Shortcuts
         * follow the aggregate key transitions, including synthetic releases. */
        xkb_state_update_key(input->shortcut_state, event->keycode + 8,
                             pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    }
    if (pressed && !input->consumed[event->keycode]) {
        if (handled) {
            input->consumed[event->keycode] = true;
            return;
        }
        if (launch_shortcut(input, event->keycode)) {
            input->consumed[event->keycode] = true;
            ++input->snapshot.launch_requests;
            input->callbacks.launch_requested(input->data);
            return;
        }
    }
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && !event->update_state) {
        /* Input device teardown in wlroots emits releases without updating
         * XKB. The aggregate keyboard survives unplugging a member. */
        xkb_state_update_key(input->group->keyboard.xkb_state, event->keycode + 8, XKB_KEY_UP);
    }
    deliver_key(input, *event);
}

static void deliver_modifiers(struct seaside_tty_input *input,
                              const struct wlr_keyboard_modifiers *mods) {
    wlr_keyboard_notify_modifiers(&input->client_keyboard, mods->depressed, mods->latched,
                                  mods->locked, mods->group);
    if (input->seat) {
        wlr_seat_keyboard_notify_modifiers(input->seat, &input->client_keyboard.modifiers);
    }
}

static void handle_group_modifiers(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, group_modifiers);
    const struct wlr_keyboard *keyboard = data;
    deliver_modifiers(input, &keyboard->modifiers);
}

static void handle_group_enter(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, group_enter);
    if (!input->shortcut_state) {
        return;
    }
    struct wl_array *keys = data;
    const uint32_t *key;
    wl_array_for_each(key, keys) {
        xkb_state_update_key(input->shortcut_state, *key + 8, XKB_KEY_DOWN);
    }
}

static void handle_group_leave(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, group_leave);
    struct wl_array *keys = data;
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    const uint32_t time_msec =
        (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
    const uint32_t *key;
    wl_array_for_each(key, keys) {
        if (input->shortcut_state) {
            xkb_state_update_key(input->shortcut_state, *key + 8, XKB_KEY_UP);
        }
        deliver_key(input, (struct wlr_keyboard_key_event){
                               .time_msec = time_msec,
                               .keycode = *key,
                               .state = WL_KEYBOARD_KEY_STATE_RELEASED,
                           });
    }
    deliver_modifiers(input, &input->group->keyboard.modifiers);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, new_input);
    struct wlr_input_device *base = data;
    if (base->type != WLR_INPUT_DEVICE_POINTER && base->type != WLR_INPUT_DEVICE_KEYBOARD) {
        return;
    }
    struct tty_device *device = calloc(1, sizeof(*device));
    if (!device) {
        input->snapshot.failed = true;
        return;
    }
    device->input = input;
    if (base->type == WLR_INPUT_DEVICE_KEYBOARD && input->group) {
        device->keyboard = wlr_keyboard_from_input_device(base);
        if (!wlr_keyboard_set_keymap(device->keyboard, input->group->keyboard.keymap) ||
            !wlr_keyboard_group_add_keyboard(input->group, device->keyboard)) {
            input->snapshot.failed = true;
            free(device);
            return;
        }
    }
    device->destroy.notify = handle_device_destroy;
    wl_signal_add(&base->events.destroy, &device->destroy);
    wl_list_insert(&input->devices, &device->link);

    if (base->type == WLR_INPUT_DEVICE_POINTER) {
        struct wlr_pointer *pointer = wlr_pointer_from_input_device(base);
        device->event.notify = handle_relative_motion;
        device->absolute_motion.notify = handle_absolute_motion;
        wl_signal_add(&pointer->events.motion, &device->event);
        wl_signal_add(&pointer->events.motion_absolute, &device->absolute_motion);
        ++input->snapshot.pointers;
    } else {
        if (!input->group) {
            device->keyboard = wlr_keyboard_from_input_device(base);
            device->direct_key = true;
            device->event.notify = handle_cursor_fixture_key;
            wl_signal_add(&device->keyboard->events.key, &device->event);
        }
        if (++input->snapshot.keyboards == 1) {
            refresh_keyboard_capability(input);
        }
    }
}

static void detach(struct seaside_tty_input *input) {
    if (!input->attached) {
        return;
    }
    input->attached = false;
    seaside_tty_input_set_focus(input, NULL);
    wl_list_remove(&input->new_input.link);
    wl_list_remove(&input->backend_destroy.link);
    /* Backend and per-device destroy signals can arrive in either order.
     * Remove all surviving listeners before the backend can reclaim its devices. */
    struct tty_device *device, *next;
    wl_list_for_each_safe(device, next, &input->devices, link) {
        remove_device(device);
    }
}

static void handle_backend_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, backend_destroy);
    detach(input);
}

struct seaside_tty_input *
seaside_tty_input_create(struct wlr_backend *backend,
                         const struct seaside_tty_input_callbacks *callbacks, void *data) {
    if (!backend || !callbacks) {
        return NULL;
    }
    struct seaside_tty_input *input = calloc(1, sizeof(*input));
    if (!input) {
        return NULL;
    }
    input->callbacks = *callbacks;
    input->data = data;
    wl_list_init(&input->devices);
    wl_list_init(&input->focus_destroy.link);
    input->new_input.notify = handle_new_input;
    input->backend_destroy.notify = handle_backend_destroy;
    wl_signal_add(&backend->events.new_input, &input->new_input);
    wl_signal_add(&backend->events.destroy, &input->backend_destroy);
    input->attached = true;
    return input;
}

static const struct wlr_keyboard_impl client_keyboard_impl = {.name = "seaside-seat-keyboard"};

static void handle_seat_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, seat_destroy);
    wl_list_remove(&input->seat_destroy.link);
    wl_list_remove(&input->focus_destroy.link);
    wl_list_init(&input->focus_destroy.link);
    input->focus = NULL;
    input->seat = NULL;
}

bool seaside_tty_input_enable_keyboard(struct seaside_tty_input *input,
                                       struct wl_display *display) {
    if (!input || !display || !input->attached || input->group || input->snapshot.keyboards > 0) {
        return false;
    }
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        return false;
    }
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(context);
    if (!keymap) {
        return false;
    }

    struct wlr_keyboard_group *group = wlr_keyboard_group_create();
    wlr_keyboard_init(&input->client_keyboard, &client_keyboard_impl, "Seaside keyboard");
    struct xkb_state *shortcut_state =
        input->callbacks.shortcut_pressed ? xkb_state_new(keymap) : NULL;
    bool mapped = group && (!input->callbacks.shortcut_pressed || shortcut_state) &&
                  wlr_keyboard_set_keymap(&group->keyboard, keymap) &&
                  wlr_keyboard_set_keymap(&input->client_keyboard, keymap);
    xkb_keymap_unref(keymap);
    struct wlr_seat *seat = mapped ? wlr_seat_create(display, "seat0") : NULL;
    if (!seat || !wlr_data_device_manager_create(display)) {
        xkb_state_unref(shortcut_state);
        wlr_seat_destroy(seat);
        if (group) {
            wlr_keyboard_group_destroy(group);
        }
        wlr_keyboard_finish(&input->client_keyboard);
        return false;
    }

    input->seat = seat;
    input->group = group;
    input->shortcut_state = shortcut_state;
    input->seat_destroy.notify = handle_seat_destroy;
    input->focus_destroy.notify = handle_focus_destroy;
    input->group_key.notify = handle_group_key;
    input->group_modifiers.notify = handle_group_modifiers;
    input->group_enter.notify = handle_group_enter;
    input->group_leave.notify = handle_group_leave;
    wl_signal_add(&seat->events.destroy, &input->seat_destroy);
    wl_signal_add(&group->keyboard.events.key, &input->group_key);
    wl_signal_add(&group->keyboard.events.modifiers, &input->group_modifiers);
    wl_signal_add(&group->events.enter, &input->group_enter);
    wl_signal_add(&group->events.leave, &input->group_leave);
    return true;
}

void seaside_tty_input_destroy(struct seaside_tty_input *input) {
    if (input) {
        detach(input);
        wlr_seat_destroy(input->seat);
        if (input->group) {
            wl_list_remove(&input->group_key.link);
            wl_list_remove(&input->group_modifiers.link);
            wl_list_remove(&input->group_enter.link);
            wl_list_remove(&input->group_leave.link);
            wlr_keyboard_group_destroy(input->group);
            wlr_keyboard_finish(&input->client_keyboard);
            xkb_state_unref(input->shortcut_state);
        }
        free(input);
    }
}

struct seaside_tty_input_snapshot
seaside_tty_input_get_snapshot(const struct seaside_tty_input *input) {
    if (!input) {
        return (struct seaside_tty_input_snapshot){0};
    }
    struct seaside_tty_input_snapshot snapshot = input->snapshot;
    snapshot.has_keyboard_focus = input->seat && input->seat->keyboard_state.focused_surface;
    return snapshot;
}

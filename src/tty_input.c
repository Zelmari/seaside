#include "tty_input.h"

#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>

struct seaside_tty_input {
    struct seaside_tty_input_callbacks callbacks;
    struct seaside_tty_input_snapshot snapshot;
    void *data;
    struct wl_list devices;
    struct wl_listener new_input;
    struct wl_listener backend_destroy;
    bool attached;
};

struct tty_device {
    struct seaside_tty_input *input;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener event;
    struct wl_listener absolute_motion;
    bool pointer;
};

static void remove_device(struct tty_device *device) {
    wl_list_remove(&device->link);
    wl_list_remove(&device->destroy.link);
    wl_list_remove(&device->event.link);
    if (device->pointer) {
        wl_list_remove(&device->absolute_motion.link);
        --device->input->snapshot.pointers;
    } else {
        --device->input->snapshot.keyboards;
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

static void handle_key(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, event);
    const struct wlr_keyboard_key_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->callbacks.exit_requested && event->keycode == KEY_ESC &&
        event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        input->callbacks.exit_requested(input->data);
    }
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
    device->pointer = base->type == WLR_INPUT_DEVICE_POINTER;
    device->destroy.notify = handle_device_destroy;
    wl_signal_add(&base->events.destroy, &device->destroy);
    wl_list_insert(&input->devices, &device->link);

    if (device->pointer) {
        struct wlr_pointer *pointer = wlr_pointer_from_input_device(base);
        device->event.notify = handle_relative_motion;
        device->absolute_motion.notify = handle_absolute_motion;
        wl_signal_add(&pointer->events.motion, &device->event);
        wl_signal_add(&pointer->events.motion_absolute, &device->absolute_motion);
        ++input->snapshot.pointers;
    } else {
        struct wlr_keyboard *keyboard = wlr_keyboard_from_input_device(base);
        device->event.notify = handle_key;
        wl_signal_add(&keyboard->events.key, &device->event);
        ++input->snapshot.keyboards;
    }
}

static void detach(struct seaside_tty_input *input) {
    if (!input->attached) {
        return;
    }
    input->attached = false;
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
    input->new_input.notify = handle_new_input;
    input->backend_destroy.notify = handle_backend_destroy;
    wl_signal_add(&backend->events.new_input, &input->new_input);
    wl_signal_add(&backend->events.destroy, &input->backend_destroy);
    input->attached = true;
    return input;
}

void seaside_tty_input_destroy(struct seaside_tty_input *input) {
    if (input) {
        detach(input);
        free(input);
    }
}

struct seaside_tty_input_snapshot
seaside_tty_input_get_snapshot(const struct seaside_tty_input *input) {
    return input ? input->snapshot : (struct seaside_tty_input_snapshot){0};
}

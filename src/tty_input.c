#define _POSIX_C_SOURCE 200809L
#include "tty_input.h"

#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/region.h>
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
    uint8_t modifiers;
    double sensitivity;
    struct tty_device *gesture_device;
    uint32_t gesture_button;
    bool gesture_released;
    size_t consumed_button_count;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_pointer_constraints_v1 *constraints;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener relative_pointer_destroy;
    struct wl_listener constraints_destroy;
    struct wl_listener new_constraint;
    struct wl_listener constraint_destroy;
    struct wl_listener pointer_focus_change;
    struct wl_listener pointer_surface_commit;
    struct wl_listener pointer_grab_begin;
    struct wl_listener pointer_grab_end;
    struct wl_listener keyboard_grab_end;
    struct wl_listener request_selection;
    struct wl_listener request_primary_selection;
    struct wl_listener request_drag;
    struct wl_listener drag_destroy;
    struct wl_listener drag_origin_destroy;
    struct tty_device *pointer_grab_device;
};

struct tty_device {
    struct seaside_tty_input *input;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener event;
    struct wl_listener absolute_motion;
    struct wl_listener button;
    struct wl_listener axis;
    struct wl_listener frame;
    struct wlr_keyboard *keyboard;
    struct libinput_device *pointer_device;
    bool consumed_buttons[KEY_CNT];
    bool suppressed_drag_buttons[KEY_CNT];
    bool drag_release_pending;
    uint32_t drag_release_button, drag_release_time;
    bool direct_key;
};

static uint32_t monotonic_msec(void) {
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

static bool pointer_locked(const struct seaside_tty_input *input) {
    return input->constraint && input->constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

static void leave_constraint(struct seaside_tty_input *input, bool notify_client) {
    if (!input->constraint) {
        return;
    }
    const bool locked = pointer_locked(input);
    struct wlr_pointer_constraint_v1 *constraint = input->constraint;
    input->constraint = NULL;
    wl_list_remove(&input->constraint_destroy.link);
    if (notify_client) {
        // Deactivating a one-shot constraint destroys it synchronously.
        wlr_pointer_constraint_v1_send_deactivated(constraint);
    }
    if (locked && input->callbacks.pointer_locked) {
        input->callbacks.pointer_locked(input->data, false);
    }
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, constraint_destroy);
    leave_constraint(input, false);
}

static pixman_region32_t *constraint_region(struct wlr_pointer_constraint_v1 *constraint) {
    // wlroots intersects an explicit region immediately, but populates its
    // cached default region on the next surface commit.
    return pixman_region32_not_empty(&constraint->current.region)
               ? &constraint->region
               : &constraint->surface->input_region;
}

static bool constraint_contains(struct wlr_pointer_constraint_v1 *constraint, double sx,
                                double sy) {
    return sx >= 0 && sy >= 0 && sx < INT32_MAX && sy < INT32_MAX &&
           pixman_region32_contains_point(constraint_region(constraint), (int32_t)sx, (int32_t)sy,
                                          NULL);
}

static void refresh_constraint(struct seaside_tty_input *input) {
    if (!input->constraints || !input->seat) {
        return;
    }
    struct wlr_seat *seat = input->seat;
    const struct wlr_seat_pointer_state *pointer = &seat->pointer_state;
    struct wlr_surface *focus = seat->keyboard_state.focused_surface;
    struct wlr_pointer_constraint_v1 *candidate = NULL;
    // Hover scrolling must not let a background window acquire a pointer lock.
    if (input->attached && focus && pointer->focused_surface &&
        wlr_surface_get_root_surface(pointer->focused_surface) ==
            wlr_surface_get_root_surface(focus) &&
        !wlr_seat_pointer_has_grab(seat) && input->consumed_button_count == 0) {
        candidate = wlr_pointer_constraints_v1_constraint_for_surface(
            input->constraints, pointer->focused_surface, seat);
        if (candidate && !constraint_contains(candidate, pointer->sx, pointer->sy)) {
            candidate = NULL;
        }
    }
    if (candidate == input->constraint) {
        return;
    }
    if (candidate) {
        struct seaside_tty_pointer_target target = {
            .surface = pointer->button_count ? candidate->surface : NULL,
        };
        if (!input->callbacks.pointer_target(input->data, &target) ||
            target.surface != candidate->surface ||
            !constraint_contains(candidate, target.sx, target.sy)) {
            candidate = NULL;
        }
    }
    leave_constraint(input, true);
    if (candidate) {
        input->constraint = candidate;
        input->constraint_destroy.notify = handle_constraint_destroy;
        wl_signal_add(&candidate->events.destroy, &input->constraint_destroy);
        wlr_pointer_constraint_v1_send_activated(candidate);
        if (pointer_locked(input) && input->callbacks.pointer_locked) {
            input->callbacks.pointer_locked(input->data, true);
        }
    }
}

static void clear_pointer_surface_watch(struct seaside_tty_input *input) {
    wl_list_remove(&input->pointer_surface_commit.link);
    wl_list_init(&input->pointer_surface_commit.link);
}

static void watch_pointer_surface(struct seaside_tty_input *input, struct wlr_surface *surface) {
    clear_pointer_surface_watch(input);
    if (surface && input->constraints &&
        wlr_pointer_constraints_v1_constraint_for_surface(input->constraints, surface,
                                                          input->seat)) {
        wl_signal_add(&surface->events.commit, &input->pointer_surface_commit);
    }
}

static void handle_pointer_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, pointer_surface_commit);
    // A region change is applied during wlroots' synced-state iteration.
    // Deactivate on the later surface event because this can free a one-shot
    // constraint and unlink its synced state.
    seaside_tty_input_refresh_pointer(input);
    struct wlr_surface *surface = input->seat->pointer_state.focused_surface;
    if (!surface || !wlr_pointer_constraints_v1_constraint_for_surface(input->constraints, surface,
                                                                       input->seat)) {
        clear_pointer_surface_watch(input);
    }
}

static void handle_pointer_focus_change(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, pointer_focus_change);
    const struct wlr_seat_pointer_focus_change_event *event = data;
    if (input->constraint && input->constraint->surface != event->new_surface) {
        leave_constraint(input, true);
    }
    watch_pointer_surface(input, event->new_surface);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, new_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;
    if (constraint->seat == input->seat &&
        constraint->surface == input->seat->pointer_state.focused_surface) {
        watch_pointer_surface(input, constraint->surface);
        refresh_constraint(input);
    }
}

static void handle_constraints_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, constraints_destroy);
    leave_constraint(input, false);
    clear_pointer_surface_watch(input);
    wl_list_remove(&input->constraints_destroy.link);
    wl_list_remove(&input->new_constraint.link);
    input->constraints = NULL;
}

static void handle_relative_pointer_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, relative_pointer_destroy);
    wl_list_remove(&input->relative_pointer_destroy.link);
    input->relative_pointer = NULL;
}

static void refresh_focus(struct seaside_tty_input *input) {
    if (!input->seat) {
        return;
    }
    if (!input->focus || input->snapshot.keyboards == 0) {
        wlr_seat_keyboard_notify_clear_focus(input->seat);
        refresh_constraint(input);
        return;
    }
    struct wlr_keyboard *keyboard = &input->client_keyboard;
    wlr_seat_keyboard_notify_enter(input->seat, input->focus, keyboard->keycodes,
                                   keyboard->num_keycodes, &keyboard->modifiers);
    refresh_constraint(input);
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

struct wlr_seat *seaside_tty_input_get_seat(const struct seaside_tty_input *input) {
    return input ? input->seat : NULL;
}

static void refresh_capabilities(struct seaside_tty_input *input) {
    if (!input->seat) {
        return;
    }
    uint32_t capabilities = input->snapshot.pointers > 0 ? WL_SEAT_CAPABILITY_POINTER : 0;
    if (input->snapshot.keyboards > 0) {
        wlr_seat_set_keyboard(input->seat, &input->client_keyboard);
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
        wlr_seat_set_capabilities(input->seat, capabilities);
        refresh_focus(input);
    } else {
        refresh_focus(input);
        wlr_seat_set_capabilities(input->seat, capabilities);
        wlr_seat_set_keyboard(input->seat, NULL);
    }
}

static void cancel_drag(struct wlr_seat *seat) {
    struct wlr_drag *drag = seat ? seat->drag : NULL;
    if (!drag) {
        return;
    }
    if (drag->source && !drag->dropped) {
        wlr_data_source_destroy(drag->source);
    } else {
        wlr_seat_pointer_end_grab(seat);
    }
}

void seaside_tty_input_cancel_drag(struct seaside_tty_input *input) {
    cancel_drag(input ? input->seat : NULL);
}

static void remove_device(struct tty_device *device) {
    struct seaside_tty_input *input = device->input;
    wl_list_remove(&device->link);
    wl_list_remove(&device->destroy.link);
    if (!device->keyboard) {
        wl_list_remove(&device->event.link);
        wl_list_remove(&device->absolute_motion.link);
        wl_list_remove(&device->button.link);
        wl_list_remove(&device->axis.link);
        wl_list_remove(&device->frame.link);
        if (input->pointer_grab_device == device) {
            input->pointer_grab_device = NULL;
            if (input->seat && input->seat->drag) {
                const uint32_t button = input->seat->pointer_state.grab_button;
                seaside_tty_input_cancel_drag(input);
                // Clear the held seat button after cancelling, so an unplug
                // cannot perform the drop or block hover on surviving devices.
                wlr_seat_pointer_notify_button(input->seat, monotonic_msec(), button,
                                               WL_POINTER_BUTTON_STATE_RELEASED);
            }
        }
        const size_t old_consumed = input->consumed_button_count;
        for (size_t button = 0; button < KEY_CNT; ++button) {
            if (device->consumed_buttons[button]) {
                --input->consumed_button_count;
            }
        }
        if (--input->snapshot.pointers == 0 && input->seat) {
            leave_constraint(input, true);
            wlr_seat_pointer_notify_clear_focus(input->seat);
            refresh_capabilities(input);
        }
        if (input->gesture_device == device) {
            input->gesture_device = NULL;
            input->gesture_released = false;
            if (input->callbacks.pointer_cancelled) {
                input->callbacks.pointer_cancelled(input->data);
            }
        }
        if (old_consumed > 0 && input->consumed_button_count == 0) {
            seaside_tty_input_refresh_pointer(input);
        }
    } else {
        if (device->direct_key) {
            wl_list_remove(&device->event.link);
        } else if (device->keyboard->group == input->group) {
            wlr_keyboard_group_remove_keyboard(input->group, device->keyboard);
        }
        if (--input->snapshot.keyboards == 0) {
            refresh_capabilities(input);
        }
    }
    free(device);
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct tty_device *device = wl_container_of(listener, device, destroy);
    remove_device(device);
}

static uint64_t update_pointer_target(struct seaside_tty_input *input, uint32_t time_msec) {
    struct wlr_seat *seat = input->seat;
    if (!input->attached || !seat || input->snapshot.pointers == 0 ||
        !input->callbacks.pointer_target || input->consumed_button_count > 0) {
        return 0;
    }
    // Wayland keeps implicit button grabs on the original pointer surface.
    // Popup and other explicit grabs instead choose which events to forward.
    const bool implicit_grab =
        seat->pointer_state.button_count > 0 && !wlr_seat_pointer_has_grab(seat);
    struct seaside_tty_pointer_target target = {0};
    if (input->constraint) {
        target.surface = input->constraint->surface;
        if (!input->callbacks.pointer_target(input->data, &target) ||
            !constraint_contains(input->constraint, target.sx, target.sy)) {
            leave_constraint(input, true);
            target = (struct seaside_tty_pointer_target){0};
        }
    }
    if (!input->constraint) {
        target.surface = implicit_grab ? seat->pointer_state.focused_surface : NULL;
        if ((!implicit_grab || target.surface) &&
            !input->callbacks.pointer_target(input->data, &target)) {
            target = (struct seaside_tty_pointer_target){0};
        }
    }
    if (target.surface) {
        wlr_seat_pointer_notify_enter(seat, target.surface, target.sx, target.sy);
        if (!pointer_locked(input)) {
            wlr_seat_pointer_notify_motion(seat, time_msec, target.sx, target.sy);
        }
    } else {
        wlr_seat_pointer_notify_clear_focus(seat);
    }
    refresh_constraint(input);
    return target.window_id;
}

void seaside_tty_input_refresh_pointer(struct seaside_tty_input *input) {
    if (input && input->attached && input->seat) {
        update_pointer_target(input, monotonic_msec());
        wlr_seat_pointer_notify_frame(input->seat);
    }
}

static void handle_pointer_grab_begin(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, pointer_grab_begin);
    refresh_constraint(input);
}

static void handle_pointer_grab_end(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, pointer_grab_end);
    if (!input->seat->drag) {
        seaside_tty_input_refresh_pointer(input);
    }
}

static void handle_keyboard_grab_end(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, keyboard_grab_end);
    refresh_focus(input);
}

static bool focused_selection_serial(struct wlr_seat *seat, uint32_t serial) {
    /* wlroots already checked the serial against the requesting client. The
     * focused client's serials also rule out writes using background input. */
    struct wlr_seat_client *focused = seat->keyboard_state.focused_client;
    return focused && wlr_seat_client_validate_event_serial(focused, serial);
}

static void handle_selection(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, request_selection);
    const struct wlr_seat_request_set_selection_event *event = data;
    struct wlr_seat *seat = input->seat;
    if (focused_selection_serial(seat, event->serial) &&
        (!event->source || event->source != seat->drag_source)) {
        wlr_seat_set_selection(seat, event->source, event->serial);
    }
}

static void handle_primary_selection(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, request_primary_selection);
    const struct wlr_seat_request_set_primary_selection_event *event = data;
    if (focused_selection_serial(input->seat, event->serial)) {
        wlr_seat_set_primary_selection(input->seat, event->source, event->serial);
    }
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, drag_destroy);
    wl_list_remove(&input->drag_destroy.link);
    wl_list_remove(&input->drag_origin_destroy.link);
    /* wlroots ends its pointer grab before sending the data-device leave. */
    seaside_tty_input_refresh_pointer(input);
}

static void handle_drag_origin_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_input *input = wl_container_of(listener, input, drag_origin_destroy);
    seaside_tty_input_cancel_drag(input);
}

static void handle_drag_request(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, request_drag);
    const struct wlr_seat_request_start_drag_event *event = data;
    struct wlr_drag *drag = event->drag;
    struct wlr_seat *seat = input->seat;
    const bool unused_source =
        drag->source && drag->source != seat->selection_source && drag->source != seat->drag_source;
    if (event->origin && input->pointer_grab_device && !input->constraint &&
        !wlr_seat_pointer_has_grab(seat) && !wlr_seat_keyboard_has_grab(seat) &&
        wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial) &&
        seat->pointer_state.buttons[0].n_pressed == 1 && (!drag->source || unused_source)) {
        wlr_seat_start_pointer_drag(seat, drag, event->serial);
        wl_signal_add(&drag->events.destroy, &input->drag_destroy);
        wl_signal_add(&event->origin->events.destroy, &input->drag_origin_destroy);
        if (drag->icon && input->callbacks.drag_icon) {
            input->callbacks.drag_icon(input->data, drag->icon);
        }
        seaside_tty_input_refresh_pointer(input);
    } else if (unused_source) {
        wlr_data_source_destroy(drag->source);
    } else {
        /* A source-less request has no source destroy signal to dispose of
         * its unstarted grab. Preserve sources already in use by the seat. */
        drag->pointer_grab.interface->cancel(&drag->pointer_grab);
    }
}

static void move_pointer(struct seaside_tty_input *input, uint32_t time_msec, bool absolute,
                         double x, double y) {
    if (!isfinite(x) || !isfinite(y) || pointer_locked(input)) {
        return;
    }
    if (input->constraint) {
        struct seaside_tty_pointer_target origin = {.surface = input->constraint->surface};
        if (!input->callbacks.pointer_target(input->data, &origin) ||
            !constraint_contains(input->constraint, origin.sx, origin.sy)) {
            leave_constraint(input, true);
        } else {
            double dx = x, dy = y;
            if (absolute &&
                (!input->callbacks.pointer_absolute_delta ||
                 !input->callbacks.pointer_absolute_delta(input->data, x, y, &dx, &dy))) {
                return;
            }
            if (!wlr_region_confine(constraint_region(input->constraint), origin.sx, origin.sy,
                                    origin.sx + dx, origin.sy + dy, &x, &y)) {
                return;
            }
            absolute = false;
            x -= origin.sx;
            y -= origin.sy;
        }
    }
    if (input->callbacks.motion) {
        input->callbacks.motion(input->data, absolute, x, y);
    }
    update_pointer_target(input, time_msec);
}

static void handle_relative_motion(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, event);
    const struct wlr_pointer_motion_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->seat && input->relative_pointer && input->consumed_button_count == 0) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            input->relative_pointer, input->seat, (uint64_t)event->time_msec * 1000, event->delta_x,
            event->delta_y, event->unaccel_dx, event->unaccel_dy);
    }
    move_pointer(input, event->time_msec, false, event->delta_x, event->delta_y);
}

static void handle_absolute_motion(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, absolute_motion);
    const struct wlr_pointer_motion_absolute_event *event = data;
    struct seaside_tty_input *input = device->input;
    move_pointer(input, event->time_msec, true, event->x, event->y);
}

static void handle_pointer_button(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, button);
    const struct wlr_pointer_button_event *event = data;
    struct seaside_tty_input *input = device->input;
    struct wlr_seat *seat = input->seat;
    if (!seat) {
        return;
    }
    const uint32_t button = event->button;
    const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    if (button < KEY_CNT && device->suppressed_drag_buttons[button]) {
        device->suppressed_drag_buttons[button] = pressed;
        return;
    }
    if (seat->drag && (pressed || device != input->pointer_grab_device)) {
        if (pressed && button < KEY_CNT) {
            device->suppressed_drag_buttons[button] = true;
        }
        return;
    }
    if (button < KEY_CNT && device->consumed_buttons[button]) {
        if (!pressed) {
            if (input->gesture_device == device && input->gesture_button == button) {
                /* wlroots emits unframed releases while destroying a pointer.
                 * A real release is followed by a frame from the backend. */
                input->gesture_released = true;
                return;
            }
            device->consumed_buttons[button] = false;
            --input->consumed_button_count;
            update_pointer_target(input, event->time_msec);
        }
        return;
    }
    if (input->consumed_button_count > 0) {
        if (pressed && button < KEY_CNT) {
            device->consumed_buttons[button] = true;
            ++input->consumed_button_count;
        }
        return;
    }
    if (!pressed && input->pointer_grab_device == device &&
        button == seat->pointer_state.grab_button) {
        if (seat->drag) {
            /* Unplugging emits synthetic releases without a pointer frame.
             * Complete a drop only on a framed release from its own device. */
            device->drag_release_pending = true;
            device->drag_release_button = button;
            device->drag_release_time = event->time_msec;
            return;
        }
        input->pointer_grab_device = NULL;
    }
    const uint64_t window = update_pointer_target(input, event->time_msec);
    if (input->callbacks.pointer_button && !wlr_seat_pointer_has_grab(seat) && !input->constraint &&
        (seat->pointer_state.button_count == 0 || !pressed) &&
        input->callbacks.pointer_button(input->data, window, button, pressed, input->modifiers) &&
        pressed && button < KEY_CNT) {
        input->gesture_device = device;
        input->gesture_button = button;
        device->consumed_buttons[button] = true;
        ++input->consumed_button_count;
        wlr_seat_pointer_notify_clear_focus(seat);
        return;
    }
    const bool first_press = pressed && seat->pointer_state.button_count == 0;
    if (wlr_seat_pointer_notify_button(seat, event->time_msec, button, event->state) &&
        first_press && !wlr_seat_pointer_has_grab(seat)) {
        input->pointer_grab_device = device;
    }
    if (!pressed && seat->pointer_state.button_count == 0) {
        update_pointer_target(input, event->time_msec);
    }
}

static void handle_pointer_axis(struct wl_listener *listener, void *data) {
    struct tty_device *device = wl_container_of(listener, device, axis);
    const struct wlr_pointer_axis_event *event = data;
    struct seaside_tty_input *input = device->input;
    if (input->seat && input->consumed_button_count == 0) {
        update_pointer_target(input, event->time_msec);
        wlr_seat_pointer_notify_axis(input->seat, event->time_msec, event->orientation,
                                     event->delta, event->delta_discrete, event->source,
                                     event->relative_direction);
    }
}

static void handle_pointer_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct tty_device *device = wl_container_of(listener, device, frame);
    struct seaside_tty_input *input = device->input;
    if (input->seat) {
        if (device->drag_release_pending) {
            device->drag_release_pending = false;
            input->pointer_grab_device = NULL;
            update_pointer_target(input, device->drag_release_time);
            wlr_seat_pointer_notify_button(input->seat, device->drag_release_time,
                                           device->drag_release_button,
                                           WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (input->gesture_device == device && input->gesture_released) {
            const uint32_t button = input->gesture_button;
            device->consumed_buttons[button] = false;
            --input->consumed_button_count;
            input->gesture_device = NULL;
            input->gesture_released = false;
            input->callbacks.pointer_button(input->data, 0, button, false, input->modifiers);
            update_pointer_target(input, monotonic_msec());
        }
        wlr_seat_pointer_notify_frame(input->seat);
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

static uint32_t shortcut_modifiers(const struct seaside_tty_input *input) {
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
    return state;
}

static uint8_t binding_modifiers(uint32_t state) {
    return ((state & WLR_MODIFIER_LOGO) ? 1 : 0) | ((state & WLR_MODIFIER_CTRL) ? 2 : 0) |
           ((state & WLR_MODIFIER_ALT) ? 4 : 0) | ((state & WLR_MODIFIER_SHIFT) ? 8 : 0);
}

static bool configured_shortcut(struct seaside_tty_input *input, uint32_t keycode) {
    if (!input->callbacks.shortcut_pressed) {
        return false;
    }
    const uint32_t state = shortcut_modifiers(input);
    if ((state & (WLR_MODIFIER_MOD3 | WLR_MODIFIER_MOD5)) != 0) {
        return false;
    }
    /* Ignore lock modifier bits while using the effective XKB keysym. */
    struct xkb_state *xkb = input->shortcut_state;
    const xkb_keycode_t code = keycode + 8;
    const uint8_t modifiers = binding_modifiers(state);
    const xkb_keysym_t effective = xkb_state_key_get_one_sym(xkb, code);
    if (input->callbacks.shortcut_pressed(input->data, modifiers, effective)) {
        return true;
    }

    /* Try the active layout's base symbol using the same modifiers. A binding
     * for the explicit shifted symbol still takes priority over Shift+digit. */
    const xkb_layout_index_t layout = xkb_state_key_get_layout(xkb, code);
    const xkb_keysym_t *level_zero = NULL;
    xkb_keysym_t base = XKB_KEY_NoSymbol;
    if (layout != XKB_LAYOUT_INVALID &&
        xkb_keymap_key_get_syms_by_level(xkb_state_get_keymap(xkb), code, layout, 0, &level_zero) ==
            1) {
        base = level_zero[0];
        if (base != effective && input->callbacks.shortcut_pressed(input->data, modifiers, base)) {
            return true;
        }
    }

    // The documented Super+D uses an uppercase name for the unshifted key.
    // Match XKB's case equivalent only after exact matches, with the same modifiers.
    const xkb_keysym_t upper = xkb_keysym_to_upper(effective);
    const xkb_keysym_t opposite = upper != effective ? upper : xkb_keysym_to_lower(effective);
    return opposite != effective && opposite != base &&
           input->callbacks.shortcut_pressed(input->data, modifiers, opposite);
}

static void update_binding_modifiers(struct seaside_tty_input *input) {
    if (!input->shortcut_state) {
        return;
    }
    const uint8_t modifiers = binding_modifiers(shortcut_modifiers(input));
    if (input->modifiers != modifiers) {
        input->modifiers = modifiers;
        if (input->attached && input->callbacks.modifiers_changed) {
            input->callbacks.modifiers_changed(input->data, modifiers);
        }
    }
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
        update_binding_modifiers(input);
    }
    if (pressed && !input->consumed[event->keycode]) {
        if (handled) {
            input->consumed[event->keycode] = true;
            return;
        }
        if (event->keycode == KEY_ESC && input->modifiers == 0 && input->seat &&
            input->seat->drag) {
            input->consumed[event->keycode] = true;
            seaside_tty_input_cancel_drag(input);
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
    update_binding_modifiers(input);
}

static void handle_group_leave(struct wl_listener *listener, void *data) {
    struct seaside_tty_input *input = wl_container_of(listener, input, group_leave);
    struct wl_array *keys = data;
    const uint32_t time_msec = monotonic_msec();
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
    update_binding_modifiers(input);
    deliver_modifiers(input, &input->group->keyboard.modifiers);
}

static void configure_pointer(struct libinput_device *device, double sensitivity) {
    if (device && libinput_device_config_accel_is_available(device) &&
        (libinput_device_config_accel_get_profiles(device) & LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT)) {
        libinput_device_config_accel_set_profile(device, LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT);
        libinput_device_config_accel_set_speed(device, sensitivity);
    }
}

void seaside_tty_input_set_sensitivity(struct seaside_tty_input *input, double sensitivity) {
    if (!input) {
        return;
    }
    input->sensitivity = sensitivity;
    struct tty_device *device;
    wl_list_for_each(device, &input->devices, link) {
        configure_pointer(device->pointer_device, sensitivity);
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
        if (wlr_input_device_is_libinput(base)) {
            device->pointer_device = wlr_libinput_get_device_handle(base);
            configure_pointer(device->pointer_device, input->sensitivity);
        }
        device->event.notify = handle_relative_motion;
        device->absolute_motion.notify = handle_absolute_motion;
        device->button.notify = handle_pointer_button;
        device->axis.notify = handle_pointer_axis;
        device->frame.notify = handle_pointer_frame;
        wl_signal_add(&pointer->events.motion, &device->event);
        wl_signal_add(&pointer->events.motion_absolute, &device->absolute_motion);
        wl_signal_add(&pointer->events.button, &device->button);
        wl_signal_add(&pointer->events.axis, &device->axis);
        wl_signal_add(&pointer->events.frame, &device->frame);
        if (++input->snapshot.pointers == 1) {
            refresh_capabilities(input);
            seaside_tty_input_refresh_pointer(input);
        }
    } else {
        if (!input->group) {
            device->keyboard = wlr_keyboard_from_input_device(base);
            device->direct_key = true;
            device->event.notify = handle_cursor_fixture_key;
            wl_signal_add(&device->keyboard->events.key, &device->event);
        }
        if (++input->snapshot.keyboards == 1) {
            refresh_capabilities(input);
        }
    }
}

static void detach(struct seaside_tty_input *input) {
    if (!input->attached) {
        return;
    }
    input->attached = false;
    seaside_tty_input_cancel_drag(input);
    leave_constraint(input, true);
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
    wl_list_init(&input->pointer_surface_commit.link);
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
    leave_constraint(input, false);
    wl_list_remove(&input->seat_destroy.link);
    wl_list_remove(&input->pointer_focus_change.link);
    clear_pointer_surface_watch(input);
    wl_list_remove(&input->pointer_grab_begin.link);
    wl_list_remove(&input->pointer_grab_end.link);
    wl_list_remove(&input->keyboard_grab_end.link);
    wl_list_remove(&input->request_selection.link);
    wl_list_remove(&input->request_primary_selection.link);
    wl_list_remove(&input->request_drag.link);
    wl_list_remove(&input->focus_destroy.link);
    wl_list_init(&input->focus_destroy.link);
    input->focus = NULL;
    struct wlr_seat *seat = input->seat;
    input->seat = NULL;
    cancel_drag(seat);
    /* A chosen drop can outlive its grab. Retire the source before its
     * destroy callback could access the freed seat. */
    wlr_data_source_destroy(seat->drag_source);
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
    const bool needs_shortcut_state = input->callbacks.shortcut_pressed ||
                                      input->callbacks.modifiers_changed ||
                                      input->callbacks.pointer_button;
    struct xkb_state *shortcut_state = needs_shortcut_state ? xkb_state_new(keymap) : NULL;
    bool mapped = group && (!needs_shortcut_state || shortcut_state) &&
                  wlr_keyboard_set_keymap(&group->keyboard, keymap) &&
                  wlr_keyboard_set_keymap(&input->client_keyboard, keymap);
    xkb_keymap_unref(keymap);
    struct wlr_seat *seat = mapped ? wlr_seat_create(display, "seat0") : NULL;
    struct wlr_relative_pointer_manager_v1 *relative_pointer = NULL;
    struct wlr_pointer_constraints_v1 *constraints = NULL;
    if (seat && input->callbacks.pointer_target) {
        relative_pointer = wlr_relative_pointer_manager_v1_create(display);
        constraints = wlr_pointer_constraints_v1_create(display);
    }
    if (!seat || (input->callbacks.pointer_target && (!relative_pointer || !constraints)) ||
        !wlr_data_device_manager_create(display) ||
        !wlr_primary_selection_v1_device_manager_create(display)) {
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
    input->pointer_focus_change.notify = handle_pointer_focus_change;
    input->pointer_surface_commit.notify = handle_pointer_surface_commit;
    input->pointer_grab_begin.notify = handle_pointer_grab_begin;
    input->pointer_grab_end.notify = handle_pointer_grab_end;
    input->keyboard_grab_end.notify = handle_keyboard_grab_end;
    input->request_selection.notify = handle_selection;
    input->request_primary_selection.notify = handle_primary_selection;
    input->request_drag.notify = handle_drag_request;
    input->drag_destroy.notify = handle_drag_destroy;
    input->drag_origin_destroy.notify = handle_drag_origin_destroy;
    input->focus_destroy.notify = handle_focus_destroy;
    input->group_key.notify = handle_group_key;
    input->group_modifiers.notify = handle_group_modifiers;
    input->group_enter.notify = handle_group_enter;
    input->group_leave.notify = handle_group_leave;
    wl_signal_add(&seat->events.destroy, &input->seat_destroy);
    wl_signal_add(&seat->pointer_state.events.focus_change, &input->pointer_focus_change);
    wl_signal_add(&seat->events.pointer_grab_begin, &input->pointer_grab_begin);
    wl_signal_add(&seat->events.pointer_grab_end, &input->pointer_grab_end);
    wl_signal_add(&seat->events.keyboard_grab_end, &input->keyboard_grab_end);
    wl_signal_add(&seat->events.request_set_selection, &input->request_selection);
    wl_signal_add(&seat->events.request_set_primary_selection, &input->request_primary_selection);
    wl_signal_add(&seat->events.request_start_drag, &input->request_drag);
    wl_signal_add(&group->keyboard.events.key, &input->group_key);
    wl_signal_add(&group->keyboard.events.modifiers, &input->group_modifiers);
    wl_signal_add(&group->events.enter, &input->group_enter);
    wl_signal_add(&group->events.leave, &input->group_leave);
    if (relative_pointer && constraints) {
        input->relative_pointer = relative_pointer;
        input->constraints = constraints;
        input->relative_pointer_destroy.notify = handle_relative_pointer_destroy;
        input->constraints_destroy.notify = handle_constraints_destroy;
        input->new_constraint.notify = handle_new_constraint;
        wl_signal_add(&relative_pointer->events.destroy, &input->relative_pointer_destroy);
        wl_signal_add(&constraints->events.destroy, &input->constraints_destroy);
        wl_signal_add(&constraints->events.new_constraint, &input->new_constraint);
    }
    refresh_capabilities(input);
    return true;
}

void seaside_tty_input_destroy(struct seaside_tty_input *input) {
    if (input) {
        detach(input);
        wlr_seat_destroy(input->seat);
        if (input->relative_pointer) {
            wl_list_remove(&input->relative_pointer_destroy.link);
        }
        if (input->constraints) {
            wl_list_remove(&input->constraints_destroy.link);
            wl_list_remove(&input->new_constraint.link);
        }
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

#include "tty_input.hpp"
#include "text_input.hpp"
#include "wl_array_span.hpp"
#include "wlr_input_method_compat.hpp"

#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <type_traits>

extern "C" {
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/region.h>
}
#include <xkbcommon/xkbcommon-keysyms.h>

namespace seaside {

struct TtyInputState {
    TtyInputObserver *observer;
    TtyInputOptions options;
    TtyInputSnapshot snapshot;
    struct wl_list devices;
    struct wl_listener new_input;
    struct wl_listener backend_destroy;
    bool attached;
    struct wlr_seat *seat;
    struct seaside_text_input *text_input;
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
    bool inverse_scroll;
    bool auto_center_on_lock;
    struct tty_device *gesture_device;
    uint32_t gesture_button;
    bool gesture_released;
    size_t consumed_button_count;
    bool dispatching_button_press;
    struct wlr_relative_pointer_manager_v1 *relative_pointer;
    struct wlr_pointer_constraints_v1 *constraints;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_protocol_logger *constraint_logger;
    struct {
        struct wl_client *client;
        uint32_t id;
    } empty_region_request;
    struct wl_listener relative_pointer_destroy;
    struct wl_listener constraints_destroy;
    struct wl_listener new_constraint;
    struct wl_listener constraint_destroy;
    struct wl_listener pointer_focus_change;
    struct wlr_cursor_shape_manager_v1 *cursor_shapes;
    struct wl_listener cursor_shapes_destroy;
    struct wl_listener request_cursor_shape;
    struct wl_listener request_cursor_surface;
    struct wl_listener cursor_surface_commit;
    struct wl_listener cursor_surface_destroy;
    struct wlr_surface *cursor_surface;
    int32_t cursor_hotspot_x, cursor_hotspot_y;
    struct wl_listener pointer_grab_begin;
    struct wl_listener pointer_grab_end;
    struct wl_listener keyboard_grab_end;
    struct wl_listener request_selection;
    struct wl_listener request_primary_selection;
    struct wl_listener request_drag;
    struct wl_listener drag_destroy;
    struct wl_listener drag_origin_destroy;
    struct wlr_surface *drag_origin;
    struct tty_device *pointer_grab_device;
};
static_assert(std::is_standard_layout_v<TtyInputState>);

enum class ConstraintRegion { unchanged, normal, empty };

struct constraint_region_state {
    TtyInputState *input;
    struct wlr_pointer_constraint_v1 *constraint;
    struct wlr_surface_synced synced;
    struct wl_listener commit, destroy;
    ConstraintRegion pending, current;
};
static_assert(std::is_standard_layout_v<constraint_region_state>);

struct tty_device {
    TtyInputState *input;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener event;
    struct wl_listener absolute_motion;
    struct wl_listener button;
    struct wl_listener axis;
    struct wl_listener frame;
    struct wlr_keyboard *keyboard;
    struct libinput_device *pointer_device;
    bool invert_axis;
    bool consumed_buttons[KEY_CNT];
    bool suppressed_drag_buttons[KEY_CNT];
    bool drag_release_pending;
    uint32_t drag_release_button, drag_release_time;
    bool direct_key;
};
static_assert(std::is_standard_layout_v<tty_device>);

static void refresh_pointer_focus(TtyInputState *input) noexcept;

static uint32_t monotonic_msec(void) {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

static bool pointer_locked(const TtyInputState *input) {
    return input->constraint && input->constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED;
}

static void leave_constraint(TtyInputState *input, bool notify_client) {
    if (!input->constraint) {
        return;
    }
    const bool locked = pointer_locked(input);
    struct wlr_pointer_constraint_v1 *constraint = input->constraint;
    input->constraint = nullptr;
    wl_list_remove(&input->constraint_destroy.link);
    if (notify_client) {
        // Deactivating a one-shot constraint destroys it synchronously.
        wlr_pointer_constraint_v1_send_deactivated(constraint);
    }
    if (locked) {
        input->observer->pointer_locked(false);
    }
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, constraint_destroy);
    leave_constraint(input, false);
}

static const pixman_region32_t *constraint_region(struct wlr_pointer_constraint_v1 *constraint) {
    auto *state = static_cast<const constraint_region_state *>(constraint->data);
    if (!state || state->current == ConstraintRegion::empty) {
        return nullptr;
    }
    // wlroots intersects an explicit region immediately, but populates its
    // cached default region on the next surface commit.
    return pixman_region32_not_empty(&constraint->current.region)
               ? &constraint->region
               : &constraint->surface->input_region;
}

static bool constraint_contains(struct wlr_pointer_constraint_v1 *constraint, double sx,
                                double sy) {
    const auto *region = constraint_region(constraint);
    return region && sx >= 0 && sy >= 0 && sx < INT32_MAX && sy < INT32_MAX &&
           pixman_region32_contains_point(region, (int32_t)sx, (int32_t)sy, nullptr);
}

static struct wlr_pointer_constraint_v1 *center_pending_lock(TtyInputState *input,
                                                             struct wlr_surface *focus,
                                                             const TtyPointerTarget &hover,
                                                             uint64_t &window) {
    if (!input->auto_center_on_lock || input->seat->pointer_state.button_count) {
        return nullptr;
    }
    auto *lock =
        wlr_pointer_constraints_v1_constraint_for_surface(input->constraints, focus, input->seat);
    seaside_pointer_content content;
    if (!lock || lock->type != WLR_POINTER_CONSTRAINT_V1_LOCKED ||
        !input->observer->pointer_content(focus, content) || !content.toplevel ||
        (!content.fullscreen && hover.surface != focus)) {
        return nullptr;
    }

    const double x = content.bounds.x + content.bounds.width / 2.0;
    const double y = content.bounds.y + content.bounds.height / 2.0;
    TtyPointerTarget center{};
    if (!input->observer->pointer_target_at(x, y, center) || center.surface != focus ||
        !constraint_contains(lock, center.sx, center.sy) || !input->observer->place_pointer(x, y)) {
        return nullptr;
    }
    wlr_seat_pointer_notify_enter(input->seat, focus, center.sx, center.sy);
    wlr_seat_pointer_notify_motion(input->seat, monotonic_msec(), center.sx, center.sy);
    wlr_seat_pointer_notify_frame(input->seat);
    window = center.window_id;
    return lock;
}

static uint64_t refresh_constraint(TtyInputState *input, uint64_t window = 0) {
    struct wlr_seat *seat = input->seat;
    if (!input->constraints || !seat || wl_list_empty(&input->constraints->constraints)) {
        return window;
    }
    struct wlr_surface *focus = seat->keyboard_state.focused_surface;
    if (!input->attached || !input->snapshot.pointers || !focus ||
        wlr_seat_pointer_has_grab(seat) || wlr_seat_keyboard_has_grab(seat) ||
        input->consumed_button_count || input->dispatching_button_press) {
        leave_constraint(input, true);
        return window;
    }

    TtyPointerTarget hover{};
    input->observer->pointer_target(hover);
    struct wlr_pointer_constraint_v1 *candidate = nullptr;
    // The scene hit is current even while the seat suppresses locked motion.
    // Hover scrolling alone cannot activate a background surface's request.
    if (hover.surface && hover.surface == seat->pointer_state.focused_surface &&
        wlr_surface_get_root_surface(hover.surface) == wlr_surface_get_root_surface(focus)) {
        candidate = wlr_pointer_constraints_v1_constraint_for_surface(input->constraints,
                                                                      hover.surface, seat);
        if (candidate && !constraint_contains(candidate, hover.sx, hover.sy)) {
            candidate = nullptr;
        }
    }
    if (candidate && candidate == input->constraint) {
        return window;
    }
    leave_constraint(input, true);

    if ((!candidate || candidate->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) &&
        (!candidate || candidate->surface == focus)) {
        if (auto *centered = center_pending_lock(input, focus, hover, window)) {
            candidate = centered;
        }
    }
    if (candidate) {
        input->constraint = candidate;
        input->constraint_destroy.notify = handle_constraint_destroy;
        wl_signal_add(&candidate->events.destroy, &input->constraint_destroy);
        wlr_pointer_constraint_v1_send_activated(candidate);
        if (pointer_locked(input)) {
            input->observer->pointer_locked(true);
        }
    }
    return window;
}

static void handle_constraint_commit(struct wl_listener *listener, void *) noexcept {
    struct constraint_region_state *state = wl_container_of(listener, state, commit);
    // A surface's synced states are already committed here. Deactivating a
    // one-shot constraint during the synced-state iteration would unlink it.
    refresh_pointer_focus(state->input);
}

static void destroy_constraint_region_state(struct wl_listener *listener, void *) noexcept {
    struct constraint_region_state *state = wl_container_of(listener, state, destroy);
    state->constraint->data = nullptr;
    wl_list_remove(&state->commit.link);
    wl_list_remove(&state->destroy.link);
    wlr_surface_synced_finish(&state->synced);
    delete state;
}

static const struct wlr_surface_synced_impl constraint_region_sync{
    .state_size = sizeof(ConstraintRegion),
    .init_state = nullptr,
    .finish_state = nullptr,
    .move_state =
        [](void *destination, void *source) {
            auto &applied = *static_cast<ConstraintRegion *>(destination);
            auto &requested = *static_cast<ConstraintRegion *>(source);
            if (requested != ConstraintRegion::unchanged) {
                applied = requested;
                requested = ConstraintRegion::unchanged;
            }
        },
    .commit = nullptr,
};

static bool is_explicitly_empty(const union wl_argument &argument) {
    auto *region = reinterpret_cast<struct wl_resource *>(argument.o);
    return region && !pixman_region32_not_empty(wlr_region_from_resource(region));
}

static void track_constraint_region(void *data, enum wl_protocol_logger_type direction,
                                    const struct wl_protocol_logger_message *message) noexcept {
    if (direction != WL_PROTOCOL_LOGGER_REQUEST) {
        return;
    }
    auto *input = static_cast<TtyInputState *>(data);
    input->empty_region_request = {};
    // wlroots 0.20 represents both null and explicitly empty constraint
    // regions with an empty region. Record that distinction before dispatch.
    struct wl_resource *resource = message->resource;
    const char *protocol = wl_resource_get_class(resource);
    if (message->arguments_count == 5 &&
        wl_resource_get_user_data(resource) == input->constraints &&
        strcmp(protocol, "zwp_pointer_constraints_v1") == 0 &&
        is_explicitly_empty(message->arguments[3])) {
        input->empty_region_request = {wl_resource_get_client(resource), message->arguments[0].n};
    } else if (message->arguments_count == 1 && strcmp(message->message->name, "set_region") == 0 &&
               (strcmp(protocol, "zwp_confined_pointer_v1") == 0 ||
                strcmp(protocol, "zwp_locked_pointer_v1") == 0)) {
        auto *constraint =
            static_cast<wlr_pointer_constraint_v1 *>(wl_resource_get_user_data(resource));
        if (constraint && constraint->pointer_constraints == input->constraints &&
            constraint->data) {
            auto *state = static_cast<constraint_region_state *>(constraint->data);
            state->pending = is_explicitly_empty(message->arguments[0]) ? ConstraintRegion::empty
                                                                        : ConstraintRegion::normal;
        }
    }
}

static void forget_cursor_surface(TtyInputState *input) {
    if (input->cursor_surface) {
        wl_list_remove(&input->cursor_surface_commit.link);
        wl_list_remove(&input->cursor_surface_destroy.link);
        input->cursor_surface = nullptr;
    }
}

static void leave_cursor_surface(TtyInputState *input) {
    if (input->cursor_surface) {
        struct wlr_surface_output *entry, *next;
        wl_list_for_each_safe(entry, next, &input->cursor_surface->current_outputs, link) {
            wlr_surface_send_leave(input->cursor_surface, entry->output);
        }
        forget_cursor_surface(input);
    }
}

static void handle_cursor_surface_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, cursor_surface_destroy);
    forget_cursor_surface(input);
    input->observer->cursor_shape(nullptr);
}

static int32_t offset_hotspot(int32_t hotspot, int32_t offset) {
    const int64_t value = (int64_t)hotspot - offset;
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return value > INT32_MAX ? INT32_MAX : (int32_t)value;
}

static void handle_cursor_surface_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, cursor_surface_commit);
    struct wlr_surface *surface = input->cursor_surface;
    input->cursor_hotspot_x = offset_hotspot(input->cursor_hotspot_x, surface->current.dx);
    input->cursor_hotspot_y = offset_hotspot(input->cursor_hotspot_y, surface->current.dy);
    input->observer->cursor_surface(surface, input->cursor_hotspot_x, input->cursor_hotspot_y);
}

static bool cursor_request_allowed(const TtyInputState *input, struct wlr_seat_client *client,
                                   uint32_t serial) {
    return input->attached && input->seat && client == input->seat->pointer_state.focused_client &&
           wlr_seat_client_validate_event_serial(client, serial);
}

static void handle_cursor_surface_request(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, request_cursor_surface);
    auto *event = static_cast<const wlr_seat_pointer_request_set_cursor_event *>(data);
    if (!cursor_request_allowed(input, event->seat_client, event->serial)) {
        return;
    }
    if (input->cursor_surface != event->surface) {
        leave_cursor_surface(input);
        input->cursor_surface = event->surface;
        if (event->surface) {
            wl_signal_add(&event->surface->events.commit, &input->cursor_surface_commit);
            wl_signal_add(&event->surface->events.destroy, &input->cursor_surface_destroy);
        }
    }
    input->cursor_hotspot_x = event->hotspot_x;
    input->cursor_hotspot_y = event->hotspot_y;
    input->observer->cursor_surface(event->surface, event->hotspot_x, event->hotspot_y);
}

static void handle_cursor_shape_request(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, request_cursor_shape);
    auto *event = static_cast<const wlr_cursor_shape_manager_v1_request_set_shape_event *>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
        !cursor_request_allowed(input, event->seat_client, event->serial)) {
        return;
    }
    leave_cursor_surface(input);
    input->observer->cursor_shape(wlr_cursor_shape_v1_name(event->shape));
}

static void forget_cursor_shapes(TtyInputState *input) {
    wl_list_remove(&input->cursor_shapes_destroy.link);
    wl_list_remove(&input->request_cursor_shape.link);
    input->cursor_shapes = nullptr;
}

static void handle_cursor_shapes_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, cursor_shapes_destroy);
    forget_cursor_shapes(input);
}

void TtyInput::cursor_presented(struct wlr_output *output, const struct timespec *when) noexcept {
    auto *input = state_.get();
    struct wlr_surface *surface = input->cursor_surface;
    if (!surface || !output || !when) {
        return;
    }
    struct wlr_surface_output *entry, *next;
    wl_list_for_each_safe(entry, next, &surface->current_outputs, link) {
        if (entry->output != output) {
            wlr_surface_send_leave(surface, entry->output);
        }
    }
    wlr_surface_send_enter(surface, output);
    wlr_fractional_scale_v1_notify_scale(surface, output->scale);
    wlr_surface_set_preferred_buffer_scale(surface, (int32_t)ceilf(output->scale));
    wlr_surface_send_frame_done(surface, when);
}

static void handle_pointer_focus_change(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, pointer_focus_change);
    auto *event = static_cast<const wlr_seat_pointer_focus_change_event *>(data);
    if (input->options.client_cursor) {
        leave_cursor_surface(input);
        input->observer->cursor_shape(nullptr);
    }
    if (input->constraint && input->constraint->surface != event->new_surface) {
        leave_constraint(input, true);
    }
}

static void handle_new_constraint(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, new_constraint);
    auto *constraint = static_cast<wlr_pointer_constraint_v1 *>(data);
    if (constraint->seat != input->seat) {
        return;
    }
    auto state =
        std::unique_ptr<constraint_region_state>(new (std::nothrow) constraint_region_state{});
    if (!state ||
        !wlr_surface_synced_init(&state->synced, constraint->surface, &constraint_region_sync,
                                 &state->pending, &state->current)) {
        wl_resource_post_no_memory(constraint->resource);
        return;
    }
    const auto request = input->empty_region_request;
    if (request.client == wl_resource_get_client(constraint->resource) &&
        request.id == wl_resource_get_id(constraint->resource)) {
        state->current = ConstraintRegion::empty;
    }
    state->input = input;
    state->constraint = constraint;
    state->commit.notify = handle_constraint_commit;
    state->destroy.notify = destroy_constraint_region_state;
    wl_signal_add(&constraint->surface->events.commit, &state->commit);
    wl_signal_add(&constraint->events.destroy, &state->destroy);
    constraint->data = state.release();
    struct wlr_surface *focus = input->seat->keyboard_state.focused_surface;
    if (focus &&
        wlr_surface_get_root_surface(focus) == wlr_surface_get_root_surface(constraint->surface)) {
        refresh_pointer_focus(input);
    }
}

static void forget_constraint_protocol(TtyInputState *input) {
    wl_protocol_logger_destroy(input->constraint_logger);
    input->constraint_logger = nullptr;
    wl_list_remove(&input->constraints_destroy.link);
    wl_list_remove(&input->new_constraint.link);
    input->constraints = nullptr;
}

static void handle_constraints_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, constraints_destroy);
    leave_constraint(input, false);
    forget_constraint_protocol(input);
}

static void handle_relative_pointer_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, relative_pointer_destroy);
    wl_list_remove(&input->relative_pointer_destroy.link);
    input->relative_pointer = nullptr;
}

static void refresh_focus(TtyInputState *input) {
    if (!input->seat) {
        return;
    }
    if (!input->focus || !wlr_seat_get_keyboard(input->seat)) {
        wlr_seat_keyboard_notify_clear_focus(input->seat);
        leave_constraint(input, true);
        return;
    }
    struct wlr_keyboard *keyboard = &input->client_keyboard;
    wlr_seat_keyboard_notify_enter(input->seat, input->focus, keyboard->keycodes,
                                   keyboard->num_keycodes, &keyboard->modifiers);
    if (input->constraints) {
        refresh_pointer_focus(input);
    }
}

static void set_keyboard_focus(TtyInputState *input, wlr_surface *surface) noexcept {
    if (!input->seat || input->focus == surface) {
        return;
    }
    seaside_text_input_restore_keyboard(input->text_input);
    wl_list_remove(&input->focus_destroy.link);
    wl_list_init(&input->focus_destroy.link);
    input->focus = surface;
    if (surface) {
        wl_signal_add(&surface->events.destroy, &input->focus_destroy);
    }
    refresh_focus(input);
}

void TtyInput::set_focus(wlr_surface *surface) noexcept {
    set_keyboard_focus(state_.get(), surface);
}

static void handle_focus_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, focus_destroy);
    set_keyboard_focus(input, nullptr);
}

wlr_seat *TtyInput::seat() const noexcept {
    return state_->seat;
}

static void refresh_capabilities(TtyInputState *input) {
    if (!input->seat) {
        return;
    }
    uint32_t capabilities = input->snapshot.pointers > 0 ? WL_SEAT_CAPABILITY_POINTER : 0;
    // Once discovered, keep the logical keyboard on the seat. Libinput
    // removes and re-adds physical devices on VT switches, and the first key
    // may arrive before a client can bind a replacement wl_keyboard.
    if (input->snapshot.keyboards > 0 && !wlr_seat_get_keyboard(input->seat)) {
        wlr_seat_set_keyboard(input->seat, &input->client_keyboard);
    }
    if (wlr_seat_get_keyboard(input->seat)) {
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(input->seat, capabilities);
    refresh_focus(input);
}

static void cancel_seat_drag(struct wlr_seat *seat) {
    struct wlr_drag *drag = seat ? seat->drag : nullptr;
    if (!drag) {
        return;
    }
    if (drag->source && !drag->dropped) {
        wlr_data_source_destroy(drag->source);
    } else {
        wlr_seat_pointer_end_grab(seat);
    }
}

void TtyInput::cancel_drag() noexcept {
    cancel_seat_drag(state_->seat);
}

void TtyInput::cancel_drag_from(wlr_surface *root) noexcept {
    auto *input = state_.get();
    if (input->drag_origin && wlr_surface_get_root_surface(input->drag_origin) == root) {
        cancel_seat_drag(input->seat);
    }
}

static void remove_device(struct tty_device *device) {
    TtyInputState *input = device->input;
    wl_list_remove(&device->link);
    wl_list_remove(&device->destroy.link);
    if (!device->keyboard) {
        wl_list_remove(&device->event.link);
        wl_list_remove(&device->absolute_motion.link);
        wl_list_remove(&device->button.link);
        wl_list_remove(&device->axis.link);
        wl_list_remove(&device->frame.link);
        if (input->pointer_grab_device == device) {
            input->pointer_grab_device = nullptr;
            if (input->seat && input->seat->drag) {
                const uint32_t button = input->seat->pointer_state.grab_button;
                cancel_seat_drag(input->seat);
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
            input->gesture_device = nullptr;
            input->gesture_released = false;
            input->observer->pointer_cancelled();
        }
        if (old_consumed > 0 && input->consumed_button_count == 0) {
            refresh_pointer_focus(input);
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
    delete device;
}

static void handle_device_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct tty_device *device = wl_container_of(listener, device, destroy);
    remove_device(device);
}

static uint64_t update_pointer_target(TtyInputState *input, uint32_t time_msec) {
    struct wlr_seat *seat = input->seat;
    if (!input->attached || !seat || input->snapshot.pointers == 0 ||
        !input->options.pointer_targets || input->consumed_button_count > 0) {
        return 0;
    }
    // Wayland keeps implicit button grabs on the original pointer surface.
    // Popup and other explicit grabs instead choose which events to forward.
    const bool implicit_grab =
        seat->pointer_state.button_count > 0 && !wlr_seat_pointer_has_grab(seat);
    TtyPointerTarget target = {};
    if (input->constraint) {
        if (!input->observer->pointer_target(target) ||
            target.surface != input->constraint->surface ||
            !constraint_contains(input->constraint, target.sx, target.sy)) {
            leave_constraint(input, true);
            target = TtyPointerTarget{};
        }
    }
    if (!input->constraint) {
        target.surface = implicit_grab ? seat->pointer_state.focused_surface : nullptr;
        if ((!implicit_grab || target.surface) && !input->observer->pointer_target(target)) {
            target = TtyPointerTarget{};
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
    return refresh_constraint(input, target.window_id);
}

static void refresh_pointer_focus(TtyInputState *input) noexcept {
    if (input->attached && input->seat) {
        update_pointer_target(input, monotonic_msec());
        wlr_seat_pointer_notify_frame(input->seat);
    }
}

void TtyInput::refresh_pointer() noexcept {
    refresh_pointer_focus(state_.get());
}

static void handle_pointer_grab_begin(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, pointer_grab_begin);
    refresh_constraint(input);
}

static void handle_pointer_grab_end(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, pointer_grab_end);
    if (!input->seat->drag) {
        refresh_pointer_focus(input);
    }
}

static void handle_keyboard_grab_end(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, keyboard_grab_end);
    refresh_focus(input);
}

static void handle_selection(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, request_selection);
    auto *event = static_cast<const wlr_seat_request_set_selection_event *>(data);
    struct wlr_seat *seat = input->seat;
    /* wlroots has validated the serial against the requesting seat client.
     * Satellite may finish negotiating X11 MIME types after focus moves. */
    if (!event->source || event->source != seat->drag_source) {
        wlr_seat_set_selection(seat, event->source, event->serial);
    }
}

static void handle_primary_selection(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, request_primary_selection);
    auto *event = static_cast<const wlr_seat_request_set_primary_selection_event *>(data);
    wlr_seat_set_primary_selection(input->seat, event->source, event->serial);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, drag_destroy);
    wl_list_remove(&input->drag_destroy.link);
    wl_list_remove(&input->drag_origin_destroy.link);
    input->drag_origin = nullptr;
    /* wlroots ends its pointer grab before sending the data-device leave. */
    refresh_pointer_focus(input);
}

static void handle_drag_origin_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, drag_origin_destroy);
    cancel_seat_drag(input->seat);
}

static void handle_drag_request(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, request_drag);
    auto *event = static_cast<const wlr_seat_request_start_drag_event *>(data);
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
        input->drag_origin = event->origin;
        if (drag->icon) {
            input->observer->drag_icon(drag->icon);
        }
        refresh_pointer_focus(input);
    } else if (unused_source) {
        wlr_data_source_destroy(drag->source);
    } else {
        /* A source-less request has no source destroy signal to dispose of
         * its unstarted grab. Preserve sources already in use by the seat. */
        drag->pointer_grab.interface->cancel(&drag->pointer_grab);
    }
}

static bool confine_delta(TtyInputState *input, const TtyPointerTarget &origin, double &dx,
                          double &dy) {
    seaside_pointer_content content;
    if (!input->observer->pointer_content(origin.surface, content)) {
        return false;
    }
    const bool clipped = content.bounds.width > 0 && content.bounds.height > 0;
    const seaside_cursor_point start{origin.lx, origin.ly};
    seaside_cursor_point target{start.x + dx, start.y + dy};
    if (clipped) {
        if (!seaside_cursor_contains(content.bounds, start)) {
            return false;
        }
        target = seaside_cursor_clamp(content.bounds, target);
    }
    const double scale_x = content.layout_per_surface_x, scale_y = content.layout_per_surface_y;
    double sx, sy;
    if (!wlr_region_confine(constraint_region(input->constraint), origin.sx, origin.sy,
                            origin.sx + (target.x - start.x) / scale_x,
                            origin.sy + (target.y - start.y) / scale_y, &sx, &sy)) {
        return false;
    }
    target = {start.x + (sx - origin.sx) * scale_x, start.y + (sy - origin.sy) * scale_y};
    if (clipped) {
        // The visible crop can end partway through a logical surface pixel.
        target = seaside_cursor_clamp(content.bounds, target);
        TtyPointerTarget hit{};
        if (!input->observer->pointer_target_at(target.x, target.y, hit) ||
            hit.surface != origin.surface ||
            !constraint_contains(input->constraint, hit.sx, hit.sy)) {
            return false;
        }
    }
    dx = target.x - start.x;
    dy = target.y - start.y;
    return true;
}

static void move_pointer(TtyInputState *input, uint32_t time_msec, bool absolute, double x,
                         double y) {
    if (!isfinite(x) || !isfinite(y) || pointer_locked(input)) {
        return;
    }
    enum seaside_pointer_motion kind =
        absolute ? SEASIDE_POINTER_ABSOLUTE : SEASIDE_POINTER_RELATIVE;
    if (input->constraint) {
        TtyPointerTarget origin{};
        if (!input->observer->pointer_target(origin) ||
            origin.surface != input->constraint->surface ||
            !constraint_contains(input->constraint, origin.sx, origin.sy)) {
            leave_constraint(input, true);
        } else {
            double dx = x, dy = y;
            if (absolute && !input->observer->pointer_absolute_delta(x, y, dx, dy)) {
                return;
            }
            if (!confine_delta(input, origin, dx, dy)) {
                return;
            }
            kind = SEASIDE_POINTER_CONFINED;
            x = dx;
            y = dy;
        }
    }
    input->observer->motion(kind, x, y);
    update_pointer_target(input, time_msec);
}

static void handle_relative_motion(struct wl_listener *listener, void *data) noexcept {
    struct tty_device *device = wl_container_of(listener, device, event);
    auto *event = static_cast<const wlr_pointer_motion_event *>(data);
    TtyInputState *input = device->input;
    if (input->seat && input->relative_pointer && input->consumed_button_count == 0) {
        wlr_relative_pointer_manager_v1_send_relative_motion(
            input->relative_pointer, input->seat, (uint64_t)event->time_msec * 1000, event->delta_x,
            event->delta_y, event->unaccel_dx, event->unaccel_dy);
    }
    move_pointer(input, event->time_msec, false, event->delta_x, event->delta_y);
    if (isfinite(event->delta_x) && isfinite(event->delta_y)) {
        input->observer->input_activity(true);
    }
}

static void handle_absolute_motion(struct wl_listener *listener, void *data) noexcept {
    struct tty_device *device = wl_container_of(listener, device, absolute_motion);
    auto *event = static_cast<const wlr_pointer_motion_absolute_event *>(data);
    TtyInputState *input = device->input;
    move_pointer(input, event->time_msec, true, event->x, event->y);
    if (isfinite(event->x) && isfinite(event->y)) {
        input->observer->input_activity(true);
    }
}

uint32_t TtyInput::begin_client_gesture(struct wlr_seat_client *client, struct wlr_surface *root,
                                        uint32_t serial) noexcept {
    auto *input = state_.get();
    struct wlr_seat *seat = input->seat;
    if (!seat || !input->attached || !client || client->seat != seat || !root ||
        !input->pointer_grab_device || !input->options.pointer_targets || input->constraint ||
        input->consumed_button_count > 0 || wlr_seat_pointer_has_grab(seat)) {
        return 0;
    }
    struct wlr_seat_pointer_state *pointer = &seat->pointer_state;
    const uint32_t button = pointer->grab_button;
    if (pointer->focused_client != client || !pointer->focused_surface ||
        wlr_surface_get_root_surface(pointer->focused_surface) != root ||
        pointer->button_count != 1 || pointer->buttons[0].n_pressed != 1 || button == 0 ||
        button >= KEY_CNT ||
        !wlr_seat_validate_pointer_grab_serial(seat, pointer->focused_surface, serial)) {
        return 0;
    }
    input->gesture_device = input->pointer_grab_device;
    input->gesture_button = button;
    input->gesture_device->consumed_buttons[button] = true;
    ++input->consumed_button_count;
    input->pointer_grab_device = nullptr;
    /* The compositor now owns the physical release. Drop client focus
     * before clearing wlroots' implicit grab so no release is sent to a client. */
    wlr_seat_pointer_notify_clear_focus(seat);
    wlr_seat_pointer_notify_button(seat, monotonic_msec(), button,
                                   WL_POINTER_BUTTON_STATE_RELEASED);
    return button;
}

static void handle_pointer_button(struct wl_listener *listener, void *data) noexcept {
    struct tty_device *device = wl_container_of(listener, device, button);
    auto *event = static_cast<const wlr_pointer_button_event *>(data);
    TtyInputState *input = device->input;
    struct wlr_seat *seat = input->seat;
    if (!seat) {
        return;
    }
    const uint32_t button = event->button;
    const bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
    /* Device removal can synthesize a release without local input. */
    if (pressed) {
        input->observer->input_activity(true);
    }
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
        input->pointer_grab_device = nullptr;
    }
    const uint64_t window = update_pointer_target(input, event->time_msec);
    struct wlr_surface *hovered = seat->pointer_state.focused_surface;
    struct wlr_surface *root = hovered ? wlr_surface_get_root_surface(hovered) : nullptr;
    const bool input_popup = root && wlr_input_popup_surface_v2_try_from_wlr_surface(root);
    if (input->options.pointer_targets && !wlr_seat_pointer_has_grab(seat) && !input->constraint &&
        !input_popup && (seat->pointer_state.button_count == 0 || !pressed)) {
        // The desktop may change focus before reporting that the press started a drag.
        input->dispatching_button_press = pressed;
        const bool consumed =
            input->observer->pointer_button(window, button, pressed, input->modifiers);
        input->dispatching_button_press = false;
        if (pressed) {
            if (consumed && button < KEY_CNT) {
                input->gesture_device = device;
                input->gesture_button = button;
                device->consumed_buttons[button] = true;
                ++input->consumed_button_count;
                wlr_seat_pointer_notify_clear_focus(seat);
                return;
            }
            refresh_constraint(input);
        }
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

static void handle_pointer_axis(struct wl_listener *listener, void *data) noexcept {
    struct tty_device *device = wl_container_of(listener, device, axis);
    auto *event = static_cast<const wlr_pointer_axis_event *>(data);
    TtyInputState *input = device->input;
    input->observer->input_activity(true);
    if (input->seat && input->consumed_button_count == 0) {
        update_pointer_target(input, event->time_msec);
        double delta = event->delta;
        int32_t discrete = event->delta_discrete;
        enum wl_pointer_axis_relative_direction direction = event->relative_direction;
        if (device->invert_axis) {
            delta = -delta;
            discrete = discrete == INT32_MIN ? INT32_MAX : -discrete;
            direction = direction == WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
                            ? WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL
                            : WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED;
        }
        wlr_seat_pointer_notify_axis(input->seat, event->time_msec, event->orientation, delta,
                                     discrete, event->source, direction);
    }
}

static void handle_pointer_frame(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct tty_device *device = wl_container_of(listener, device, frame);
    TtyInputState *input = device->input;
    if (input->seat) {
        if (device->drag_release_pending) {
            device->drag_release_pending = false;
            input->pointer_grab_device = nullptr;
            update_pointer_target(input, device->drag_release_time);
            wlr_seat_pointer_notify_button(input->seat, device->drag_release_time,
                                           device->drag_release_button,
                                           WL_POINTER_BUTTON_STATE_RELEASED);
        }
        if (input->gesture_device == device && input->gesture_released) {
            const uint32_t button = input->gesture_button;
            device->consumed_buttons[button] = false;
            --input->consumed_button_count;
            input->gesture_device = nullptr;
            input->gesture_released = false;
            input->observer->pointer_button(0, button, false, input->modifiers);
            update_pointer_target(input, monotonic_msec());
        }
        wlr_seat_pointer_notify_frame(input->seat);
    }
}

static void handle_cursor_fixture_key(struct wl_listener *listener, void *data) noexcept {
    struct tty_device *device = wl_container_of(listener, device, event);
    auto *event = static_cast<const wlr_keyboard_key_event *>(data);
    TtyInputState *input = device->input;
    if (event->keycode == KEY_ESC && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        input->observer->exit_requested();
    }
}

static bool launch_shortcut(TtyInputState *input, uint32_t keycode) {
    struct wlr_keyboard *keyboard = &input->group->keyboard;
    return input->options.fixture_launch &&
           wlr_keyboard_get_modifiers(keyboard) == WLR_MODIFIER_LOGO &&
           xkb_state_key_get_one_sym(keyboard->xkb_state, keycode + 8) == XKB_KEY_Return;
}

static uint32_t shortcut_modifiers(const TtyInputState *input) {
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

static bool configured_shortcut(TtyInputState *input, uint32_t keycode) {
    if (!input->shortcut_state) {
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
    if (input->observer->shortcut_pressed(modifiers, effective)) {
        return true;
    }

    /* Try the active layout's base symbol using the same modifiers. A binding
     * for the explicit shifted symbol still takes priority over Shift+digit. */
    const xkb_layout_index_t layout = xkb_state_key_get_layout(xkb, code);
    const xkb_keysym_t *level_zero = nullptr;
    xkb_keysym_t base = XKB_KEY_NoSymbol;
    if (layout != XKB_LAYOUT_INVALID &&
        xkb_keymap_key_get_syms_by_level(xkb_state_get_keymap(xkb), code, layout, 0, &level_zero) ==
            1) {
        base = level_zero[0];
        if (base != effective && input->observer->shortcut_pressed(modifiers, base)) {
            return true;
        }
    }

    // The documented Super+D uses an uppercase name for the unshifted key.
    // Match XKB's case equivalent only after exact matches, with the same modifiers.
    const xkb_keysym_t upper = xkb_keysym_to_upper(effective);
    const xkb_keysym_t opposite = upper != effective ? upper : xkb_keysym_to_lower(effective);
    return opposite != effective && opposite != base &&
           input->observer->shortcut_pressed(modifiers, opposite);
}

static void update_binding_modifiers(TtyInputState *input) {
    if (!input->shortcut_state) {
        return;
    }
    const uint8_t modifiers = binding_modifiers(shortcut_modifiers(input));
    if (input->modifiers != modifiers) {
        input->modifiers = modifiers;
        if (input->attached) {
            input->observer->modifiers_changed(modifiers);
        }
    }
}

static void deliver_key(TtyInputState *input, struct wlr_keyboard_key_event event) {
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
    if ((pressed && already_down) || seaside_text_input_key(input->text_input, keyboard, &event) ||
        pressed == already_down || (pressed && keyboard->num_keycodes == WLR_KEYBOARD_KEYS_CAP)) {
        return;
    }
    seaside_text_input_restore_keyboard(input->text_input);
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

static void handle_group_key(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, group_key);
    auto *event = static_cast<const wlr_keyboard_key_event *>(data);
    if (event->keycode >= KEY_CNT) {
        return;
    }
    const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    // A release without an XKB update can come from keyboard teardown.
    if ((pressed || event->update_state)) {
        input->observer->input_activity(false);
    }
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
            cancel_seat_drag(input->seat);
            return;
        }
        if (launch_shortcut(input, event->keycode)) {
            input->consumed[event->keycode] = true;
            ++input->snapshot.launch_requests;
            input->observer->launch_requested();
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

static void deliver_modifiers(TtyInputState *input, const struct wlr_keyboard_modifiers *mods) {
    wlr_keyboard_notify_modifiers(&input->client_keyboard, mods->depressed, mods->latched,
                                  mods->locked, mods->group);
    if (input->seat && !seaside_text_input_modifiers(input->text_input, &input->client_keyboard)) {
        seaside_text_input_restore_keyboard(input->text_input);
        wlr_seat_keyboard_notify_modifiers(input->seat, &input->client_keyboard.modifiers);
    }
}

static void handle_group_modifiers(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, group_modifiers);
    auto *keyboard = static_cast<const wlr_keyboard *>(data);
    deliver_modifiers(input, &keyboard->modifiers);
}

static void handle_group_enter(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, group_enter);
    if (!input->shortcut_state) {
        return;
    }
    auto *keys = static_cast<const wl_array *>(data);
    for (const uint32_t key : wl_array_span<uint32_t>(keys)) {
        xkb_state_update_key(input->shortcut_state, key + 8, XKB_KEY_DOWN);
    }
    update_binding_modifiers(input);
}

static void handle_group_leave(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, group_leave);
    auto *keys = static_cast<const wl_array *>(data);
    const uint32_t time_msec = monotonic_msec();
    for (const uint32_t key : wl_array_span<uint32_t>(keys)) {
        if (input->shortcut_state) {
            xkb_state_update_key(input->shortcut_state, key + 8, XKB_KEY_UP);
        }
        deliver_key(input, wlr_keyboard_key_event{
                               .time_msec = time_msec,
                               .keycode = key,
                               .update_state = false,
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

void TtyInput::set_sensitivity(double sensitivity) noexcept {
    auto *input = state_.get();
    if (!input) {
        return;
    }
    input->sensitivity = sensitivity;
    struct tty_device *device;
    wl_list_for_each(device, &input->devices, link) {
        configure_pointer(device->pointer_device, sensitivity);
    }
}

static void configure_scroll(struct tty_device *device, bool inverse) {
    struct libinput_device *pointer = device->pointer_device;
    const bool configured = pointer && libinput_device_config_scroll_has_natural_scroll(pointer) &&
                            libinput_device_config_scroll_set_natural_scroll_enabled(
                                pointer, inverse) == LIBINPUT_CONFIG_STATUS_SUCCESS;
    device->invert_axis = inverse && !configured;
}

void TtyInput::set_inverse_scroll(bool inverse) noexcept {
    auto *input = state_.get();
    if (input->inverse_scroll == inverse) {
        return;
    }
    input->inverse_scroll = inverse;
    struct tty_device *device;
    wl_list_for_each(device, &input->devices, link) {
        if (!device->keyboard) {
            configure_scroll(device, inverse);
        }
    }
}

void TtyInput::set_auto_center_on_lock(bool enabled) noexcept {
    state_->auto_center_on_lock = enabled;
}

static void handle_new_input(struct wl_listener *listener, void *data) noexcept {
    TtyInputState *input = wl_container_of(listener, input, new_input);
    auto *base = static_cast<wlr_input_device *>(data);
    if (base->type != WLR_INPUT_DEVICE_POINTER && base->type != WLR_INPUT_DEVICE_KEYBOARD) {
        return;
    }
    struct tty_device *device = new (std::nothrow) tty_device{};
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
            delete device;
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
        configure_scroll(device, input->inverse_scroll);
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
            refresh_pointer_focus(input);
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

static void detach(TtyInputState *input) {
    if (!input->attached) {
        return;
    }
    input->attached = false;
    cancel_seat_drag(input->seat);
    leave_constraint(input, true);
    set_keyboard_focus(input, nullptr);
    wl_list_remove(&input->new_input.link);
    wl_list_remove(&input->backend_destroy.link);
    /* Backend and per-device destroy signals can arrive in either order.
     * Remove all surviving listeners before the backend can reclaim its devices. */
    struct tty_device *device, *next;
    wl_list_for_each_safe(device, next, &input->devices, link) {
        remove_device(device);
    }
}

static void handle_backend_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, backend_destroy);
    detach(input);
}

TtyInput::TtyInput(std::unique_ptr<TtyInputState> state) noexcept : state_(std::move(state)) {}

std::expected<std::unique_ptr<TtyInput>, TtyInputError>
TtyInput::create(wlr_backend *backend, TtyInputObserver &observer,
                 TtyInputOptions options) noexcept {
    if (!backend) {
        return std::unexpected(TtyInputError::invalid_backend);
    }
    std::unique_ptr<TtyInputState> state(new (std::nothrow) TtyInputState{});
    if (!state) {
        return std::unexpected(TtyInputError::allocation);
    }
    std::unique_ptr<TtyInput> input(new (std::nothrow) TtyInput(std::move(state)));
    if (!input) {
        return std::unexpected(TtyInputError::allocation);
    }
    auto &attached = *input->state_;
    attached.observer = &observer;
    attached.options = options;
    wl_list_init(&attached.devices);
    wl_list_init(&attached.focus_destroy.link);
    attached.new_input.notify = handle_new_input;
    attached.backend_destroy.notify = handle_backend_destroy;
    wl_signal_add(&backend->events.new_input, &attached.new_input);
    wl_signal_add(&backend->events.destroy, &attached.backend_destroy);
    attached.attached = true;
    return input;
}

static const struct wlr_keyboard_impl client_keyboard_impl = {.name = "seaside-seat-keyboard",
                                                              .led_update = nullptr};

static void handle_seat_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyInputState *input = wl_container_of(listener, input, seat_destroy);
    seaside_text_input_destroy(input->text_input);
    input->text_input = nullptr;
    leave_constraint(input, false);
    wl_list_remove(&input->seat_destroy.link);
    wl_list_remove(&input->pointer_focus_change.link);
    leave_cursor_surface(input);
    if (input->options.client_cursor) {
        wl_list_remove(&input->request_cursor_surface.link);
    }
    wl_list_remove(&input->pointer_grab_begin.link);
    wl_list_remove(&input->pointer_grab_end.link);
    wl_list_remove(&input->keyboard_grab_end.link);
    wl_list_remove(&input->request_selection.link);
    wl_list_remove(&input->request_primary_selection.link);
    wl_list_remove(&input->request_drag.link);
    wl_list_remove(&input->focus_destroy.link);
    wl_list_init(&input->focus_destroy.link);
    input->focus = nullptr;
    struct wlr_seat *seat = input->seat;
    input->seat = nullptr;
    cancel_seat_drag(seat);
    /* A chosen drop can outlive its grab. Retire the source before its
     * destroy callback could access the freed seat. */
    wlr_data_source_destroy(seat->drag_source);
}

bool TtyInput::enable_keyboard(struct wl_display *display) noexcept {
    auto *input = state_.get();
    if (!display || !input->attached || input->group || input->snapshot.keyboards > 0) {
        return false;
    }
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        return false;
    }
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_context_unref(context);
    if (!keymap) {
        return false;
    }

    struct wlr_keyboard_group *group = wlr_keyboard_group_create();
    wlr_keyboard_init(&input->client_keyboard, &client_keyboard_impl, "Seaside keyboard");
    const bool needs_shortcut_state =
        input->options.track_modifiers || input->options.pointer_targets;
    struct xkb_state *shortcut_state = needs_shortcut_state ? xkb_state_new(keymap) : nullptr;
    bool mapped = group && (!needs_shortcut_state || shortcut_state) &&
                  wlr_keyboard_set_keymap(&group->keyboard, keymap) &&
                  wlr_keyboard_set_keymap(&input->client_keyboard, keymap);
    xkb_keymap_unref(keymap);
    struct wlr_seat *seat = mapped ? wlr_seat_create(display, "seat0") : nullptr;
    struct seaside_text_input *text_input =
        seat ? seaside_text_input_create(display, seat, &input->client_keyboard, *input->observer)
             : nullptr;
    struct wlr_relative_pointer_manager_v1 *relative_pointer = nullptr;
    struct wlr_pointer_constraints_v1 *constraints = nullptr;
    struct wl_protocol_logger *constraint_logger = nullptr;
    struct wlr_cursor_shape_manager_v1 *cursor_shapes = nullptr;
    const bool needs_cursor = input->options.client_cursor;
    if (seat && input->options.pointer_targets) {
        relative_pointer = wlr_relative_pointer_manager_v1_create(display);
        constraints = wlr_pointer_constraints_v1_create(display);
        if (constraints) {
            constraint_logger =
                wl_display_add_protocol_logger(display, track_constraint_region, input);
        }
    }
    if (seat && needs_cursor) {
        cursor_shapes = wlr_cursor_shape_manager_v1_create(display, 1);
    }
    if (!seat || !text_input ||
        (input->options.pointer_targets &&
         (!relative_pointer || !constraints || !constraint_logger)) ||
        (needs_cursor && !cursor_shapes) || !wlr_data_device_manager_create(display) ||
        !wlr_primary_selection_v1_device_manager_create(display) ||
        !wlr_ext_data_control_manager_v1_create(display, 1) ||
        !wlr_data_control_manager_v1_create(display)) {
        xkb_state_unref(shortcut_state);
        if (constraint_logger) {
            wl_protocol_logger_destroy(constraint_logger);
        }
        seaside_text_input_destroy(text_input);
        wlr_seat_destroy(seat);
        if (group) {
            wlr_keyboard_group_destroy(group);
        }
        wlr_keyboard_finish(&input->client_keyboard);
        return false;
    }

    input->seat = seat;
    input->text_input = text_input;
    input->group = group;
    input->shortcut_state = shortcut_state;
    input->seat_destroy.notify = handle_seat_destroy;
    input->pointer_focus_change.notify = handle_pointer_focus_change;
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
    if (cursor_shapes) {
        input->cursor_shapes = cursor_shapes;
        input->request_cursor_surface.notify = handle_cursor_surface_request;
        input->request_cursor_shape.notify = handle_cursor_shape_request;
        input->cursor_surface_commit.notify = handle_cursor_surface_commit;
        input->cursor_surface_destroy.notify = handle_cursor_surface_destroy;
        input->cursor_shapes_destroy.notify = handle_cursor_shapes_destroy;
        wl_signal_add(&seat->events.request_set_cursor, &input->request_cursor_surface);
        wl_signal_add(&cursor_shapes->events.request_set_shape, &input->request_cursor_shape);
        wl_signal_add(&cursor_shapes->events.destroy, &input->cursor_shapes_destroy);
    }
    wl_signal_add(&group->keyboard.events.key, &input->group_key);
    wl_signal_add(&group->keyboard.events.modifiers, &input->group_modifiers);
    wl_signal_add(&group->events.enter, &input->group_enter);
    wl_signal_add(&group->events.leave, &input->group_leave);
    if (relative_pointer && constraints) {
        input->relative_pointer = relative_pointer;
        input->constraints = constraints;
        input->constraint_logger = constraint_logger;
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

TtyInput::~TtyInput() noexcept {
    auto *input = state_.get();
    detach(input);
    wlr_seat_destroy(input->seat);
    if (input->relative_pointer) {
        wl_list_remove(&input->relative_pointer_destroy.link);
    }
    if (input->cursor_shapes) {
        forget_cursor_shapes(input);
    }
    if (input->constraints) {
        forget_constraint_protocol(input);
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
}

TtyInputSnapshot TtyInput::snapshot() const noexcept {
    auto *input = state_.get();
    TtyInputSnapshot snapshot = input->snapshot;
    snapshot.has_keyboard_focus = input->seat && input->seat->keyboard_state.focused_surface;
    return snapshot;
}

} // namespace seaside

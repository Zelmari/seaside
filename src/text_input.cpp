#include "text_input.hpp"
#include "wlr_input_method_compat.hpp"

#include <glib.h>
#include <linux/input-event-codes.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <type_traits>
#include <unistd.h>

extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
}

namespace seaside {

struct seaside_text_input {
    struct wlr_seat *seat;
    struct wlr_keyboard *keyboard;
    struct virtual_keyboard *forwarding;
    struct wlr_surface *focus;
    struct wlr_input_method_v2 *method;
    struct wlr_text_input_v3 *active;
    struct wl_list inputs, virtual_keyboards;
    struct wl_listener new_text_input, new_input_method;
    struct wl_listener text_manager_destroy, method_manager_destroy;
    struct wl_listener focus_change, method_commit, method_destroy, new_popup;
    struct wl_listener grab_keyboard, grab_destroy;
    struct wl_listener new_virtual_keyboard, virtual_manager_destroy;
    uint64_t grab_generation;
    uint64_t grabbed_keys[KEY_CNT];
    TtyInputObserver *observer;
};
static_assert(std::is_standard_layout_v<seaside_text_input>);

struct text_input {
    struct seaside_text_input *relay;
    struct wlr_text_input_v3 *input;
    struct wl_list link;
    struct wl_listener enable, commit, disable, destroy;
};
static_assert(std::is_standard_layout_v<text_input>);

struct virtual_keyboard {
    struct seaside_text_input *relay;
    struct wlr_virtual_keyboard_v1 *virtual_kbd;
    struct wl_list link;
    struct wl_listener key, modifiers, destroy;
    bool delivered[KEY_CNT];
};
static_assert(std::is_standard_layout_v<virtual_keyboard>);

static void remove_listener(struct wl_listener *listener) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

static void update_popup(struct seaside_text_input *relay,
                         struct wlr_input_popup_surface_v2 *popup) {
    struct wlr_text_input_v3 *input = relay->active;
    const struct wlr_box *cursor =
        input && (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CURSOR_RECTANGLE)
            ? &input->current.cursor_rectangle
            : nullptr;
    relay->observer->input_popup(popup, input ? input->focused_surface : nullptr, cursor);
}

static void update_popups(struct seaside_text_input *relay) {
    struct wlr_input_popup_surface_v2 *popup;
    wl_list_for_each(popup, &relay->method->popup_surfaces, link) {
        update_popup(relay, popup);
    }
}

void seaside_text_input_restore_keyboard(struct seaside_text_input *relay) {
    if (!relay || !relay->forwarding) {
        return;
    }
    struct virtual_keyboard *forwarding = relay->forwarding;
    relay->forwarding = nullptr;
    if (relay->focus == relay->seat->keyboard_state.focused_surface) {
        for (size_t key = 0; key < KEY_CNT; ++key) {
            if (forwarding->delivered[key]) {
                wlr_seat_keyboard_notify_key(relay->seat, 0, key, WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
    }
    for (size_t key = 0; key < KEY_CNT; ++key) {
        forwarding->delivered[key] = false;
    }
    wlr_seat_set_keyboard(relay->seat, relay->keyboard);
}

static void deactivate(struct seaside_text_input *relay) {
    if (!relay->active) {
        return;
    }
    struct wlr_input_method_keyboard_grab_v2 *grab =
        relay->method ? relay->method->keyboard_grab : nullptr;
    if (grab) {
        for (size_t key = 0; key < KEY_CNT; ++key) {
            if (relay->grabbed_keys[key] == relay->grab_generation) {
                wlr_input_method_keyboard_grab_v2_send_key(grab, 0, key,
                                                           WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }
    }
    relay->active = nullptr;
    ++relay->grab_generation;
    seaside_text_input_restore_keyboard(relay);
    if (relay->method) {
        wlr_input_method_v2_send_deactivate(relay->method);
        wlr_input_method_v2_send_done(relay->method);
        update_popups(relay);
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(relay->seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_modifiers(relay->seat, &keyboard->modifiers);
    }
}

/* Values from zwp_text_input_v3's content_hint and content_purpose enums. */
enum {
    text_input_hidden = 0x40,
    text_input_sensitive = 0x80,
    text_input_password = 8,
    text_input_pin = 9,
    text_input_max_bytes = 4000,
};

static bool text_length(const char *text, size_t *length) {
    *length = text ? strnlen(text, text_input_max_bytes + 1) : 0;
    return !text ||
           (*length <= text_input_max_bytes && g_utf8_validate_len(text, *length, nullptr));
}

static bool text_offset(const char *text, size_t length, size_t offset) {
    return offset <= length && (offset == length || ((uint8_t)text[offset] & 0xc0) != 0x80);
}

static bool read_surrounding_text(const struct wlr_text_input_v3 *input, size_t *length) {
    const struct wlr_text_input_v3_state *state = &input->current;
    const char *text = state->surrounding.text;
    const bool protected_text =
        (state->content_type.hint & (text_input_hidden | text_input_sensitive)) ||
        state->content_type.purpose == text_input_password ||
        state->content_type.purpose == text_input_pin;
    return !protected_text &&
           (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) &&
           text_length(text, length) && text_offset(text, *length, state->surrounding.cursor) &&
           text_offset(text, *length, state->surrounding.anchor);
}

static void send_state(struct seaside_text_input *relay) {
    if (!relay->method) {
        return;
    }
    const struct wlr_text_input_v3 *input = relay->active;
    const struct wlr_text_input_v3_state *state = &input->current;
    if (!relay->method->active) {
        // Each activation starts a new input context and discards the old edit batch.
        free(relay->method->pending.commit_text);
        free(relay->method->pending.preedit.text);
        relay->method->pending = wlr_input_method_v2_state{};
        wlr_input_method_v2_send_activate(relay->method);
    }
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_SURROUNDING_TEXT) {
        size_t length;
        if (read_surrounding_text(input, &length) && state->surrounding.text) {
            wlr_input_method_v2_send_surrounding_text(relay->method, state->surrounding.text,
                                                      state->surrounding.cursor,
                                                      state->surrounding.anchor);
        } else {
            wlr_input_method_v2_send_surrounding_text(relay->method, "", 0, 0);
        }
    }
    wlr_input_method_v2_send_text_change_cause(relay->method, state->text_change_cause);
    if (input->active_features & WLR_TEXT_INPUT_V3_FEATURE_CONTENT_TYPE) {
        wlr_input_method_v2_send_content_type(relay->method, state->content_type.hint,
                                              state->content_type.purpose);
    }
    wlr_input_method_v2_send_done(relay->method);
    update_popups(relay);
}

static void reset_text_state(struct wlr_text_input_v3 *input) {
    // The commit counter belongs to the object and survives focus changes.
    free(input->pending.surrounding.text);
    free(input->current.surrounding.text);
    input->pending = wlr_text_input_v3_state{};
    input->current = wlr_text_input_v3_state{};
    input->pending_enabled = false;
    input->current_enabled = false;
    input->active_features = 0;
}

static void text_enable(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct text_input *binding = wl_container_of(listener, binding, enable);
    struct seaside_text_input *relay = binding->relay;
    struct wlr_text_input_v3 *input = binding->input;
    if (!input->focused_surface || input->focused_surface != relay->focus ||
        (relay->active && relay->active != input)) {
        reset_text_state(input);
        return;
    }
    relay->active = input;
    send_state(relay);
    input->pending.text_change_cause = 0;
}

static void text_disable(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct text_input *binding = wl_container_of(listener, binding, disable);
    if (binding->relay->active == binding->input) {
        deactivate(binding->relay);
    }
}

static void text_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct text_input *binding = wl_container_of(listener, binding, commit);
    if (binding->relay->active == binding->input) {
        // A second enable on the same object arrives as a commit in wlroots.
        // The newly enabled editor may expose different features.
        binding->input->active_features = binding->input->current.features;
        send_state(binding->relay);
        binding->input->pending.text_change_cause = 0;
    }
}

static void destroy_binding(struct text_input *binding) {
    if (binding->relay->active == binding->input) {
        deactivate(binding->relay);
    }
    wl_list_remove(&binding->link);
    wl_list_remove(&binding->enable.link);
    wl_list_remove(&binding->disable.link);
    wl_list_remove(&binding->commit.link);
    wl_list_remove(&binding->destroy.link);
    delete binding;
}

static void text_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct text_input *binding = wl_container_of(listener, binding, destroy);
    destroy_binding(binding);
}

static void enter(struct text_input *binding, struct wlr_surface *focus) {
    if (focus && wl_resource_get_client(binding->input->resource) ==
                     wl_resource_get_client(focus->resource)) {
        wlr_text_input_v3_send_enter(binding->input, focus);
    }
}

static void focus_change(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, focus_change);
    auto *event = static_cast<const wlr_seat_keyboard_focus_change_event *>(data);
    deactivate(relay);
    relay->focus = event->new_surface;
    struct text_input *binding;
    wl_list_for_each(binding, &relay->inputs, link) {
        if (binding->input->focused_surface) {
            wlr_text_input_v3_send_leave(binding->input);
        }
        reset_text_state(binding->input);
        enter(binding, relay->focus);
    }
}

static void new_text_input(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, new_text_input);
    auto *input = static_cast<wlr_text_input_v3 *>(data);
    if (input->seat != relay->seat) {
        return;
    }
    struct text_input *binding = new (std::nothrow) text_input{};
    if (!binding) {
        wl_client_post_no_memory(wl_resource_get_client(input->resource));
        return;
    }
    binding->relay = relay;
    binding->input = input;
    binding->enable.notify = text_enable;
    binding->disable.notify = text_disable;
    binding->commit.notify = text_commit;
    binding->destroy.notify = text_destroy;
    wl_signal_add(&input->events.enable, &binding->enable);
    wl_signal_add(&input->events.disable, &binding->disable);
    wl_signal_add(&input->events.commit, &binding->commit);
    wl_signal_add(&input->events.destroy, &binding->destroy);
    wl_list_insert(&relay->inputs, &binding->link);
    enter(binding, relay->focus);
}

static bool valid_deletion(const struct wlr_text_input_v3 *input, uint32_t before, uint32_t after) {
    size_t length;
    if (!read_surrounding_text(input, &length)) {
        return false;
    }
    const uint32_t cursor = input->current.surrounding.cursor;
    const uint32_t anchor = input->current.surrounding.anchor;
    const size_t start = MIN(cursor, anchor);
    const size_t end = MAX(cursor, anchor);
    const char *text = input->current.surrounding.text;
    return before <= start && after <= length - end && text_offset(text, length, start - before) &&
           text_offset(text, length, end + after);
}

static void method_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, method_commit);
    struct wlr_text_input_v3 *input = relay->active;
    if (!input || !input->focused_surface || !relay->method->client_active) {
        return;
    }
    /* wlroots emits this signal only for the current input method done serial.
     * It also supplies the text client's separate commit serial in send_done. */
    const struct wlr_input_method_v2_state *state = &relay->method->current;
    const uint32_t before = state->wlr_delete.before_length, after = state->wlr_delete.after_length;
    if ((before || after) && valid_deletion(input, before, after)) {
        wlr_text_input_v3_send_delete_surrounding_text(input, before, after);
    }
    size_t length;
    if (state->commit_text && text_length(state->commit_text, &length)) {
        wlr_text_input_v3_send_commit_string(input, state->commit_text);
    }
    const int32_t start = state->preedit.cursor_begin, end = state->preedit.cursor_end;
    const char *preedit = state->preedit.text;
    if (preedit && text_length(preedit, &length) &&
        ((start == -1 && end == -1) ||
         (start >= 0 && end >= 0 && text_offset(preedit, length, start) &&
          text_offset(preedit, length, end)))) {
        wlr_text_input_v3_send_preedit_string(input, preedit, start, end);
    } else {
        wlr_text_input_v3_send_preedit_string(input, nullptr, 0, 0);
    }
    wlr_text_input_v3_send_done(input);
}

static void new_popup(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, new_popup);
    update_popup(relay, static_cast<wlr_input_popup_surface_v2 *>(data));
}

static void grab_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, grab_destroy);
    remove_listener(&relay->grab_destroy);
    ++relay->grab_generation;
    seaside_text_input_restore_keyboard(relay);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(relay->seat);
    if (keyboard) {
        wlr_seat_keyboard_notify_modifiers(relay->seat, &keyboard->modifiers);
    }
}

static void grab_keyboard(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, grab_keyboard);
    auto *grab = static_cast<wlr_input_method_keyboard_grab_v2 *>(data);
    if (wlr_seat_get_keyboard(relay->seat)) {
        wlr_input_method_keyboard_grab_v2_set_keyboard(grab, relay->keyboard);
    }
    wl_signal_add(&grab->events.destroy, &relay->grab_destroy);
}

static void detach_method(struct seaside_text_input *relay) {
    remove_listener(&relay->method_commit);
    remove_listener(&relay->method_destroy);
    remove_listener(&relay->new_popup);
    remove_listener(&relay->grab_keyboard);
    remove_listener(&relay->grab_destroy);
    relay->method = nullptr;
}

static void method_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, method_destroy);
    seaside_text_input_restore_keyboard(relay);
    if (relay->active && relay->active->focused_surface) {
        wlr_text_input_v3_send_preedit_string(relay->active, nullptr, 0, 0);
        wlr_text_input_v3_send_done(relay->active);
    }
    detach_method(relay);
}

static void new_input_method(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, new_input_method);
    auto *method = static_cast<wlr_input_method_v2 *>(data);
    uid_t uid;
    wl_client_get_credentials(wl_resource_get_client(method->resource), nullptr, &uid, nullptr);
    if (method->seat != relay->seat || relay->method || uid != geteuid()) {
        wlr_input_method_v2_send_unavailable(method);
        return;
    }
    relay->method = method;
    wl_signal_add(&method->events.commit, &relay->method_commit);
    wl_signal_add(&method->events.destroy, &relay->method_destroy);
    wl_signal_add(&method->events.new_popup_surface, &relay->new_popup);
    wl_signal_add(&method->events.grab_keyboard, &relay->grab_keyboard);
    if (relay->active) {
        send_state(relay);
    }
}

static void text_manager_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, text_manager_destroy);
    remove_listener(&relay->text_manager_destroy);
    remove_listener(&relay->new_text_input);
}

static void method_manager_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, method_manager_destroy);
    remove_listener(&relay->method_manager_destroy);
    remove_listener(&relay->new_input_method);
}

static bool can_forward(const struct virtual_keyboard *binding) {
    const struct seaside_text_input *relay = binding->relay;
    return relay->active && relay->method && binding->virtual_kbd->has_keymap &&
           wlr_seat_get_keyboard(relay->seat) && !wlr_seat_keyboard_has_grab(relay->seat) &&
           wl_resource_get_client(binding->virtual_kbd->resource) ==
               wl_resource_get_client(relay->method->resource);
}

static void select_virtual_keyboard(struct virtual_keyboard *binding) {
    struct seaside_text_input *relay = binding->relay;
    if (relay->forwarding != binding) {
        seaside_text_input_restore_keyboard(relay);
        relay->forwarding = binding;
        wlr_seat_set_keyboard(relay->seat, &binding->virtual_kbd->keyboard);
    }
}

static void virtual_key(struct wl_listener *listener, void *data) noexcept {
    struct virtual_keyboard *binding = wl_container_of(listener, binding, key);
    auto *event = static_cast<const wlr_keyboard_key_event *>(data);
    if (event->keycode >= KEY_CNT || !can_forward(binding)) {
        return;
    }
    bool *delivered = &binding->delivered[event->keycode];
    const bool pressed = event->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (pressed == *delivered) {
        return;
    }
    select_virtual_keyboard(binding);
    *delivered = pressed;
    // Forwarded IME keys go directly to the focused client and never trigger a binding.
    wlr_seat_keyboard_notify_key(binding->relay->seat, event->time_msec, event->keycode,
                                 event->state);
}

static void virtual_modifiers(struct wl_listener *listener, void *data) noexcept {
    struct virtual_keyboard *binding = wl_container_of(listener, binding, modifiers);
    auto *keyboard = static_cast<wlr_keyboard *>(data);
    if (can_forward(binding)) {
        select_virtual_keyboard(binding);
        wlr_seat_keyboard_notify_modifiers(binding->relay->seat, &keyboard->modifiers);
    }
}

static void remove_virtual_keyboard(struct virtual_keyboard *binding) {
    if (binding->relay->forwarding == binding) {
        seaside_text_input_restore_keyboard(binding->relay);
    }
    wl_list_remove(&binding->link);
    wl_list_remove(&binding->key.link);
    wl_list_remove(&binding->modifiers.link);
    wl_list_remove(&binding->destroy.link);
    delete binding;
}

static void virtual_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct virtual_keyboard *binding = wl_container_of(listener, binding, destroy);
    remove_virtual_keyboard(binding);
}

static void new_virtual_keyboard(struct wl_listener *listener, void *data) noexcept {
    struct seaside_text_input *relay = wl_container_of(listener, relay, new_virtual_keyboard);
    auto *keyboard = static_cast<wlr_virtual_keyboard_v1 *>(data);
    if (keyboard->seat != relay->seat) {
        return;
    }
    struct virtual_keyboard *binding = new (std::nothrow) virtual_keyboard{};
    if (!binding) {
        wl_client_post_no_memory(wl_resource_get_client(keyboard->resource));
        return;
    }
    binding->relay = relay;
    binding->virtual_kbd = keyboard;
    binding->key.notify = virtual_key;
    binding->modifiers.notify = virtual_modifiers;
    binding->destroy.notify = virtual_destroy;
    wl_list_insert(&relay->virtual_keyboards, &binding->link);
    wl_signal_add(&keyboard->keyboard.events.key, &binding->key);
    wl_signal_add(&keyboard->keyboard.events.modifiers, &binding->modifiers);
    wl_signal_add(&keyboard->keyboard.base.events.destroy, &binding->destroy);
}

static void virtual_manager_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct seaside_text_input *relay = wl_container_of(listener, relay, virtual_manager_destroy);
    remove_listener(&relay->virtual_manager_destroy);
    remove_listener(&relay->new_virtual_keyboard);
}

struct seaside_text_input *seaside_text_input_create(struct wl_display *display,
                                                     struct wlr_seat *seat,
                                                     struct wlr_keyboard *keyboard,
                                                     TtyInputObserver &observer) {
    struct seaside_text_input *relay = new (std::nothrow) seaside_text_input{};
    if (!relay) {
        return nullptr;
    }
    struct wlr_text_input_manager_v3 *text = wlr_text_input_manager_v3_create(display);
    struct wlr_input_method_manager_v2 *method = wlr_input_method_manager_v2_create(display);
    struct wlr_virtual_keyboard_manager_v1 *virtual_manager =
        wlr_virtual_keyboard_manager_v1_create(display);
    if (!text || !method || !virtual_manager) {
        delete relay;
        return nullptr;
    }
    relay->seat = seat;
    relay->keyboard = keyboard;
    relay->focus = seat->keyboard_state.focused_surface;
    relay->observer = &observer;
    relay->grab_generation = 1;
    wl_list_init(&relay->inputs);
    wl_list_init(&relay->virtual_keyboards);
    wl_list_init(&relay->grab_destroy.link);
    relay->new_text_input.notify = new_text_input;
    relay->new_input_method.notify = new_input_method;
    relay->text_manager_destroy.notify = text_manager_destroy;
    relay->method_manager_destroy.notify = method_manager_destroy;
    relay->focus_change.notify = focus_change;
    relay->method_commit.notify = method_commit;
    relay->method_destroy.notify = method_destroy;
    relay->new_popup.notify = new_popup;
    relay->grab_keyboard.notify = grab_keyboard;
    relay->grab_destroy.notify = grab_destroy;
    relay->new_virtual_keyboard.notify = new_virtual_keyboard;
    relay->virtual_manager_destroy.notify = virtual_manager_destroy;
    wl_signal_add(&text->events.new_text_input, &relay->new_text_input);
    wl_signal_add(&text->events.destroy, &relay->text_manager_destroy);
    wl_signal_add(&method->events.new_input_method, &relay->new_input_method);
    wl_signal_add(&method->events.destroy, &relay->method_manager_destroy);
    wl_signal_add(&virtual_manager->events.new_virtual_keyboard, &relay->new_virtual_keyboard);
    wl_signal_add(&virtual_manager->events.destroy, &relay->virtual_manager_destroy);
    wl_signal_add(&seat->keyboard_state.events.focus_change, &relay->focus_change);
    return relay;
}

void seaside_text_input_destroy(struct seaside_text_input *relay) {
    if (!relay) {
        return;
    }
    deactivate(relay);
    if (relay->method) {
        detach_method(relay);
    }
    struct virtual_keyboard *virtual_kbd, *next_virtual;
    wl_list_for_each_safe(virtual_kbd, next_virtual, &relay->virtual_keyboards, link) {
        remove_virtual_keyboard(virtual_kbd);
    }
    struct text_input *binding, *next;
    wl_list_for_each_safe(binding, next, &relay->inputs, link) {
        destroy_binding(binding);
    }
    remove_listener(&relay->focus_change);
    remove_listener(&relay->new_text_input);
    remove_listener(&relay->new_input_method);
    remove_listener(&relay->text_manager_destroy);
    remove_listener(&relay->method_manager_destroy);
    remove_listener(&relay->new_virtual_keyboard);
    remove_listener(&relay->virtual_manager_destroy);
    delete relay;
}

static struct wlr_input_method_keyboard_grab_v2 *active_grab(struct seaside_text_input *relay,
                                                             struct wlr_keyboard *keyboard) {
    if (!relay->active || !relay->method || !relay->method->keyboard_grab ||
        wlr_seat_keyboard_has_grab(relay->seat)) {
        return nullptr;
    }
    struct wlr_input_method_keyboard_grab_v2 *grab = relay->method->keyboard_grab;
    wlr_input_method_keyboard_grab_v2_set_keyboard(grab, keyboard);
    return grab;
}

bool seaside_text_input_key(struct seaside_text_input *relay, struct wlr_keyboard *keyboard,
                            const struct wlr_keyboard_key_event *event) {
    if (!relay || event->keycode >= KEY_CNT) {
        return false;
    }
    uint64_t *generation = &relay->grabbed_keys[event->keycode];
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        const uint64_t pressed = *generation;
        *generation = 0;
        if (!pressed) {
            return false;
        }
        struct wlr_input_method_keyboard_grab_v2 *grab =
            relay->method ? relay->method->keyboard_grab : nullptr;
        if (grab && pressed == relay->grab_generation) {
            wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec, event->keycode,
                                                       event->state);
        }
        return true;
    }
    struct wlr_input_method_keyboard_grab_v2 *grab = active_grab(relay, keyboard);
    if (grab) {
        *generation = relay->grab_generation;
        wlr_input_method_keyboard_grab_v2_send_key(grab, event->time_msec, event->keycode,
                                                   event->state);
    }
    return grab != nullptr;
}

bool seaside_text_input_modifiers(struct seaside_text_input *relay, struct wlr_keyboard *keyboard) {
    struct wlr_input_method_keyboard_grab_v2 *grab = relay ? active_grab(relay, keyboard) : nullptr;
    if (grab) {
        wlr_input_method_keyboard_grab_v2_send_modifiers(grab, &keyboard->modifiers);
    }
    return grab != nullptr;
}

} // namespace seaside

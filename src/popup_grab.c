#include "popup_grab.h"

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

static struct wlr_surface *owner_surface(struct wlr_surface *surface) {
    while (surface) {
        struct wlr_xdg_popup *parent = wlr_xdg_popup_try_from_wlr_surface(surface);
        if (!parent) {
            break;
        }
        surface = parent->parent;
    }
    return surface;
}

static void enter(struct wlr_seat *seat, struct wlr_surface *surface) {
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    if (keyboard) {
        // The XDG grab intentionally ignores notify_enter. Give the topmost
        // grabbed popup the seat's filtered set of pressed client keys.
        wlr_seat_keyboard_enter(seat, surface, keyboard->keycodes, keyboard->num_keycodes,
                                &keyboard->modifiers);
    }
}

static void on_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_popup_focus *focus = wl_container_of(listener, focus, map);
    struct wlr_xdg_popup *popup = focus->role;
    if (!popup->seat || !popup->parent ||
        owner_surface(popup->seat->keyboard_state.focused_surface) !=
            owner_surface(popup->parent)) {
        return;
    }
    struct wlr_xdg_popup_grab *grab;
    wl_list_for_each(grab, &popup->base->client->shell->popup_grabs, link) {
        if (grab->seat == popup->seat && grab->seat->keyboard_state.grab == &grab->keyboard_grab &&
            grab->popups.next == &popup->grab_link) {
            enter(popup->seat, popup->base->surface);
            return;
        }
    }
}

static void on_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_popup_focus *focus = wl_container_of(listener, focus, unmap);
    struct wlr_xdg_popup *popup = focus->role;
    if (popup->seat && popup->seat->keyboard_state.focused_surface == popup->base->surface &&
        popup->parent && popup->parent->mapped) {
        enter(popup->seat, popup->parent);
    }
}

void seaside_popup_focus_init(struct seaside_popup_focus *focus, struct wlr_xdg_popup *role) {
    focus->role = role;
    focus->map.notify = on_map;
    focus->unmap.notify = on_unmap;
    wl_signal_add(&role->base->surface->events.map, &focus->map);
    wl_signal_add(&role->base->surface->events.unmap, &focus->unmap);
}

void seaside_popup_focus_finish(struct seaside_popup_focus *focus) {
    wl_list_remove(&focus->map.link);
    wl_list_remove(&focus->unmap.link);
}

static struct wlr_xdg_popup *unfocused_popup(struct wlr_xdg_shell *shell,
                                             struct wlr_surface *target) {
    struct wlr_xdg_popup_grab *grab;
    wl_list_for_each(grab, &shell->popup_grabs, link) {
        if (grab->seat->keyboard_state.grab != &grab->keyboard_grab) {
            continue;
        }
        struct wlr_xdg_popup *popup;
        wl_list_for_each(popup, &grab->popups, grab_link) {
            if (owner_surface(popup->parent) != target) {
                return popup;
            }
        }
    }
    return NULL;
}

void seaside_popup_dismiss_unfocused(struct wlr_xdg_shell *shell, struct wlr_surface *target) {
    // Walk newest first. Dismissal can destroy the last grab and its list.
    struct wlr_xdg_popup *popup;
    while ((popup = unfocused_popup(shell, target))) {
        wlr_xdg_popup_destroy(popup);
    }
}

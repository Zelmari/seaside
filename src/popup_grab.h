#ifndef SEASIDE_POPUP_GRAB_H
#define SEASIDE_POPUP_GRAB_H

#include <wayland-server-core.h>

struct wlr_surface;
struct wlr_xdg_popup;
struct wlr_xdg_shell;

struct seaside_popup_focus {
    struct wlr_xdg_popup *role;
    struct wl_listener map, unmap;
};

#ifdef __cplusplus
extern "C" {
#endif

void seaside_popup_focus_init(struct seaside_popup_focus *focus, struct wlr_xdg_popup *role);
void seaside_popup_focus_finish(struct seaside_popup_focus *focus);
void seaside_popup_dismiss_unfocused(struct wlr_xdg_shell *shell, struct wlr_surface *target);

#ifdef __cplusplus
}
#endif

#endif

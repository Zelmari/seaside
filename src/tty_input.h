#ifndef SEASIDE_TTY_INPUT_H
#define SEASIDE_TTY_INPUT_H

#include "cursor_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct timespec;
struct wlr_backend;
struct wlr_drag_icon;
struct wlr_box;
struct wlr_input_popup_surface_v2;
struct wlr_output;
struct wlr_seat;
struct wlr_seat_client;
struct wlr_surface;
struct seaside_tty_input;

struct seaside_tty_pointer_target {
    struct wlr_surface *surface;
    double sx, sy;
    uint64_t window_id;
};

struct seaside_tty_input_callbacks {
    /* Absolute coordinates are normalized. Relative and confined values are deltas. */
    void (*motion)(void *data, enum seaside_pointer_motion kind, double x, double y);
    /* Escape requests shutdown only in the TTY qualification fixture. */
    void (*exit_requested)(void *data);
    /* An optional exact Super+Return shortcut in the terminal fixture. */
    void (*launch_requested)(void *data);
    /* Modifier bits match the binding table (Super, Ctrl, Alt, Shift).
     * Returning true consumes both the press and release. */
    bool (*shortcut_pressed)(void *data, uint8_t modifiers, uint32_t keysym);
    void (*modifiers_changed)(void *data, uint8_t modifiers);
    /* Hit-test for a NULL surface. Otherwise locate that exact visible surface
     * and report its local coordinates, even outside its input bounds. */
    bool (*pointer_target)(void *data, struct seaside_tty_pointer_target *target);
    /* A press may be consumed with its release. window_id is zero over empty
     * space. Existing client grabs take priority over this callback. */
    bool (*pointer_button)(void *data, uint64_t window_id, uint32_t button, bool pressed,
                           uint8_t modifiers);
    /* Convert normalized hardware coordinates for pointer confinement. */
    bool (*pointer_absolute_delta)(void *data, double x, double y, double *dx, double *dy);
    void (*pointer_locked)(void *data, bool locked);
    /* NULL shape restores the desktop arrow. NULL surface hides the cursor. */
    void (*cursor_shape)(void *data, const char *shape);
    void (*cursor_surface)(void *data, struct wlr_surface *surface, int32_t x, int32_t y);
    /* The initiating device disappeared before its consumed button release. */
    void (*pointer_cancelled)(void *data);
    /* The icon is borrowed. The scene removes its node on icon destruction. */
    void (*drag_icon)(void *data, struct wlr_drag_icon *icon);
    /* An inactive popup has no focus or cursor rectangle. */
    void (*input_popup)(void *data, struct wlr_input_popup_surface_v2 *popup,
                        struct wlr_surface *focus, const struct wlr_box *cursor);
    /* Local input also counts when a binding or pointer constraint consumes it.
     * Motion reports after routing so it can wake the destination output. */
    void (*activity)(void *data, bool pointer);
};

struct seaside_tty_input_snapshot {
    size_t pointers;
    size_t keyboards;
    bool failed;
    bool has_keyboard_focus;
    uint64_t forwarded_key_events;
    uint64_t forwarded_key_presses;
    uint64_t launch_requests;
};

#ifdef __cplusplus
extern "C" {
#endif

/* Borrows the backend and copies callbacks. Attach on the Wayland thread before
 * starting the backend: device discovery can happen synchronously at start.
 * Callbacks may request loop termination. Destroy after dispatch has stopped. */
struct seaside_tty_input *
seaside_tty_input_create(struct wlr_backend *backend,
                         const struct seaside_tty_input_callbacks *callbacks, void *data);

/* Publish the seat and its protocol globals before backend start. Keyboard
 * focus borrows the mapped surface until it is cleared or destroyed. */
bool seaside_tty_input_enable_keyboard(struct seaside_tty_input *input, struct wl_display *display);
void seaside_tty_input_set_sensitivity(struct seaside_tty_input *input, double sensitivity);
void seaside_tty_input_set_inverse_scroll(struct seaside_tty_input *input, bool inverse);
void seaside_tty_input_set_focus(struct seaside_tty_input *input, struct wlr_surface *surface);
struct wlr_seat *seaside_tty_input_get_seat(const struct seaside_tty_input *input);
/* Reevaluate hover after a scene change, even when the cursor has not moved. */
void seaside_tty_input_refresh_pointer(struct seaside_tty_input *input);
void seaside_tty_input_cursor_presented(struct seaside_tty_input *input, struct wlr_output *output,
                                        const struct timespec *when);
/* Take over a live client grab and return its initiating button, or zero. */
uint32_t seaside_tty_input_begin_client_gesture(struct seaside_tty_input *input,
                                                struct wlr_seat_client *client,
                                                struct wlr_surface *root, uint32_t serial);
void seaside_tty_input_cancel_drag(struct seaside_tty_input *input);
void seaside_tty_input_cancel_drag_from(struct seaside_tty_input *input, struct wlr_surface *root);
void seaside_tty_input_destroy(struct seaside_tty_input *input);
struct seaside_tty_input_snapshot
seaside_tty_input_get_snapshot(const struct seaside_tty_input *input);

#ifdef __cplusplus
}
#endif

#endif

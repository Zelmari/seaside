#ifndef SEASIDE_TTY_INPUT_H
#define SEASIDE_TTY_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wlr_backend;
struct wlr_seat;
struct wlr_surface;
struct seaside_tty_input;

struct seaside_tty_pointer_target {
    struct wlr_surface *surface;
    double sx, sy;
    uint64_t window_id;
};

struct seaside_tty_input_callbacks {
    /* Absolute coordinates are normalized. Relative values are wlroots deltas. */
    void (*motion)(void *data, bool absolute, double x, double y);
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
void seaside_tty_input_set_focus(struct seaside_tty_input *input, struct wlr_surface *surface);
struct wlr_seat *seaside_tty_input_get_seat(const struct seaside_tty_input *input);
/* Reevaluate hover after a scene change, even when the cursor has not moved. */
void seaside_tty_input_refresh_pointer(struct seaside_tty_input *input);
void seaside_tty_input_destroy(struct seaside_tty_input *input);
struct seaside_tty_input_snapshot
seaside_tty_input_get_snapshot(const struct seaside_tty_input *input);

#ifdef __cplusplus
}
#endif

#endif

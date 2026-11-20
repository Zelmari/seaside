#ifndef SEASIDE_TTY_INPUT_H
#define SEASIDE_TTY_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wlr_backend;
struct wlr_surface;
struct seaside_tty_input;

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

/* Publish the keyboard seat and data-device global before backend start. Focus
 * borrows the mapped surface until it is cleared or destroyed. NULL clears it. */
bool seaside_tty_input_enable_keyboard(struct seaside_tty_input *input, struct wl_display *display);
void seaside_tty_input_set_focus(struct seaside_tty_input *input, struct wlr_surface *surface);
void seaside_tty_input_destroy(struct seaside_tty_input *input);
struct seaside_tty_input_snapshot
seaside_tty_input_get_snapshot(const struct seaside_tty_input *input);

#ifdef __cplusplus
}
#endif

#endif

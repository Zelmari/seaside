#ifndef SEASIDE_TTY_DRM_H
#define SEASIDE_TTY_DRM_H

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_output;
struct wlr_renderer;
struct seaside_tty_drm;

struct seaside_tty_drm_output_callbacks {
    bool (*added)(void *data, struct wlr_output *output, const char *name, const char *identity,
                  int x, int y, int width, int height);
    void (*removed)(void *data, struct wlr_output *output, const char *name);
};

struct seaside_tty_drm_observation {
    bool active;
    bool failed;
    bool cursor_presented;
    bool output_pending;
    uint32_t connector_id;
    uint32_t cursor_plane_id;
    uint64_t presented_frames;
    uint64_t cursor_moves;
    /* Page flips with a cursor position different from the previous flip. */
    uint64_t presented_moves;
};

#ifdef __cplusplus
extern "C" {
#endif

/* A real foreground VT is required before acquiring a seat or user resources. */
bool seaside_tty_active_console(void);

/* Owns the seat. A NULL device uses wlroots' normal seat GPU discovery.
 * On success transfer both returned handles to Server::start;
 * if not transferred, the caller must destroy them before destroying display.
 * Destroy the backend/display before releasing this owner. All calls use the
 * display event-loop thread. Session activation may dispatch that loop briefly. */
struct seaside_tty_drm *seaside_tty_drm_create(struct wl_display *display, const char *device_path,
                                               struct wlr_backend **backend,
                                               struct wlr_renderer **renderer);
void seaside_tty_drm_destroy(struct seaside_tty_drm *drm);

/* Register before enabling outputs. Strings and handles in the callback are
 * borrowed. Passing NULL clears both callbacks before their owner is freed. */
void seaside_tty_drm_set_output_callbacks(struct seaside_tty_drm *drm,
                                          const struct seaside_tty_drm_output_callbacks *callbacks,
                                          void *data);

/* Drain new and rejected output work outside backend/commit callbacks. Returns
 * false for a fatal owner or attachment error. A failing output is disabled
 * individually. Other outputs and the server can keep running. */
bool seaside_tty_drm_enable_pending(struct seaside_tty_drm *drm, struct wlr_allocator *allocator,
                                    struct wlr_renderer *renderer);

/* First-output qualification fixture. The result is borrowed. */
struct wlr_output *seaside_tty_drm_enable(struct seaside_tty_drm *drm,
                                          struct wlr_allocator *allocator,
                                          struct wlr_renderer *renderer);

/* Position and relative deltas use logical output-layout coordinates. Absolute
 * input is normalized against the bounds of the active outputs. */
bool seaside_tty_drm_cursor_position(const struct seaside_tty_drm *drm, double *x, double *y);
bool seaside_tty_drm_absolute_delta(const struct seaside_tty_drm *drm, double x, double y,
                                    double *dx, double *dy);
bool seaside_tty_drm_set_cursor_visible(struct seaside_tty_drm *drm, bool visible);
bool seaside_tty_drm_move(struct seaside_tty_drm *drm, bool absolute, double x, double y);
bool seaside_tty_drm_change_vt(struct seaside_tty_drm *drm, unsigned vt);
struct seaside_tty_drm_observation seaside_tty_drm_observation(const struct seaside_tty_drm *drm);

#ifdef __cplusplus
}
#endif

#endif

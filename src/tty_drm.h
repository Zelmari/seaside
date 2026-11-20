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

struct seaside_tty_drm_observation {
    bool active;
    bool failed;
    bool cursor_presented;
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

/* Owns the seat. On success transfer both returned handles to Server::start;
 * if not transferred, the caller must destroy them before destroying display.
 * Destroy the backend/display before releasing this owner. All calls use the
 * display event-loop thread. Session activation may dispatch that loop briefly. */
struct seaside_tty_drm *seaside_tty_drm_create(struct wl_display *display, const char *device_path,
                                               struct wlr_backend **backend,
                                               struct wlr_renderer **renderer);
void seaside_tty_drm_destroy(struct seaside_tty_drm *drm);

/* Called after backend start, before attaching the scene to the selected real
 * output. The result is borrowed until the output's destroy event. */
struct wlr_output *seaside_tty_drm_enable(struct seaside_tty_drm *drm,
                                          struct wlr_allocator *allocator,
                                          struct wlr_renderer *renderer);

/* Absolute coordinates are normalized, relative coordinates are logical pixels.
 * The accepted move count advances only for a changed cursor position. */
bool seaside_tty_drm_move(struct seaside_tty_drm *drm, bool absolute, double x, double y);
struct seaside_tty_drm_observation seaside_tty_drm_observation(const struct seaside_tty_drm *drm);

#ifdef __cplusplus
}
#endif

#endif

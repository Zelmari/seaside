#ifndef SEASIDE_TTY_DRM_H
#define SEASIDE_TTY_DRM_H

#include "cursor_layout.hpp"

#include <stdbool.h>
#include <stdint.h>

enum { SEASIDE_DEFAULT_CURSOR_SIZE = 16 };

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_output;
struct wlr_output_mode;
struct wlr_renderer;
struct wlr_surface;
struct timespec;
struct seaside_tty_drm;

struct seaside_tty_drm_output_callbacks {
    bool (*added)(void *data, struct wlr_output *output, const char *name, const char *identity,
                  int x, int y, int width, int height);
    void (*removed)(void *data, struct wlr_output *output, const char *name);
    void (*observed)(void *data, struct wlr_output *output, const char *name, const char *identity,
                     bool connected, const char *disabled_reason);
    void (*cursor_presented)(void *data, struct wlr_output *output, const struct timespec *when);
};

struct seaside_drm_output_settings {
    struct wlr_output_mode *mode;
    int width, height, refresh_millihz;
    int x, y, rotation;
    float scale;
    bool adaptive_sync;
};

struct seaside_drm_output_description {
    const char *make, *model;
    int preferred_width, preferred_height, max_refresh_millihz;
    int64_t max_pixels;
    bool adaptive_sync_supported;
};

struct seaside_drm_output_status {
    struct seaside_drm_output_settings settings;
    /* A sleeping output can keep a newer accepted mode for its next wake. */
    struct {
        int width, height, refresh_millihz;
    } last_mode;
    int logical_width, logical_height;
    bool sleeping;
    bool cursor;
    bool adaptive_sync;
    bool frame_pending;
    uint64_t committed_frames, presented_frames, discarded_frames, last_presentation_ns;
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
                                    struct wlr_renderer *renderer, struct wlr_output *preferred);

/* These handles and the selected mode are borrowed for one loop turn. */
struct seaside_drm_output_description seaside_tty_drm_describe_output(struct wlr_output *output);
bool seaside_tty_drm_output_status(struct seaside_tty_drm *drm, struct wlr_output *output,
                                   struct seaside_drm_output_status *status);
bool seaside_tty_drm_select_mode(struct wlr_output *output,
                                 struct seaside_drm_output_settings *settings);
bool seaside_tty_drm_check_settings(struct wlr_output *output,
                                    const struct seaside_drm_output_settings *settings);
bool seaside_tty_drm_apply_settings(struct seaside_tty_drm *drm, struct wlr_output *output,
                                    const struct seaside_drm_output_settings *settings);
/* Power transitions preserve the logical output, mode selection and workspace. */
bool seaside_tty_drm_set_output_power(struct seaside_tty_drm *drm, struct wlr_output *output,
                                      bool on);
void seaside_tty_drm_set_direct_scanout(struct seaside_tty_drm *drm, struct wlr_output *output,
                                        bool enabled);
void seaside_tty_drm_reject_unrestorable(struct seaside_tty_drm *drm, struct wlr_output *output);
/* Reject only unverified outputs when the active seat's initial presentation expires. */
void seaside_tty_drm_reject_unpresented_cursors(struct seaside_tty_drm *drm);
void seaside_tty_drm_reconcile_cursor(struct seaside_tty_drm *drm);

/* First-output qualification fixture. The result is borrowed. */
struct wlr_output *seaside_tty_drm_enable(struct seaside_tty_drm *drm,
                                          struct wlr_allocator *allocator,
                                          struct wlr_renderer *renderer);

/* Position and relative deltas use logical output-layout coordinates. Absolute
 * input is normalized against the bounds of the active outputs. */
bool seaside_tty_drm_cursor_position(const struct seaside_tty_drm *drm, double *x, double *y);
struct wlr_output *seaside_tty_drm_pointer_output(const struct seaside_tty_drm *drm);
bool seaside_tty_drm_absolute_delta(const struct seaside_tty_drm *drm, double x, double y,
                                    double *dx, double *dy);
bool seaside_tty_drm_set_cursor_visible(struct seaside_tty_drm *drm, bool visible);
/* A NULL shape restores the desktop arrow. A NULL cursor surface hides it. */
bool seaside_tty_drm_set_cursor_shape(struct seaside_tty_drm *drm, const char *shape);
bool seaside_tty_drm_set_cursor_surface(struct seaside_tty_drm *drm, struct wlr_surface *surface,
                                        int32_t hotspot_x, int32_t hotspot_y);
bool seaside_tty_drm_move(struct seaside_tty_drm *drm, enum seaside_pointer_motion kind, double x,
                          double y);
bool seaside_tty_drm_change_vt(struct seaside_tty_drm *drm, unsigned vt);
struct seaside_tty_drm_observation seaside_tty_drm_observation(const struct seaside_tty_drm *drm);

#ifdef __cplusplus
}
#endif

#endif

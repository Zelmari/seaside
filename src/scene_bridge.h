#ifndef SEASIDE_SCENE_BRIDGE_H
#define SEASIDE_SCENE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct wl_display;
struct wlr_buffer;
struct wlr_output;
struct wlr_surface;
struct seaside_scene_bridge;

typedef void (*seaside_scene_focus_fn)(void *data, struct wlr_surface *surface);
typedef void (*seaside_scene_output_appearance_fn)(void *data, struct wlr_output *output, int width,
                                                   float scale);
typedef void (*seaside_scene_image_release_fn)(void *data);

struct seaside_scene_rect {
    int x, y, width, height;
};

enum seaside_scene_window_event_kind {
    SEASIDE_WINDOW_CREATED,
    SEASIDE_WINDOW_INITIAL_COMMIT,
    SEASIDE_WINDOW_MAPPED,
    SEASIDE_WINDOW_UNMAPPED,
    SEASIDE_WINDOW_DESTROYED,
    SEASIDE_WINDOW_COMMITTED,
    SEASIDE_WINDOW_PARENT_CHANGED,
    SEASIDE_WINDOW_MAXIMIZE_REQUESTED,
    SEASIDE_WINDOW_FULLSCREEN_REQUESTED,
    SEASIDE_WINDOW_ATTENTION_REQUESTED,
};

struct seaside_scene_window_event {
    enum seaside_scene_window_event_kind kind;
    uint64_t id, parent;
    struct seaside_scene_rect geometry;
    bool enabled;
    bool ordinary_commit;
};

/* CREATED returns the logical ID. Other events return zero. */
typedef uint64_t (*seaside_scene_window_fn)(void *data,
                                            const struct seaside_scene_window_event *event);

struct seaside_scene_window_placement {
    struct seaside_scene_rect geometry, output, clip;
    uint64_t stacking;
    int border_width;
    bool visible, activated, maximized, fullscreen, tiled, resize, resizing;
};

struct seaside_scene_target {
    struct wlr_surface *surface;
    uint64_t id;
    double sx, sy;
};

struct seaside_scene_size_limits {
    int min_width, min_height, max_width, max_height;
};

struct seaside_scene_window_metadata {
    const char *app_id, *title;
};

struct seaside_scene_snapshot {
    size_t live_toplevels;
    size_t mapped_toplevels;
    size_t outputs;
    uint64_t initial_configures;
    uint64_t buffer_commits;
    uint64_t map_events;
    uint64_t unmap_events;
    uint64_t destroyed_toplevels;
    uint64_t frames;
    uint64_t failed_frames;
    bool failed;
};

#ifdef __cplusplus
extern "C" {
#endif

struct seaside_scene_bridge *seaside_scene_create(struct wl_display *display);
void seaside_scene_destroy(struct seaside_scene_bridge *scene);
/* Output mode, renderer initialization and cursor policy belong to the caller.
 * This owns only the scene viewport and its frame/destroy listeners. */
bool seaside_scene_add_output(struct seaside_scene_bridge *scene, struct wlr_output *output);
bool seaside_scene_place_output(struct seaside_scene_bridge *scene, struct wlr_output *output,
                                int x, int y);
void seaside_scene_remove_output(struct seaside_scene_bridge *scene, struct wlr_output *output);
struct seaside_scene_snapshot seaside_scene_get_snapshot(const struct seaside_scene_bridge *scene);

/* Successful creation holds the supplied pixel owner until the last scene
 * reference is released. Drop the producer reference or transfer it to the
 * wallpaper or banner setter. Pixels stay immutable until release. */
struct wlr_buffer *seaside_scene_create_image(int width, int height, const uint8_t *pixels,
                                              seaside_scene_image_release_fn release, void *owner);
void seaside_scene_drop_image(struct wlr_buffer *buffer);
void seaside_scene_set_wallpaper(struct seaside_scene_bridge *scene, struct wlr_buffer *buffer);
void seaside_scene_set_banner(struct seaside_scene_bridge *scene, struct wlr_output *output,
                              struct wlr_buffer *buffer, int logical_height);
/* Width zero reports removal or a viewport no longer eligible to draw. */
void seaside_scene_set_appearance_handler(struct seaside_scene_bridge *scene,
                                          seaside_scene_output_appearance_fn handler, void *data);

void seaside_scene_set_window_handler(struct seaside_scene_bridge *scene,
                                      seaside_scene_window_fn handler, void *data);
void seaside_scene_place_window(struct seaside_scene_bridge *scene, uint64_t id,
                                const struct seaside_scene_window_placement *placement);
struct wlr_surface *seaside_scene_window_surface(struct seaside_scene_bridge *scene, uint64_t id);
bool seaside_scene_close_window(struct seaside_scene_bridge *scene, uint64_t id);
struct seaside_scene_size_limits
seaside_scene_window_size_limits(struct seaside_scene_bridge *scene, uint64_t id);
/* Borrowed toplevel strings are valid until the next Wayland dispatch. */
struct seaside_scene_window_metadata
seaside_scene_get_window_metadata(struct seaside_scene_bridge *scene, uint64_t id);
/* Set target->surface to resolve an implicit grab or constrained surface. */
bool seaside_scene_window_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target);
/* Watch future maps from one owned client process. The focus callback gets a
 * borrowed mapped surface, then NULL when focus ends. Callbacks run on the
 * display owner thread and must return without reentering this API, destroying
 * the scene or throwing across C. Clear the watch before reaping the child or
 * releasing the callback target. */
bool seaside_scene_watch_client(struct seaside_scene_bridge *scene, pid_t pid,
                                seaside_scene_focus_fn focus, void *data);
void seaside_scene_clear_client_watch(struct seaside_scene_bridge *scene);

#ifdef __cplusplus
}
#endif

#endif

#ifndef SEASIDE_SCENE_BRIDGE_H
#define SEASIDE_SCENE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct wl_display;
struct wlr_output;
struct wlr_surface;
struct seaside_scene_bridge;

typedef void (*seaside_scene_focus_fn)(void *data, struct wlr_surface *surface);

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
struct seaside_scene_snapshot seaside_scene_get_snapshot(const struct seaside_scene_bridge *scene);
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

#ifndef SEASIDE_SCENE_BRIDGE_H
#define SEASIDE_SCENE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wlr_output;
struct seaside_scene_bridge;

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

#ifdef __cplusplus
}
#endif

#endif

#ifndef SEASIDE_SCENE_BRIDGE_H
#define SEASIDE_SCENE_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct wl_display;
struct wl_event_loop;
struct wlr_allocator;
struct wlr_buffer;
struct wlr_box;
struct wlr_compositor;
struct wlr_drag_icon;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_input_popup_surface_v2;
struct wlr_linux_dmabuf_v1;
struct wlr_output;
struct wlr_renderer;
struct wlr_seat_client;
struct wlr_surface;
struct seaside_scene_bridge;

typedef void (*seaside_scene_focus_fn)(void *data, struct wlr_surface *surface);
typedef void (*seaside_scene_output_appearance_fn)(void *data, struct wlr_output *output, int width,
                                                   float scale);
typedef void (*seaside_scene_image_release_fn)(void *data);
typedef struct wlr_output *(*seaside_layer_output_fn)(void *data);
typedef void (*seaside_layer_changed_fn)(void *data);
typedef void (*seaside_layer_hidden_fn)(void *data, struct wlr_surface *root);

struct seaside_scene_rect {
    int x, y, width, height;
};

enum seaside_output_support {
    SEASIDE_OUTPUT_UNKNOWN,
    SEASIDE_OUTPUT_UNSUPPORTED,
    SEASIDE_OUTPUT_SUPPORTED
};

struct seaside_scene_render_status {
    enum seaside_output_support tearing, night_light;
    bool tearing_applied, night_light_applied;
    const char *tearing_reason, *night_light_reason;
    const char *backend, *frame_state, *presentation_mode, *fallback_reason;
};

struct seaside_scene_frame_observation {
    const char *event;
    const char *presentation_mode;
    uint32_t commit_seq;
    bool succeeded;
    bool tearing;
    /* Monotonic presentation feedback. Zero means unavailable. */
    uint64_t presentation_ns;
};

typedef void (*seaside_scene_frame_fn)(void *data, struct wlr_output *output,
                                       const struct seaside_scene_frame_observation *observation);
typedef void (*seaside_scene_before_frame_fn)(void *data, struct wlr_output *output);

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
    SEASIDE_WINDOW_MOVE_REQUESTED,
    SEASIDE_WINDOW_RESIZE_REQUESTED,
};

enum seaside_scene_resize_edge {
    SEASIDE_RESIZE_TOP = 1,
    SEASIDE_RESIZE_BOTTOM = 2,
    SEASIDE_RESIZE_LEFT = 4,
    SEASIDE_RESIZE_RIGHT = 8,
};

struct seaside_scene_window_event {
    enum seaside_scene_window_event_kind kind;
    uint64_t id, parent;
    struct seaside_scene_rect geometry;
    bool enabled;
    bool ordinary_commit;
    struct wlr_seat_client *seat_client;
    /* The XDG configure for a map/commit, or the input serial for a gesture. */
    uint32_t serial;
    uint32_t resize_edges;
};

/* CREATED returns the logical ID. Other events return zero. */
typedef uint64_t (*seaside_scene_window_fn)(void *data,
                                            const struct seaside_scene_window_event *event);

struct seaside_scene_window_placement {
    struct seaside_scene_rect geometry, output, clip;
    uint64_t stacking;
    int border_width;
    bool visible, activated, maximized, fullscreen, stretch_fullscreen, tiled, resize, resizing;
    bool fit_resize_buffer;
    bool unconstrained_restore;
    bool interactive_resize;
    bool unclipped_move;
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
    const char *icon_name, *icon_png_base64;
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

void seaside_scene_set_linux_dmabuf(struct seaside_scene_bridge *scene,
                                    struct wlr_linux_dmabuf_v1 *dmabuf);
void seaside_scene_watch_surfaces(struct seaside_scene_bridge *scene,
                                  struct wlr_compositor *compositor);
void seaside_scene_set_titlebars(struct seaside_scene_bridge *scene, bool visible);
void seaside_scene_set_border_colors(struct seaside_scene_bridge *scene, uint32_t active,
                                     uint32_t inactive);
void seaside_scene_set_render_policy(struct seaside_scene_bridge *scene, bool direct_scanout,
                                     bool allow_tearing, bool night_light, int temperature_kelvin);
void seaside_scene_set_blur(struct seaside_scene_bridge *scene, bool enabled, double strength,
                            int passes);
void seaside_scene_set_frame_observer(struct seaside_scene_bridge *scene,
                                      seaside_scene_frame_fn callback, void *data);
void seaside_scene_set_before_frame(struct seaside_scene_bridge *scene,
                                    seaside_scene_before_frame_fn callback, void *data);
void seaside_scene_schedule_frame(struct wlr_output *output);
struct wl_event_loop *seaside_scene_event_loop(const struct seaside_scene_bridge *scene);
struct seaside_scene_render_status
seaside_scene_get_render_status(const struct seaside_scene_bridge *scene,
                                struct wlr_output *output);

struct seaside_scene_bridge *seaside_scene_create(struct wl_display *display);
void seaside_scene_destroy(struct seaside_scene_bridge *scene);
/* The borrowed icon removes its own scene node on destruction. Drag icons
 * are rendered above windows and are excluded from pointer hit-testing. */
void seaside_scene_set_drag_icon(struct seaside_scene_bridge *scene, struct wlr_drag_icon *icon);
void seaside_scene_move_drag_icon(struct seaside_scene_bridge *scene, int x, int y);
void seaside_scene_set_input_popup(struct seaside_scene_bridge *scene,
                                   struct wlr_input_popup_surface_v2 *popup,
                                   struct wlr_surface *focus, const struct wlr_box *cursor);
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
void seaside_scene_set_layer_handler(struct seaside_scene_bridge *scene,
                                     seaside_layer_output_fn focused_output,
                                     seaside_layer_changed_fn changed,
                                     seaside_layer_hidden_fn hidden, void *data);
struct seaside_scene_rect seaside_scene_layer_work_area(struct seaside_scene_bridge *scene,
                                                        struct wlr_output *output,
                                                        struct seaside_scene_rect bounds,
                                                        bool fullscreen);
void seaside_scene_dismiss_unfocused_grabs(struct seaside_scene_bridge *scene,
                                           struct wlr_surface *target);
void seaside_scene_select_layer(struct seaside_scene_bridge *scene, struct wlr_surface *surface);
struct wlr_surface *seaside_scene_layer_keyboard_focus(struct seaside_scene_bridge *scene,
                                                       struct wlr_output *focused_output);
struct wlr_output *seaside_scene_visible_output(const struct seaside_scene_bridge *scene,
                                                struct wlr_surface *surface);
/* Install the selected-window protocols before dispatching client maps. */
bool seaside_scene_enable_window_capture(struct seaside_scene_bridge *scene,
                                         struct wlr_allocator *allocator,
                                         struct wlr_renderer *renderer,
                                         struct wlr_ext_image_copy_capture_manager_v1 *copy);
bool seaside_scene_window_capture_client_ready(const struct seaside_scene_bridge *scene, pid_t pid,
                                               uid_t uid);
void seaside_scene_capture_pointer(struct seaside_scene_bridge *scene, double x, double y);
void seaside_scene_place_window(struct seaside_scene_bridge *scene, uint64_t id,
                                const struct seaside_scene_window_placement *placement);
uint32_t seaside_scene_configure_tiled_window(struct seaside_scene_bridge *scene, uint64_t id,
                                              struct seaside_scene_rect geometry);
uint32_t seaside_scene_window_commit_serial(const struct seaside_scene_bridge *scene, uint64_t id);
struct seaside_scene_rect
seaside_scene_window_content_geometry(const struct seaside_scene_bridge *scene, uint64_t id);
void seaside_scene_set_fit_resize_buffer(struct seaside_scene_bridge *scene, uint64_t id, bool fit);
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

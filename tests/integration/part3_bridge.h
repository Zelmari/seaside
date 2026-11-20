#ifndef SEASIDE_PART3_BRIDGE_H
#define SEASIDE_PART3_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct wlr_backend;
struct wlr_renderer;
struct wlr_allocator;
struct wlr_output;
struct part3_output;

struct part3_output_snapshot {
    unsigned black_frames;
    unsigned client_frames;
    unsigned invalid_frames;
};

#ifdef __cplusplus
extern "C" {
#endif

struct wlr_backend *part3_headless_backend_create(struct wl_display *display);
struct wlr_renderer *part3_pixman_renderer_create(void);
struct part3_output *part3_output_create(struct wl_display *display, struct wlr_backend *backend,
                                         struct wlr_allocator *allocator,
                                         struct wlr_renderer *renderer);
struct wlr_output *part3_output_handle(struct part3_output *output);
struct part3_output_snapshot part3_output_get_snapshot(struct part3_output *output);
void part3_output_destroy(struct part3_output *output);

#ifdef __cplusplus
}
#endif

#endif

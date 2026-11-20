#ifndef SEASIDE_WLROOTS_BRIDGE_H
#define SEASIDE_WLROOTS_BRIDGE_H

#include <stdbool.h>

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;

/* wlroots 0.20 headers use C-only array parameters. Keep them in C translation
 * units rather than rewriting upstream headers or redefining C keywords. */
#ifdef __cplusplus
extern "C" {
#endif

struct wlr_allocator *seaside_wlr_allocator_create(struct wlr_backend *backend,
                                                   struct wlr_renderer *renderer);
bool seaside_wlr_renderer_init_display(struct wlr_renderer *renderer, struct wl_display *display);
struct wlr_compositor *seaside_wlr_compositor_create(struct wl_display *display,
                                                     struct wlr_renderer *renderer);
bool seaside_wlr_backend_start(struct wlr_backend *backend);
void seaside_wlr_backend_destroy(struct wlr_backend *backend);
void seaside_wlr_allocator_destroy(struct wlr_allocator *allocator);
void seaside_wlr_renderer_destroy(struct wlr_renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif

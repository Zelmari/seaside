#include "wlroots_bridge.h"

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

struct wlr_allocator *seaside_wlr_allocator_create(struct wlr_backend *backend,
                                                   struct wlr_renderer *renderer) {
    return wlr_allocator_autocreate(backend, renderer);
}

bool seaside_wlr_renderer_init_display(struct wlr_renderer *renderer, struct wl_display *display) {
    return wlr_renderer_init_wl_display(renderer, display);
}

struct wlr_compositor *seaside_wlr_compositor_create(struct wl_display *display,
                                                     struct wlr_renderer *renderer) {
    /* Both globals are display-owned, including on partial creation failure. */
    struct wlr_compositor *compositor = wlr_compositor_create(display, 6, renderer);
    if (!compositor || !wlr_subcompositor_create(display)) {
        return NULL;
    }
    return compositor;
}

bool seaside_wlr_backend_start(struct wlr_backend *backend) {
    return wlr_backend_start(backend);
}

void seaside_wlr_backend_destroy(struct wlr_backend *backend) {
    wlr_backend_destroy(backend);
}

void seaside_wlr_allocator_destroy(struct wlr_allocator *allocator) {
    wlr_allocator_destroy(allocator);
}

void seaside_wlr_renderer_destroy(struct wlr_renderer *renderer) {
    wlr_renderer_destroy(renderer);
}

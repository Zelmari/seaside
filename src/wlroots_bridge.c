#include "wlroots_bridge.h"

#include <wlr/backend.h>
#include <wlr/config.h>
#if WLR_HAS_GLES2_RENDERER
#include <wlr/render/gles2.h>
#endif
#if WLR_HAS_VULKAN_RENDERER && __has_include(<vulkan/vulkan_core.h>)
#include <wlr/render/vulkan.h>
#endif
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>

struct wlr_allocator *seaside_wlr_allocator_create(struct wlr_backend *backend,
                                                   struct wlr_renderer *renderer) {
    return wlr_allocator_autocreate(backend, renderer);
}

bool seaside_wlr_renderer_init_display(struct wlr_renderer *renderer, struct wl_display *display,
                                       struct wlr_backend *backend,
                                       struct wlr_linux_dmabuf_v1 **dmabuf) {
    if (!wlr_renderer_init_wl_shm(renderer, display) ||
        !wlr_presentation_create(display, backend, 2) ||
        !wlr_fractional_scale_manager_v1_create(display, 1)) {
        return false;
    }
    const int drm_fd = wlr_renderer_get_drm_fd(renderer);
    if (drm_fd >= 0 && wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DMABUF)) {
        *dmabuf = wlr_linux_dmabuf_v1_create_with_renderer(display, 5, renderer);
        if (!*dmabuf) {
            return false;
        }
        if (renderer->features.timeline && backend->features.timeline &&
            !wlr_linux_drm_syncobj_manager_v1_create(display, 1, drm_fd)) {
            return false;
        }
    }
    return true;
}

struct wlr_compositor *seaside_wlr_compositor_create(struct wl_display *display,
                                                     struct wlr_renderer *renderer) {
    /* These globals are display-owned, including on partial creation failure. */
    struct wlr_compositor *compositor = wlr_compositor_create(display, 6, renderer);
    if (!compositor || !wlr_subcompositor_create(display) || !wlr_viewporter_create(display)) {
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

const char *seaside_wlr_renderer_name(struct wlr_renderer *renderer) {
    if (!renderer) {
        return NULL;
    }
    if (wlr_renderer_is_pixman(renderer)) {
        return "pixman";
    }
#if WLR_HAS_GLES2_RENDERER
    if (wlr_renderer_is_gles2(renderer)) {
        return "gles2";
    }
#endif
#if WLR_HAS_VULKAN_RENDERER && __has_include(<vulkan/vulkan_core.h>)
    if (wlr_renderer_is_vk(renderer)) {
        return "vulkan";
    }
#endif
    return NULL;
}

void seaside_wlr_renderer_destroy(struct wlr_renderer *renderer) {
    wlr_renderer_destroy(renderer);
}

#include "wlroots_bridge.h"

#include <stdlib.h>
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

struct surface_sync_tracking {
    struct wlr_addon addon;
    struct wlr_surface *surface;
    struct wl_listener commit;
    bool explicit_buffer;
};

struct compositor_sync_tracking {
    struct wl_listener new_surface, destroy;
};

static void destroy_surface_sync(struct wlr_addon *addon) {
    struct surface_sync_tracking *tracking = wl_container_of(addon, tracking, addon);
    wl_list_remove(&tracking->commit.link);
    wlr_addon_finish(addon);
    free(tracking);
}

static const struct wlr_addon_interface surface_sync_tracking_impl = {
    .name = "seaside-surface-sync-tracking", .destroy = destroy_surface_sync};

static void track_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct surface_sync_tracking *tracking = wl_container_of(listener, tracking, commit);
    struct wlr_surface *surface = tracking->surface;
    if (surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
        const struct wlr_linux_drm_syncobj_surface_v1_state *state =
            wlr_linux_drm_syncobj_v1_get_surface_state(surface);
        tracking->explicit_buffer = surface->buffer && state && state->acquire_timeline;
    }
}

static void track_new_surface(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_surface *surface = data;
    struct surface_sync_tracking *tracking = calloc(1, sizeof(*tracking));
    if (!tracking) {
        wl_resource_post_no_memory(surface->resource);
        return;
    }
    tracking->surface = surface;
    tracking->commit.notify = track_surface_commit;
    wlr_addon_init(&tracking->addon, &surface->addons, &surface_sync_tracking_impl,
                   &surface_sync_tracking_impl);
    wl_signal_add(&surface->events.commit, &tracking->commit);
}

static void destroy_compositor_sync(struct wl_listener *listener, void *data) {
    (void)data;
    struct compositor_sync_tracking *tracking = wl_container_of(listener, tracking, destroy);
    wl_list_remove(&tracking->new_surface.link);
    wl_list_remove(&tracking->destroy.link);
    free(tracking);
}

bool seaside_wlr_surface_waits_for_sync_replacement(struct wlr_surface *surface) {
    struct wlr_addon *addon =
        wlr_addon_find(&surface->addons, &surface_sync_tracking_impl, &surface_sync_tracking_impl);
    if (!addon) {
        return false;
    }
    const struct surface_sync_tracking *tracking = wl_container_of(addon, tracking, addon);
    const struct wlr_linux_drm_syncobj_surface_v1_state *state =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    // wlroots 0.20 drops the state with the protocol object. Keep capture
    // from resampling that buffer until a replacement supplies fresh state.
    return tracking->explicit_buffer && (!state || !state->acquire_timeline);
}

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
    if (renderer && renderer->features.timeline) {
        struct compositor_sync_tracking *tracking = calloc(1, sizeof(*tracking));
        if (!tracking) {
            return NULL;
        }
        tracking->new_surface.notify = track_new_surface;
        tracking->destroy.notify = destroy_compositor_sync;
        wl_signal_add(&compositor->events.new_surface, &tracking->new_surface);
        wl_signal_add(&compositor->events.destroy, &tracking->destroy);
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

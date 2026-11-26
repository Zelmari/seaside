#define _POSIX_C_SOURCE 200809L
#include "window_capture.h"
#include "wlroots_bridge.hpp"

#include <drm.h>
#include <drm_fourcc.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/transform.h>

struct seaside_window_capture {
    struct wl_display *display;
    struct wlr_allocator *allocator;
    struct wlr_renderer *renderer;
    const struct wlr_drm_format *format;
    struct wlr_ext_foreign_toplevel_list_v1 *list;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
    struct wl_list painting;
    struct wl_listener request, new_session;
};

struct seaside_window_source {
    struct seaside_window_capture *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_ext_foreign_toplevel_handle_v1 *handle;
    struct wlr_ext_image_capture_source_v1 source;
    struct wlr_swapchain *swapchain;
    struct wlr_drm_syncobj_timeline *timeline;
    struct wlr_drm_syncobj_timeline_waiter waiter;
    uint64_t render_point;
    struct wlr_buffer *rendered_buffer;
    struct timespec rendered_at;
    bool waiting_for_render;
    struct wl_event_source *timer;
    struct wl_list observers;
    struct wl_list painted_link, painted_sessions;
    struct wl_listener title, app_id;
    seaside_window_cursor_fn cursor;
    void *cursor_data;
    size_t active;
    bool scheduled, pending, damaged, refresh, presented, painting_cursor, cursor_painted;
};

struct painted_session {
    struct seaside_window_source *target;
    struct wlr_ext_image_copy_capture_session_v1 *session;
    struct wl_listener destroy;
    struct wl_list link;
};

struct observed_surface {
    struct seaside_window_source *target;
    struct wlr_surface *surface;
    struct wl_list link;
    struct wl_listener commit, unmap, destroy, new_subsurface;
    struct wl_listener new_popup, xdg_destroy;
    bool visited;
};

struct window_frame {
    struct wlr_ext_image_capture_source_v1_frame_event base;
    struct wlr_buffer *buffer;
    struct wlr_buffer *with_cursor;
    struct timespec when;
};

/* Coalesce independent subsurface commits and pace hidden clients at most
 * once per ordinary refresh interval. The one-shot exists only during use. */
static void schedule(struct seaside_window_source *target) {
    if (target->timer && !target->scheduled) {
        target->scheduled = wl_event_source_timer_update(target->timer, 16) == 0;
    }
}

static void surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, commit);
    // A callback-only commit can still need a wakeup without a new image.
    if (observer->surface->current.committed != WLR_SURFACE_STATE_FRAME_CALLBACK_LIST) {
        observer->target->damaged = true;
    }
    schedule(observer->target);
}

static void surface_tree_changed(struct seaside_window_source *target) {
    target->refresh = true;
    target->damaged = true;
    schedule(target);
}

static void surface_new_subsurface(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, new_subsurface);
    surface_tree_changed(observer->target);
}

static void surface_new_popup(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, new_popup);
    surface_tree_changed(observer->target);
}

static void surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, unmap);
    surface_tree_changed(observer->target);
}

static void remove_observer(struct observed_surface *observer) {
    wl_list_remove(&observer->new_subsurface.link);
    wl_list_remove(&observer->new_popup.link);
    wl_list_remove(&observer->xdg_destroy.link);
    wl_list_remove(&observer->commit.link);
    wl_list_remove(&observer->unmap.link);
    wl_list_remove(&observer->destroy.link);
    wl_list_remove(&observer->link);
    free(observer);
}

static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, destroy);
    surface_tree_changed(observer->target);
    remove_observer(observer);
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct observed_surface *observer = wl_container_of(listener, observer, xdg_destroy);
    surface_tree_changed(observer->target);
    remove_observer(observer);
}

static bool observe_surface(struct seaside_window_source *target, struct wlr_surface *surface,
                            struct wlr_xdg_surface *xdg) {
    struct observed_surface *observer;
    wl_list_for_each(observer, &target->observers, link) {
        if (observer->surface == surface) {
            observer->visited = true;
            return true;
        }
    }
    observer = calloc(1, sizeof(*observer));
    if (!observer) {
        return false;
    }
    observer->target = target;
    observer->surface = surface;
    observer->visited = true;
    observer->commit.notify = surface_commit;
    observer->unmap.notify = surface_unmap;
    observer->destroy.notify = surface_destroy;
    observer->new_subsurface.notify = surface_new_subsurface;
    observer->new_popup.notify = surface_new_popup;
    observer->xdg_destroy.notify = xdg_surface_destroy;
    wl_signal_add(&surface->events.commit, &observer->commit);
    wl_signal_add(&surface->events.unmap, &observer->unmap);
    wl_signal_add(&surface->events.destroy, &observer->destroy);
    wl_signal_add(&surface->events.new_subsurface, &observer->new_subsurface);
    if (xdg) {
        wl_signal_add(&xdg->events.new_popup, &observer->new_popup);
        wl_signal_add(&xdg->events.destroy, &observer->xdg_destroy);
    } else {
        wl_list_init(&observer->new_popup.link);
        wl_list_init(&observer->xdg_destroy.link);
    }
    wl_list_insert(&target->observers, &observer->link);
    return true;
}

static bool observe_tree(struct seaside_window_source *target, struct wlr_surface *surface,
                         struct wlr_xdg_surface *xdg) {
    if (!observe_surface(target, surface, xdg)) {
        return false;
    }
    struct wlr_subsurface *child;
    wl_list_for_each(child, &surface->current.subsurfaces_below, current.link) {
        if (!observe_tree(target, child->surface, NULL)) {
            return false;
        }
    }
    wl_list_for_each(child, &surface->current.subsurfaces_above, current.link) {
        if (!observe_tree(target, child->surface, NULL)) {
            return false;
        }
    }
    if (xdg) {
        struct wlr_xdg_popup *popup;
        wl_list_for_each(popup, &xdg->popups, link) {
            if (!observe_tree(target, popup->base->surface, popup->base)) {
                return false;
            }
        }
    }
    return true;
}

static bool refresh_observers(struct seaside_window_source *target) {
    struct wlr_xdg_surface *xdg = target->toplevel->base;
    if (!observe_tree(target, xdg->surface, xdg)) {
        return false;
    }
    struct observed_surface *observer, *next;
    wl_list_for_each_safe(observer, next, &target->observers, link) {
        if (observer->visited) {
            observer->visited = false;
        } else {
            remove_observer(observer);
        }
    }
    target->refresh = false;
    return true;
}

static bool valid_geometry(const struct wlr_box *box) {
    // Bound resources from client-provided dimensions before allocating.
    return box->width > 0 && box->height > 0 && box->width <= 8192 && box->height <= 8192;
}

static bool update_constraints(struct seaside_window_source *target) {
    const struct wlr_box *box = &target->toplevel->base->geometry;
    if (!valid_geometry(box)) {
        return false;
    }
    struct wlr_surface *root = target->toplevel->base->surface;
    if (root->current.width <= 0 || root->current.height <= 0) {
        return false;
    }
    struct wlr_fbox pixels;
    wlr_surface_get_buffer_source_box(root, &pixels);
    wlr_fbox_transform(&pixels, &pixels, root->current.transform, root->current.buffer_width,
                       root->current.buffer_height);
    // Window geometry is logical. Preserve its aspect ratio and the root
    // buffer's detail, including fractional viewport crops and rotated buffers.
    double density = fmax(pixels.width / root->current.width, pixels.height / root->current.height);
    density = fmin(fmax(1, density), 8192.0 / fmax(box->width, box->height));
    const int width = (int)lround(box->width * density);
    const int height = (int)lround(box->height * density);
    if (target->swapchain && target->swapchain->width == width &&
        target->swapchain->height == height) {
        return true;
    }
    struct wlr_swapchain *next =
        wlr_swapchain_create(target->owner->allocator, width, height, target->owner->format);
    if (!next) {
        return false;
    }
    if (!wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&target->source, next,
                                                                        target->owner->renderer) ||
        (!target->source.shm_formats_len && !target->source.dmabuf_formats.len)) {
        wlr_swapchain_destroy(next);
        return false;
    }
    wlr_swapchain_destroy(target->swapchain);
    target->swapchain = next;
    target->damaged = true;
    return true;
}

struct render_window {
    struct wlr_render_pass *pass;
    struct wlr_box geometry;
    struct seaside_window_source *target;
    double scale_x, scale_y;
    bool failed;
};

static void draw_surface(struct wlr_surface *surface, int x, int y, void *data) {
    struct render_window *render = data;
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    const int width = surface->current.width;
    const int height = surface->current.height;
    // Compare before narrowing positions from client-provided geometry.
    const int64_t left = (int64_t)x - render->geometry.x;
    const int64_t top = (int64_t)y - render->geometry.y;
    const int64_t right = left + width, bottom = top + height;
    const int64_t x1 = left > 0 ? left : 0;
    const int64_t y1 = top > 0 ? top : 0;
    const int64_t x2 = right < render->geometry.width ? right : render->geometry.width;
    const int64_t y2 = bottom < render->geometry.height ? bottom : render->geometry.height;
    if (!texture || x2 <= x1 || y2 <= y1) {
        return;
    }
    // The clipped coordinates fit the bounded window geometry. Round shared
    // edges once so adjacent surfaces meet on the capture grid.
    const int dx = (int)lround((double)x1 * render->scale_x),
              dy = (int)lround((double)y1 * render->scale_y);
    const struct wlr_box destination = {dx, dy, (int)lround((double)x2 * render->scale_x) - dx,
                                        (int)lround((double)y2 * render->scale_y) - dy};
    struct wlr_fbox source;
    wlr_surface_get_buffer_source_box(surface, &source);
    const enum wl_output_transform transform = surface->current.transform;
    if (x1 != left || y1 != top || x2 != right || y2 != bottom) {
        // Crop in transformed surface space, then map that rectangle back to
        // the renderer's buffer coordinates. Destination sizes stay bounded
        // even with a large client viewport crossing a capture edge.
        int buffer_width = surface->current.buffer_width;
        int buffer_height = surface->current.buffer_height;
        wlr_fbox_transform(&source, &source, transform, buffer_width, buffer_height);
        wlr_output_transform_coords(transform, &buffer_width, &buffer_height);
        source.x += (double)(x1 - left) * source.width / width;
        source.y += (double)(y1 - top) * source.height / height;
        source.width *= (double)(x2 - x1) / width;
        source.height *= (double)(y2 - y1) / height;
        wlr_fbox_transform(&source, &source, wlr_output_transform_invert(transform), buffer_width,
                           buffer_height);
    }
    struct wlr_linux_drm_syncobj_surface_v1_state *sync =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    if (sync && sync->acquire_timeline &&
        !wlr_linux_drm_syncobj_v1_state_add_release_point(
            sync, render->target->timeline, render->target->render_point,
            wl_display_get_event_loop(render->target->owner->display))) {
        // Never read a client buffer unless its release tracks this pass.
        render->failed = true;
        return;
    }
    wlr_render_pass_add_texture(render->pass,
                                &(struct wlr_render_texture_options){
                                    .texture = texture,
                                    .src_box = source,
                                    .dst_box = destination,
                                    .transform = wlr_output_transform_invert(transform),
                                    .wait_timeline = sync ? sync->acquire_timeline : NULL,
                                    .wait_point = sync ? sync->acquire_point : 0,
                                });
}

static void callback_surface(struct wlr_surface *surface, int x, int y, void *data) {
    (void)x;
    (void)y;
    wlr_surface_send_frame_done(surface, data);
}

struct capture_sync {
    bool explicit_sync, wants_replacement;
};

static void check_sync_surface(struct wlr_surface *surface, int x, int y, void *data) {
    (void)x;
    (void)y;
    struct capture_sync *tracking = data;
    const struct wlr_linux_drm_syncobj_surface_v1_state *sync =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    tracking->explicit_sync |= sync && sync->acquire_timeline;
    tracking->wants_replacement |= seaside_wlr_surface_waits_for_sync_replacement(surface);
}

static void publish_frame(struct seaside_window_source *target, struct wlr_buffer *buffer,
                          struct timespec when) {
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
    struct window_frame frame = {.base = {.damage = &damage}, .buffer = buffer, .when = when};
    wl_signal_emit_mutable(&target->source.events.frame, &frame.base);
    wlr_buffer_unlock(frame.with_cursor);
    pixman_region32_fini(&damage);
}

static void invalidate_source(struct seaside_window_source *target);

static bool deliver_rendered_buffer(struct seaside_window_source *target) {
    bool ready = false;
    if (!wlr_drm_syncobj_timeline_check(target->timeline, target->render_point,
                                        DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &ready)) {
        invalidate_source(target);
        return false;
    }
    if (!ready) {
        // Poll only if the waiter could not be armed or woke without a signal.
        schedule(target);
        return false;
    }
    struct wlr_buffer *buffer = target->rendered_buffer;
    target->rendered_buffer = NULL;
    publish_frame(target, buffer, target->rendered_at);
    wlr_buffer_unlock(buffer);
    return true;
}

static void capture_render_ready(struct wlr_drm_syncobj_timeline_waiter *waiter) {
    struct seaside_window_source *target = wl_container_of(waiter, target, waiter);
    wlr_drm_syncobj_timeline_waiter_finish(waiter);
    target->waiting_for_render = false;
    if (deliver_rendered_buffer(target) && target->pending) {
        schedule(target);
    }
}

static bool emit_frame(struct seaside_window_source *target, struct timespec when,
                       bool explicit_sync) {
    struct wlr_renderer *renderer = target->owner->renderer;
    if (explicit_sync && !target->timeline) {
        target->timeline = wlr_drm_syncobj_timeline_create(wlr_renderer_get_drm_fd(renderer));
        if (!target->timeline) {
            return false;
        }
    }
    struct wlr_buffer *buffer = wlr_swapchain_acquire(target->swapchain);
    if (!buffer) {
        return false;
    }
    const struct wlr_buffer_pass_options options = {
        .signal_timeline = explicit_sync ? target->timeline : NULL,
        .signal_point = explicit_sync ? ++target->render_point : 0,
    };
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, &options);
    bool rendered = false;
    if (pass) {
        const struct wlr_box geometry = target->toplevel->base->geometry;
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {0, 0, buffer->width, buffer->height},
                                           .color = {0, 0, 0, 1},
                                           .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                       });
        struct render_window render = {
            .pass = pass,
            .geometry = geometry,
            .target = target,
            .scale_x = (double)buffer->width / geometry.width,
            .scale_y = (double)buffer->height / geometry.height,
        };
        wlr_xdg_surface_for_each_surface(target->toplevel->base, draw_surface, &render);
        rendered = wlr_render_pass_submit(pass) && !render.failed;
        if (rendered && explicit_sync) {
            target->rendered_buffer = buffer;
            target->rendered_at = when;
            target->waiting_for_render = wlr_drm_syncobj_timeline_waiter_init(
                &target->waiter, target->timeline, target->render_point, 0,
                wl_display_get_event_loop(target->owner->display), capture_render_ready);
            if (!target->waiting_for_render) {
                schedule(target);
            }
            return true;
        }
        if (rendered) {
            publish_frame(target, buffer, when);
        }
    }
    wlr_buffer_unlock(buffer);
    return rendered;
}

static const struct wlr_ext_image_capture_source_v1_interface window_source_impl;

static void invalidate_source(struct seaside_window_source *target) {
    wlr_ext_image_capture_source_v1_finish(&target->source);
    wlr_ext_image_capture_source_v1_init(&target->source, &window_source_impl);
}

static int capture_tick(void *data) {
    struct seaside_window_source *target = data;
    target->scheduled = false;
    if (target->rendered_buffer &&
        (target->waiting_for_render || !deliver_rendered_buffer(target))) {
        return 0;
    }
    struct timespec now;
    const bool requested = target->pending;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        (target->refresh && !refresh_observers(target)) || !update_constraints(target)) {
        // Keep the toplevel selectable if rendering can recover later. The
        // old source and its sessions cannot safely continue with bad buffers.
        invalidate_source(target);
        return 0;
    }
    if (target->pending && target->damaged) {
        struct capture_sync sync = {0};
        if (target->owner->renderer->features.timeline) {
            wlr_xdg_surface_for_each_surface(target->toplevel->base, check_sync_surface, &sync);
        }
        if (!sync.wants_replacement) {
            if (!emit_frame(target, now, sync.explicit_sync)) {
                invalidate_source(target);
                return 0;
            }
            target->pending = false;
            target->damaged = false;
        }
    }
    // An occluded view can still be on the active workspace. Give the
    // requested capture its own pacing even if the output skipped that view.
    // The one-shot callbacks are removed on delivery, so the output cannot
    // deliver the same callback a second time.
    if (requested) {
        wlr_xdg_surface_for_each_surface(target->toplevel->base, callback_surface, &now);
    }
    return 0;
}

static void start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors) {
    struct seaside_window_source *target = wl_container_of(source, target, source);
    target->painting_cursor = with_cursors;
    if (target->active++ == 0) {
        // wlroots sends the first buffer constraints immediately after start.
        // Defer stopping the source on failure until it attaches the session.
        (void)update_constraints(target);
        target->damaged = true;
        target->refresh = true;
        schedule(target);
    }
}

static void stop(struct wlr_ext_image_capture_source_v1 *source) {
    struct seaside_window_source *target = wl_container_of(source, target, source);
    if (--target->active == 0) {
        wl_event_source_timer_update(target->timer, 0);
        target->scheduled = false;
        target->pending = false;
        if (target->waiting_for_render) {
            wlr_drm_syncobj_timeline_waiter_finish(&target->waiter);
            target->waiting_for_render = false;
        }
        wlr_buffer_unlock(target->rendered_buffer);
        target->rendered_buffer = NULL;
        wlr_drm_syncobj_timeline_unref(target->timeline);
        target->timeline = NULL;
        target->render_point = 0;
        struct observed_surface *observer, *next;
        wl_list_for_each_safe(observer, next, &target->observers, link) {
            remove_observer(observer);
        }
        wlr_swapchain_destroy(target->swapchain);
        target->swapchain = NULL;
    }
}

static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool needs_frame) {
    struct seaside_window_source *target = wl_container_of(source, target, source);
    target->pending = true;
    target->damaged |= needs_frame;
    schedule(target);
}

static bool paint_cursor(struct seaside_window_source *target, struct window_frame *frame) {
    if (frame->with_cursor) {
        return true;
    }
    const struct wlr_box geometry = target->toplevel->base->geometry;
    if (geometry.width <= 0 || geometry.height <= 0) {
        return false;
    }
    struct wlr_render_texture_options cursor;
    target->cursor_painted =
        target->presented &&
        target->cursor(target->cursor_data, (double)frame->buffer->width / geometry.width,
                       (double)frame->buffer->height / geometry.height, &cursor);
    if (!target->cursor_painted) {
        return true;
    }
    struct wlr_buffer *buffer = wlr_swapchain_acquire(target->swapchain);
    if (!buffer) {
        return false;
    }
    struct wlr_renderer *renderer = target->owner->renderer;
    struct wlr_texture *content = wlr_texture_from_buffer(renderer, frame->buffer);
    struct wlr_render_pass *pass =
        content ? wlr_renderer_begin_buffer_pass(renderer, buffer, NULL) : NULL;
    if (pass) {
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                                              .texture = content,
                                              .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                          });
        wlr_render_pass_add_texture(pass, &cursor);
        if (wlr_render_pass_submit(pass)) {
            frame->with_cursor = buffer;
        }
    }
    wlr_texture_destroy(content);
    if (!frame->with_cursor) {
        wlr_buffer_unlock(buffer);
        return false;
    }
    return true;
}

static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                       struct wlr_ext_image_copy_capture_frame_v1 *frame,
                       struct wlr_ext_image_capture_source_v1_frame_event *data) {
    struct seaside_window_source *target = wl_container_of(source, target, source);
    struct window_frame *event = wl_container_of(data, event, base);
    struct wlr_buffer *buffer = event->buffer;
    struct painted_session *painting;
    wl_list_for_each(painting, &target->painted_sessions, link) {
        if (painting->session->frame == frame) {
            if (!paint_cursor(target, event)) {
                wlr_ext_image_copy_capture_frame_v1_fail(
                    frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
                return;
            }
            if (event->with_cursor) {
                buffer = event->with_cursor;
            }
            break;
        }
    }
    if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, buffer, target->owner->renderer)) {
        wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &event->when);
    }
}

static const struct wlr_ext_image_capture_source_v1_interface window_source_impl = {
    .start = start,
    .stop = stop,
    .request_frame = request_frame,
    .copy_frame = copy_frame,
};

static void painted_session_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct painted_session *painting = wl_container_of(listener, painting, destroy);
    struct seaside_window_source *target = painting->target;
    wl_list_remove(&painting->destroy.link);
    wl_list_remove(&painting->link);
    if (wl_list_empty(&target->painted_sessions)) {
        wl_list_remove(&target->painted_link);
        target->cursor_painted = false;
    }
    free(painting);
}

static void new_session(struct wl_listener *listener, void *data) {
    struct seaside_window_capture *capture = wl_container_of(listener, capture, new_session);
    struct wlr_ext_image_copy_capture_session_v1 *session = data;
    if (session->source->impl != &window_source_impl) {
        return;
    }
    struct seaside_window_source *target = wl_container_of(session->source, target, source);
    if (!target->painting_cursor) {
        return;
    }
    struct painted_session *painting = calloc(1, sizeof(*painting));
    if (!painting) {
        wl_client_post_no_memory(wl_resource_get_client(session->resource));
        return;
    }
    painting->target = target;
    painting->session = session;
    painting->destroy.notify = painted_session_destroy;
    wl_signal_add(&session->events.destroy, &painting->destroy);
    if (wl_list_empty(&target->painted_sessions)) {
        wl_list_insert(&capture->painting, &target->painted_link);
    }
    wl_list_insert(&target->painted_sessions, &painting->link);
}

static void source_request(struct wl_listener *listener, void *data) {
    struct seaside_window_capture *capture = wl_container_of(listener, capture, request);
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
    struct seaside_window_source *target = request->toplevel_handle->data;
    struct wlr_ext_image_capture_source_v1 *source = NULL;
    uid_t uid;
    wl_client_get_credentials(request->client, NULL, &uid, NULL);
    if (uid == geteuid() && request->toplevel_handle->list == capture->list && target &&
        valid_geometry(&target->toplevel->base->geometry)) {
        source = &target->source;
    }
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request, source);
}

struct seaside_window_capture *
seaside_window_capture_create(struct wl_display *display, struct wlr_allocator *allocator,
                              struct wlr_renderer *renderer,
                              struct wlr_ext_image_copy_capture_manager_v1 *copy) {
    if (!display || !allocator || !renderer || !copy) {
        return NULL;
    }
    const struct wlr_drm_format_set *formats =
        wlr_renderer_get_texture_formats(renderer, allocator->buffer_caps);
    const struct wlr_drm_format *format =
        formats ? wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888) : NULL;
    if (!format) {
        return NULL;
    }
    struct seaside_window_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        return NULL;
    }
    capture->display = display;
    capture->allocator = allocator;
    capture->renderer = renderer;
    capture->format = format;
    capture->list = wlr_ext_foreign_toplevel_list_v1_create(display, 1);
    capture->sources = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(display, 1);
    if (!capture->list || !capture->sources) {
        free(capture);
        return NULL;
    }
    capture->request.notify = source_request;
    capture->new_session.notify = new_session;
    wl_signal_add(&capture->sources->events.new_request, &capture->request);
    wl_signal_add(&copy->events.new_session, &capture->new_session);
    wl_list_init(&capture->painting);
    return capture;
}

void seaside_window_capture_destroy(struct seaside_window_capture *capture) {
    if (capture) {
        wl_list_remove(&capture->request.link);
        wl_list_remove(&capture->new_session.link);
        free(capture);
    }
}

enum { FOREIGN_METADATA_MAX = 512 };

struct foreign_metadata {
    char title[FOREIGN_METADATA_MAX + 1];
    char app_id[FOREIGN_METADATA_MAX + 1];
};

static void sanitize_metadata(char *out, const char *input) {
    size_t used = 0;
    // xdpw's chooser uses one source per line. Keep client metadata printable
    // and valid UTF-8, with enough space for the compositor-owned identifier.
    for (const char *cursor = input; cursor && *cursor && used < FOREIGN_METADATA_MAX;) {
        gunichar value = g_utf8_get_char_validated(cursor, -1);
        if (value == (gunichar)-1 || value == (gunichar)-2) {
            value = 0xfffd;
            ++cursor;
        } else {
            cursor = g_utf8_next_char(cursor);
        }
        const GUnicodeType type = g_unichar_type(value);
        if (!g_unichar_isprint(value) || type == G_UNICODE_LINE_SEPARATOR ||
            type == G_UNICODE_PARAGRAPH_SEPARATOR || type == G_UNICODE_FORMAT) {
            value = ' ';
        }
        char encoded[6];
        const size_t size = (size_t)g_unichar_to_utf8(value, encoded);
        if (size > FOREIGN_METADATA_MAX - used) {
            break;
        }
        memcpy(out + used, encoded, size);
        used += size;
    }
    out[used] = '\0';
}

static struct wlr_ext_foreign_toplevel_handle_v1_state
foreign_state(const struct wlr_xdg_toplevel *toplevel, struct foreign_metadata *metadata) {
    sanitize_metadata(metadata->title, toplevel->title);
    sanitize_metadata(metadata->app_id, toplevel->app_id);
    return (struct wlr_ext_foreign_toplevel_handle_v1_state){
        .title = metadata->title,
        .app_id = metadata->app_id,
    };
}

static void update_metadata(struct seaside_window_source *target) {
    struct foreign_metadata metadata;
    struct wlr_ext_foreign_toplevel_handle_v1_state state =
        foreign_state(target->toplevel, &metadata);
    wlr_ext_foreign_toplevel_handle_v1_update_state(target->handle, &state);
}

static void title_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_window_source *target = wl_container_of(listener, target, title);
    update_metadata(target);
}

static void app_id_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_window_source *target = wl_container_of(listener, target, app_id);
    update_metadata(target);
}

struct seaside_window_source *seaside_window_capture_map(struct seaside_window_capture *capture,
                                                         struct wlr_xdg_toplevel *toplevel,
                                                         bool presented,
                                                         seaside_window_cursor_fn cursor,
                                                         void *cursor_data) {
    struct seaside_window_source *target = calloc(1, sizeof(*target));
    if (!target) {
        return NULL;
    }
    target->owner = capture;
    target->toplevel = toplevel;
    target->presented = presented;
    target->cursor = cursor;
    target->cursor_data = cursor_data;
    target->timer =
        wl_event_loop_add_timer(wl_display_get_event_loop(capture->display), capture_tick, target);
    struct foreign_metadata metadata;
    struct wlr_ext_foreign_toplevel_handle_v1_state state = foreign_state(toplevel, &metadata);
    target->handle =
        target->timer ? wlr_ext_foreign_toplevel_handle_v1_create(capture->list, &state) : NULL;
    if (!target->handle) {
        if (target->timer) {
            wl_event_source_remove(target->timer);
        }
        free(target);
        return NULL;
    }
    target->handle->data = target;
    wl_list_init(&target->observers);
    wl_list_init(&target->painted_sessions);
    wlr_ext_image_capture_source_v1_init(&target->source, &window_source_impl);
    target->title.notify = title_changed;
    target->app_id.notify = app_id_changed;
    wl_signal_add(&toplevel->events.set_title, &target->title);
    wl_signal_add(&toplevel->events.set_app_id, &target->app_id);
    return target;
}

void seaside_window_capture_unmap(struct seaside_window_source *target) {
    if (target) {
        wlr_ext_image_capture_source_v1_finish(&target->source);
        wlr_swapchain_destroy(target->swapchain);
        wlr_ext_foreign_toplevel_handle_v1_destroy(target->handle);
        wl_list_remove(&target->title.link);
        wl_list_remove(&target->app_id.link);
        wl_event_source_remove(target->timer);
        free(target);
    }
}

void seaside_window_capture_presented(struct seaside_window_source *target, bool presented) {
    if (target && target->presented != presented) {
        target->presented = presented;
        target->damaged |= target->cursor_painted;
        if (target->active && !presented) {
            schedule(target);
        }
    }
}

void seaside_window_capture_cursor_changed(struct seaside_window_capture *capture) {
    if (capture) {
        struct seaside_window_source *target;
        wl_list_for_each(target, &capture->painting, painted_link) {
            struct wlr_render_texture_options cursor;
            if (target->cursor_painted ||
                (target->presented && target->cursor(target->cursor_data, 1.0, 1.0, &cursor))) {
                target->damaged = true;
                schedule(target);
            }
        }
    }
}

struct ready_bindings {
    const struct seaside_window_capture *capture;
    bool list, source;
};

static enum wl_iterator_result check_binding(struct wl_resource *resource, void *data) {
    struct ready_bindings *bindings = data;
    const char *name = wl_resource_get_class(resource);
    if (strcmp(name, "ext_foreign_toplevel_list_v1") == 0) {
        bindings->list |= wl_resource_get_user_data(resource) == bindings->capture->list;
    } else if (strcmp(name, "ext_foreign_toplevel_image_capture_source_manager_v1") == 0) {
        bindings->source |= wl_resource_get_user_data(resource) == bindings->capture->sources;
    }
    return bindings->list && bindings->source ? WL_ITERATOR_STOP : WL_ITERATOR_CONTINUE;
}

bool seaside_window_capture_client_ready(const struct seaside_window_capture *capture, pid_t pid,
                                         uid_t uid) {
    if (!capture || pid <= 0 || uid != geteuid()) {
        return false;
    }
    struct wl_client *client;
    wl_client_for_each(client, wl_display_get_client_list(capture->display)) {
        pid_t peer_pid;
        uid_t peer_uid;
        wl_client_get_credentials(client, &peer_pid, &peer_uid, NULL);
        if (peer_pid == pid && peer_uid == uid) {
            struct ready_bindings bindings = {.capture = capture};
            wl_client_for_each_resource(client, check_binding, &bindings);
            if (bindings.list && bindings.source) {
                return true;
            }
        }
    }
    return false;
}

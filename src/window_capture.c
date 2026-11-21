#define _POSIX_C_SOURCE 200809L
#include "window_capture.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>

struct seaside_window_capture {
    struct wl_display *display;
    struct wlr_allocator *allocator;
    struct wlr_renderer *renderer;
    const struct wlr_drm_format *format;
    struct wlr_ext_foreign_toplevel_list_v1 *list;
    struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
    struct wl_listener request;
};

struct seaside_window_source {
    struct seaside_window_capture *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_ext_foreign_toplevel_handle_v1 *handle;
    struct wlr_ext_image_capture_source_v1 source;
    struct wlr_swapchain *swapchain;
    struct wl_event_source *timer;
    struct wl_list observers;
    struct wl_listener title, app_id;
    size_t active;
    bool scheduled, pending, damaged, refresh, presented;
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
    if (target->swapchain && target->swapchain->width == box->width &&
        target->swapchain->height == box->height) {
        return true;
    }
    struct wlr_swapchain *next = wlr_swapchain_create(target->owner->allocator, box->width,
                                                      box->height, target->owner->format);
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
};

static void draw_surface(struct wlr_surface *surface, int x, int y, void *data) {
    struct render_window *render = data;
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    const int width = surface->current.width;
    const int height = surface->current.height;
    // Compare before narrowing positions from client-provided geometry.
    const int64_t left = (int64_t)x - render->geometry.x;
    const int64_t top = (int64_t)y - render->geometry.y;
    if (!texture || width <= 0 || height <= 0 || left >= render->geometry.width ||
        top >= render->geometry.height || left + width <= 0 || top + height <= 0) {
        return;
    }
    struct wlr_fbox source;
    wlr_surface_get_buffer_source_box(surface, &source);
    wlr_render_pass_add_texture(render->pass, &(struct wlr_render_texture_options){
                                                  .texture = texture,
                                                  .src_box = source,
                                                  .dst_box = {(int)left, (int)top, width, height},
                                                  .transform = surface->current.transform,
                                              });
}

static void callback_surface(struct wlr_surface *surface, int x, int y, void *data) {
    (void)x;
    (void)y;
    wlr_surface_send_frame_done(surface, data);
}

static bool emit_frame(struct seaside_window_source *target, struct timespec when) {
    struct wlr_buffer *buffer = wlr_swapchain_acquire(target->swapchain);
    if (!buffer) {
        return false;
    }
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(target->owner->renderer, buffer, NULL);
    bool rendered = false;
    if (pass) {
        const struct wlr_box geometry = target->toplevel->base->geometry;
        pixman_region32_t full_damage;
        pixman_region32_init_rect(&full_damage, 0, 0, buffer->width, buffer->height);
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {0, 0, buffer->width, buffer->height},
                                           .color = {0, 0, 0, 1},
                                           .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                       });
        struct render_window render = {.pass = pass, .geometry = geometry};
        wlr_xdg_surface_for_each_surface(target->toplevel->base, draw_surface, &render);
        rendered = wlr_render_pass_submit(pass);
        if (rendered) {
            struct window_frame frame = {
                .base = {.damage = &full_damage}, .buffer = buffer, .when = when};
            wl_signal_emit_mutable(&target->source.events.frame, &frame.base);
        }
        pixman_region32_fini(&full_damage);
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
    struct timespec now;
    const bool requested = target->pending;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        (target->refresh && !refresh_observers(target)) || !update_constraints(target) ||
        (target->pending && target->damaged && !emit_frame(target, now))) {
        // Keep the toplevel selectable if rendering can recover later. The
        // old source and its sessions cannot safely continue with bad buffers.
        invalidate_source(target);
        return 0;
    }
    if (target->pending && target->damaged) {
        target->pending = false;
        target->damaged = false;
    }
    if (requested && !target->presented) {
        wlr_xdg_surface_for_each_surface(target->toplevel->base, callback_surface, &now);
    }
    return 0;
}

static void start(struct wlr_ext_image_capture_source_v1 *source, bool with_cursors) {
    (void)with_cursors;
    struct seaside_window_source *target = wl_container_of(source, target, source);
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

static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                       struct wlr_ext_image_copy_capture_frame_v1 *frame,
                       struct wlr_ext_image_capture_source_v1_frame_event *data) {
    struct seaside_window_source *target = wl_container_of(source, target, source);
    const struct window_frame *event = wl_container_of(data, event, base);
    if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(frame, event->buffer,
                                                        target->owner->renderer)) {
        wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &event->when);
    }
}

static const struct wlr_ext_image_capture_source_v1_interface window_source_impl = {
    .start = start,
    .stop = stop,
    .request_frame = request_frame,
    .copy_frame = copy_frame,
};

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

struct seaside_window_capture *seaside_window_capture_create(struct wl_display *display,
                                                             struct wlr_allocator *allocator,
                                                             struct wlr_renderer *renderer) {
    if (!display || !allocator || !renderer) {
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
    wl_signal_add(&capture->sources->events.new_request, &capture->request);
    return capture;
}

void seaside_window_capture_destroy(struct seaside_window_capture *capture) {
    if (capture) {
        wl_list_remove(&capture->request.link);
        free(capture);
    }
}

static struct wlr_ext_foreign_toplevel_handle_v1_state
foreign_state(const struct wlr_xdg_toplevel *toplevel) {
    return (struct wlr_ext_foreign_toplevel_handle_v1_state){
        .title = toplevel->title ? toplevel->title : "",
        .app_id = toplevel->app_id ? toplevel->app_id : "",
    };
}

static void title_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_window_source *target = wl_container_of(listener, target, title);
    struct wlr_ext_foreign_toplevel_handle_v1_state state = foreign_state(target->toplevel);
    wlr_ext_foreign_toplevel_handle_v1_update_state(target->handle, &state);
}

static void app_id_changed(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_window_source *target = wl_container_of(listener, target, app_id);
    struct wlr_ext_foreign_toplevel_handle_v1_state state = foreign_state(target->toplevel);
    wlr_ext_foreign_toplevel_handle_v1_update_state(target->handle, &state);
}

struct seaside_window_source *seaside_window_capture_map(struct seaside_window_capture *capture,
                                                         struct wlr_xdg_toplevel *toplevel,
                                                         bool presented) {
    struct seaside_window_source *target = calloc(1, sizeof(*target));
    if (!target) {
        return NULL;
    }
    target->owner = capture;
    target->toplevel = toplevel;
    target->presented = presented;
    target->timer =
        wl_event_loop_add_timer(wl_display_get_event_loop(capture->display), capture_tick, target);
    struct wlr_ext_foreign_toplevel_handle_v1_state state = foreign_state(toplevel);
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
        if (target->active && !presented) {
            schedule(target);
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

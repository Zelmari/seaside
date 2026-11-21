#define _POSIX_C_SOURCE 200809L
#include "scene_bridge.h"
#include "window_capture.h"

#include <drm_fourcc.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>

struct seaside_scene_bridge {
    struct wl_display *display;
    struct wlr_scene *scene;
    struct wlr_scene_tree *backgrounds, *windows, *drag_icons, *decorations;
    struct wlr_buffer *wallpaper;
    struct wlr_xdg_shell *shell;
    struct wlr_xdg_activation_v1 *activation;
    struct seaside_window_capture *capture;
    struct wl_list views;
    struct wl_list popups;
    struct wl_list outputs;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_listener shell_destroy;
    struct wl_listener activation_destroy, request_activation;
    struct seaside_scene_snapshot snapshot;
    seaside_scene_window_fn window;
    void *window_data;
    seaside_scene_output_appearance_fn appearance;
    void *appearance_data;
    struct scene_view *focused_view;
    pid_t watched_pid;
    seaside_scene_focus_fn focus;
    void *focus_data;
    bool attached;
};

struct scene_view {
    struct seaside_scene_bridge *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct seaside_window_source *capture;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *xdg_tree;
    struct wlr_scene_rect *fullscreen_background;
    struct wlr_scene_rect *borders[4];
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener set_parent;
    struct seaside_scene_window_placement placement;
    uint64_t id;
    uint32_t restore_serial;
    bool mapped;
};

struct scene_popup {
    struct scene_view *view;
    struct wlr_xdg_popup *popup;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *surface_tree;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener destroy;
    struct wl_listener tree_destroy;
};

struct scene_output {
    struct seaside_scene_bridge *owner;
    struct wlr_scene_output *viewport;
    struct wlr_scene_buffer *wallpaper, *banner;
    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener destroy;
    struct wl_listener commit;
    int width, height;
    int banner_width, banner_height, banner_logical_height;
    float scale;
};

struct scene_image {
    struct wlr_buffer base;
    const uint8_t *pixels;
    seaside_scene_image_release_fn release;
    void *owner;
};

static void destroy_scene_image(struct wlr_buffer *base) {
    struct scene_image *image = wl_container_of(base, image, base);
    wlr_buffer_finish(base);
    image->release(image->owner);
    free(image);
}

static bool scene_image_data(struct wlr_buffer *base, uint32_t flags, void **data, uint32_t *format,
                             size_t *stride) {
    struct scene_image *image = wl_container_of(base, image, base);
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    *data = (void *)image->pixels;
    *format = DRM_FORMAT_XRGB8888;
    *stride = (size_t)base->width * 4;
    return true;
}

static void end_scene_image_data(struct wlr_buffer *base) {
    (void)base;
}

static const struct wlr_buffer_impl scene_image_impl = {
    .destroy = destroy_scene_image,
    .begin_data_ptr_access = scene_image_data,
    .end_data_ptr_access = end_scene_image_data,
};

struct wlr_buffer *seaside_scene_create_image(int width, int height, const uint8_t *pixels,
                                              seaside_scene_image_release_fn release, void *owner) {
    struct scene_image *image = calloc(1, sizeof(*image));
    if (!image) {
        return NULL;
    }
    image->pixels = pixels;
    image->release = release;
    image->owner = owner;
    wlr_buffer_init(&image->base, &scene_image_impl, width, height);
    return &image->base;
}

void seaside_scene_drop_image(struct wlr_buffer *buffer) {
    if (buffer) {
        wlr_buffer_drop(buffer);
    }
}

static struct scene_view *find_view(struct seaside_scene_bridge *scene, uint64_t id) {
    if (!id) {
        return NULL;
    }
    struct scene_view *view;
    wl_list_for_each(view, &scene->views, link) {
        if (view->id == id) {
            return view;
        }
    }
    return NULL;
}

static struct scene_view *toplevel_view(struct seaside_scene_bridge *scene,
                                        const struct wlr_xdg_toplevel *toplevel) {
    struct scene_view *view;
    wl_list_for_each(view, &scene->views, link) {
        if (view->toplevel == toplevel) {
            return view;
        }
    }
    return NULL;
}

static struct scene_popup *surface_popup(struct seaside_scene_bridge *scene,
                                         const struct wlr_surface *surface) {
    struct scene_popup *popup;
    wl_list_for_each(popup, &scene->popups, link) {
        if (popup->popup->base->surface == surface) {
            return popup;
        }
    }
    return NULL;
}

static int clamp_coordinate(int64_t value) {
    return (int)(value < INT_MIN ? INT_MIN : value > INT_MAX ? INT_MAX : value);
}

/* Clip in layout coordinates first so very distant scrolling columns do not
 * create oversized surface-local coordinates or off-output scene rectangles. */
static struct wlr_box intersection(struct seaside_scene_rect clip, int64_t x, int64_t y,
                                   int64_t width, int64_t height) {
    int64_t right = x + width, bottom = y + height;
    const int64_t clip_right = (int64_t)clip.x + clip.width;
    const int64_t clip_bottom = (int64_t)clip.y + clip.height;
    x = x > clip.x ? x : clip.x;
    y = y > clip.y ? y : clip.y;
    right = right < clip_right ? right : clip_right;
    bottom = bottom < clip_bottom ? bottom : clip_bottom;
    if (right <= x || bottom <= y) {
        return (struct wlr_box){0};
    }
    return (struct wlr_box){clamp_coordinate(x), clamp_coordinate(y), (int)(right - x),
                            (int)(bottom - y)};
}

static bool has_viewport(struct seaside_scene_rect rect) {
    return rect.width > 0 && rect.height > 0;
}

static void clip_toplevel(struct scene_view *view) {
    const struct seaside_scene_window_placement *placement = &view->placement;
    const struct seaside_scene_rect viewport = placement->clip;
    if (!has_viewport(viewport)) {
        wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node, NULL);
        wlr_scene_node_set_enabled(&view->xdg_tree->node, !placement->tiled);
        return;
    }
    const struct seaside_scene_rect box = placement->geometry;
    struct wlr_box clip =
        placement->tiled
            ? intersection(viewport, box.x, box.y, box.width, box.height)
            : (struct wlr_box){viewport.x, viewport.y, viewport.width, viewport.height};
    if (clip.width <= 0 || clip.height <= 0) {
        wlr_scene_node_set_enabled(&view->xdg_tree->node, false);
        return;
    }
    const struct wlr_box geometry = view->toplevel->base->geometry;
    clip.x = clamp_coordinate((int64_t)clip.x - box.x + geometry.x);
    clip.y = clamp_coordinate((int64_t)clip.y - box.y + geometry.y);
    wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node, &clip);
    wlr_scene_node_set_enabled(&view->xdg_tree->node, true);
}

static uint64_t emit_window(struct scene_view *view, enum seaside_scene_window_event_kind kind,
                            bool enabled, bool ordinary_commit) {
    struct seaside_scene_bridge *scene = view->owner;
    if (!scene->window || (!view->id && kind != SEASIDE_WINDOW_CREATED)) {
        return 0;
    }
    struct scene_view *parent = toplevel_view(scene, view->toplevel->parent);
    struct wlr_box box = view->toplevel->base->geometry;
    const struct seaside_scene_window_event event = {
        .kind = kind,
        .id = view->id,
        .parent = parent ? parent->id : 0,
        .geometry = {box.x, box.y, box.width, box.height},
        .enabled = enabled,
        .ordinary_commit = ordinary_commit,
    };
    return scene->window(scene->window_data, &event);
}

static void end_focus(struct seaside_scene_bridge *scene, bool deactivate) {
    struct scene_view *view = scene->focused_view;
    if (!view) {
        return;
    }
    scene->focused_view = NULL;
    if (deactivate && view->toplevel->base->initialized) {
        wlr_xdg_toplevel_set_activated(view->toplevel, false);
    }
    scene->focus(scene->focus_data, NULL);
}

static void consider_focus(struct scene_view *view) {
    struct seaside_scene_bridge *scene = view->owner;
    if (scene->watched_pid == 0 || scene->focused_view) {
        return;
    }
    pid_t pid = 0;
    wl_client_get_credentials(wl_resource_get_client(view->toplevel->resource), &pid, NULL, NULL);
    if (pid != scene->watched_pid) {
        return;
    }
    scene->focused_view = view;
    wlr_xdg_toplevel_set_activated(view->toplevel, true);
    scene->focus(scene->focus_data, view->toplevel->base->surface);
}

static void view_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, map);
    if (!view->mapped) {
        view->mapped = true;
        view->owner->snapshot.mapped_toplevels++;
        view->owner->snapshot.map_events++;
    }
    if (view->id) {
        emit_window(view, SEASIDE_WINDOW_MAPPED, false, false);
    } else {
        wlr_scene_node_set_enabled(&view->tree->node, true);
        consider_focus(view);
    }
    if (view->owner->capture && !view->capture) {
        view->capture = seaside_window_capture_map(
            view->owner->capture, view->toplevel,
            view->id ? view->placement.visible && has_viewport(view->placement.clip)
                     : !wl_list_empty(&view->owner->outputs));
    }
}

static void view_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, unmap);
    seaside_window_capture_unmap(view->capture);
    view->capture = NULL;
    if (view->mapped) {
        view->mapped = false;
        view->owner->snapshot.mapped_toplevels--;
        view->owner->snapshot.unmap_events++;
    }
    wlr_scene_node_set_enabled(&view->tree->node, false);
    view->placement = (struct seaside_scene_window_placement){0};
    view->restore_serial = 0;
    emit_window(view, SEASIDE_WINDOW_UNMAPPED, false, false);
    if (view->owner->focused_view == view) {
        // Unmap resets the XDG configure sequence. The next initial commit
        // will establish inactive state before another map can take focus.
        end_focus(view->owner, false);
    }
}

static void view_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, commit);
    if (view->toplevel->base->initial_commit) {
        uint32_t capabilities = view->id ? WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                               WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
                                         : 0;
        wlr_xdg_toplevel_set_wm_capabilities(view->toplevel, capabilities);
        wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        wlr_xdg_toplevel_set_activated(view->toplevel, false);
        emit_window(view, SEASIDE_WINDOW_INITIAL_COMMIT, false, false);
        view->owner->snapshot.initial_configures++;
    }
    if (wlr_surface_has_buffer(view->toplevel->base->surface)) {
        view->owner->snapshot.buffer_commits++;
        /* The client's last tiled or special size may arrive after a request
         * to restore its ordinary floating geometry. */
        uint32_t serial = view->toplevel->base->current.configure_serial;
        bool ordinary =
            !view->toplevel->current.maximized && !view->toplevel->current.fullscreen &&
            !view->toplevel->current.tiled &&
            (!view->restore_serial ||
             (serial && (uint32_t)(serial - view->restore_serial) < UINT32_C(0x80000000)));
        if (ordinary) {
            view->restore_serial = 0;
        }
        clip_toplevel(view);
        emit_window(view, SEASIDE_WINDOW_COMMITTED, false, ordinary);
    }
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_maximize);
    emit_window(view, SEASIDE_WINDOW_MAXIMIZE_REQUESTED, view->toplevel->requested.maximized,
                false);
    /* XDG requires a configure even when the requested state stays the same. */
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_fullscreen);
    emit_window(view, SEASIDE_WINDOW_FULLSCREEN_REQUESTED, view->toplevel->requested.fullscreen,
                false);
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void view_set_parent(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, set_parent);
    emit_window(view, SEASIDE_WINDOW_PARENT_CHANGED, false, false);
}

static void destroy_view(struct scene_view *view) {
    seaside_window_capture_unmap(view->capture);
    if (view->owner->focused_view == view) {
        end_focus(view->owner, false);
    }
    if (view->mapped) {
        view->owner->snapshot.mapped_toplevels--;
    }
    view->owner->snapshot.live_toplevels--;
    view->owner->snapshot.destroyed_toplevels++;
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->set_parent.link);
    wl_list_remove(&view->link);
    wlr_scene_node_set_enabled(&view->tree->node, false);
    emit_window(view, SEASIDE_WINDOW_DESTROYED, false, false);
    wlr_scene_node_destroy(&view->tree->node);
    free(view);
}

static void view_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, destroy);
    destroy_view(view);
}

static void new_toplevel(struct wl_listener *listener, void *data) {
    struct seaside_scene_bridge *scene = wl_container_of(listener, scene, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    struct scene_view *view = calloc(1, sizeof(*view));
    if (!view) {
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
        return;
    }
    view->owner = scene;
    view->toplevel = toplevel;
    view->tree = wlr_scene_tree_create(scene->windows);
    if (view->tree) {
        view->xdg_tree = wlr_scene_xdg_surface_create(view->tree, toplevel->base);
    }
    if (!view->xdg_tree) {
        if (view->tree) {
            wlr_scene_node_destroy(&view->tree->node);
        }
        free(view);
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
        return;
    }
    view->tree->node.data = view;
    wlr_scene_node_set_enabled(&view->tree->node, false);
    view->commit.notify = view_commit;
    view->map.notify = view_map;
    view->unmap.notify = view_unmap;
    view->destroy.notify = view_destroy;
    view->request_maximize.notify = view_request_maximize;
    view->request_fullscreen.notify = view_request_fullscreen;
    view->set_parent.notify = view_set_parent;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    wl_signal_add(&toplevel->events.set_parent, &view->set_parent);
    wl_list_insert(&scene->views, &view->link);
    view->id = emit_window(view, SEASIDE_WINDOW_CREATED, false, false);
    if (scene->window && !view->id) {
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
    }
    scene->snapshot.live_toplevels++;
}

static void unconstrain_popup(struct scene_popup *popup) {
    const struct scene_view *view = popup->view;
    const struct seaside_scene_rect *output = &view->placement.output;
    if (output->width > 0 && output->height > 0) {
        struct wlr_box bounds = {
            .x = output->x - view->placement.geometry.x + view->toplevel->base->geometry.x,
            .y = output->y - view->placement.geometry.y + view->toplevel->base->geometry.y,
            .width = output->width,
            .height = output->height,
        };
        wlr_xdg_popup_unconstrain_from_box(popup->popup, &bounds);
    }
}

static void clip_popup(struct scene_popup *popup) {
    const struct seaside_scene_rect viewport = popup->view->placement.clip;
    struct wlr_scene_node *node = &popup->surface_tree->node;
    if (!has_viewport(viewport)) {
        wlr_scene_subsurface_tree_set_clip(node, NULL);
        return;
    }
    int x, y;
    wlr_scene_node_coords(node, &x, &y);
    const struct wlr_box clip = {clamp_coordinate((int64_t)viewport.x - x),
                                 clamp_coordinate((int64_t)viewport.y - y), viewport.width,
                                 viewport.height};
    wlr_scene_subsurface_tree_set_clip(node, &clip);
}

static void clip_view_popups(struct scene_view *view) {
    struct scene_popup *popup;
    wl_list_for_each(popup, &view->owner->popups, link) {
        if (popup->view == view) {
            clip_popup(popup);
        }
    }
}

static void popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->popup->base->initial_commit) {
        unconstrain_popup(popup);
        wlr_xdg_surface_schedule_configure(popup->popup->base);
    }
    const struct wlr_box position = popup->popup->current.geometry;
    const struct wlr_box geometry = popup->popup->base->geometry;
    wlr_scene_node_set_position(&popup->tree->node, position.x, position.y);
    wlr_scene_node_set_position(&popup->surface_tree->node, clamp_coordinate(-(int64_t)geometry.x),
                                clamp_coordinate(-(int64_t)geometry.y));
    clip_view_popups(popup->view);
}

static void popup_reposition(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, reposition);
    unconstrain_popup(popup);
}

static void popup_tree_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, tree_destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->tree_destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, destroy);
    wlr_scene_node_destroy(&popup->tree->node);
}

static void new_popup(struct wl_listener *listener, void *data) {
    struct seaside_scene_bridge *scene = wl_container_of(listener, scene, new_popup);
    struct wlr_xdg_popup *xdg = data;
    if (!scene->window || !xdg->parent) {
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    struct scene_view *view =
        toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(xdg->parent));
    /* Keep ancestor movement in the scene hierarchy while giving every XDG
     * surface its own local viewport clip. A popup can extend beyond its tile. */
    struct wlr_scene_tree *parent_tree = view ? view->tree : NULL;
    if (!parent_tree) {
        struct scene_popup *parent = surface_popup(scene, xdg->parent);
        if (parent) {
            view = parent->view;
            parent_tree = parent->tree;
        }
    }
    if (!parent_tree) {
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    struct scene_popup *popup = calloc(1, sizeof(*popup));
    if (popup) {
        popup->tree = wlr_scene_tree_create(parent_tree);
        if (popup->tree) {
            popup->surface_tree = wlr_scene_subsurface_tree_create(popup->tree, xdg->base->surface);
        }
    }
    if (!popup || !popup->surface_tree) {
        if (popup && popup->tree) {
            wlr_scene_node_destroy(&popup->tree->node);
        }
        free(popup);
        wl_client_post_no_memory(wl_resource_get_client(xdg->resource));
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    popup->view = view;
    popup->popup = xdg;
    popup->commit.notify = popup_commit;
    popup->reposition.notify = popup_reposition;
    popup->destroy.notify = popup_destroy;
    popup->tree_destroy.notify = popup_tree_destroy;
    wl_list_insert(&scene->popups, &popup->link);
    wl_signal_add(&xdg->base->surface->events.commit, &popup->commit);
    wl_signal_add(&xdg->events.reposition, &popup->reposition);
    wl_signal_add(&xdg->events.destroy, &popup->destroy);
    wl_signal_add(&popup->tree->node.events.destroy, &popup->tree_destroy);
}

static struct scene_output *find_output(struct seaside_scene_bridge *scene,
                                        const struct wlr_output *output) {
    struct scene_output *view;
    wl_list_for_each(view, &scene->outputs, link) {
        if (view->viewport->output == output) {
            return view;
        }
    }
    return NULL;
}

static void fit_wallpaper(struct scene_output *view) {
    const struct wlr_buffer *image = view->owner->wallpaper;
    if (!image || view->width <= 0 || view->height <= 0) {
        wlr_scene_node_set_enabled(&view->wallpaper->node, false);
        return;
    }
    const double output_ratio = (double)view->width / view->height;
    struct wlr_fbox crop = {.width = image->width, .height = image->height};
    if (crop.width / crop.height > output_ratio) {
        crop.width = crop.height * output_ratio;
        crop.x = (image->width - crop.width) / 2;
    } else {
        crop.height = crop.width / output_ratio;
        crop.y = (image->height - crop.height) / 2;
    }
    wlr_scene_buffer_set_source_box(view->wallpaper, &crop);
    wlr_scene_buffer_set_dest_size(view->wallpaper, view->width, view->height);
    wlr_scene_node_set_enabled(&view->wallpaper->node, true);
}

static void fit_banner(struct scene_output *view) {
    const bool visible = view->banner_logical_height > 0 && view->width > 0 && view->height > 0;
    wlr_scene_node_set_enabled(&view->banner->node, visible);
    if (visible) {
        const int height =
            view->banner_logical_height < view->height ? view->banner_logical_height : view->height;
        const struct wlr_fbox crop = {
            .width = view->banner_width,
            .height = (double)view->banner_height * height / view->banner_logical_height,
        };
        wlr_scene_buffer_set_source_box(view->banner, &crop);
        wlr_scene_buffer_set_dest_size(view->banner, view->width, height);
    }
}

static void update_output_appearance(struct scene_output *view) {
    struct wlr_output *output = view->viewport->output;
    int width = 0, height = 0;
    if (output->enabled) {
        wlr_output_effective_resolution(output, &width, &height);
    }
    const bool reraster = view->width != width || view->scale != output->scale;
    view->width = width;
    view->height = height;
    view->scale = output->scale;
    wlr_scene_node_set_position(&view->wallpaper->node, view->viewport->x, view->viewport->y);
    wlr_scene_node_set_position(&view->banner->node, view->viewport->x, view->viewport->y);
    fit_wallpaper(view);
    if (reraster) {
        // The old banner size belongs to another viewport. Drop it before
        // moving or stretching it into the new geometry.
        wlr_scene_buffer_set_buffer(view->banner, NULL);
        view->banner_logical_height = 0;
        if (view->owner->appearance) {
            view->owner->appearance(view->owner->appearance_data, output, width, output->scale);
        }
    }
    fit_banner(view);
}

static void output_commit(struct wl_listener *listener, void *data) {
    struct scene_output *view = wl_container_of(listener, view, commit);
    const struct wlr_output_event_commit *event = data;
    if (event->state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE |
                                   WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM)) {
        update_output_appearance(view);
    }
}

static void output_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_output *output = wl_container_of(listener, output, frame);
    if (!wlr_scene_output_commit(output->viewport, NULL)) {
        output->owner->snapshot.failed_frames++;
        output->owner->snapshot.failed = true;
        return;
    }
    output->owner->snapshot.frames++;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        wlr_scene_output_send_frame_done(output->viewport, &now);
    } else {
        output->owner->snapshot.failed = true;
    }
}

static void output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_output *output = wl_container_of(listener, output, destroy);
    struct seaside_scene_bridge *scene = output->owner;
    output->owner->snapshot.outputs--;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->commit.link);
    wl_list_remove(&output->link);
    wlr_scene_node_destroy(&output->wallpaper->node);
    wlr_scene_node_destroy(&output->banner->node);
    if (scene->appearance) {
        scene->appearance(scene->appearance_data, output->viewport->output, 0, 0);
    }
    free(output);
}

static void detach(struct seaside_scene_bridge *scene) {
    if (!scene->attached) {
        return;
    }
    end_focus(scene, false);
    scene->window = NULL;
    scene->window_data = NULL;
    scene->watched_pid = 0;
    scene->focus = NULL;
    scene->focus_data = NULL;
    wl_list_remove(&scene->new_toplevel.link);
    wl_list_remove(&scene->new_popup.link);
    wl_list_remove(&scene->shell_destroy.link);
    if (scene->activation) {
        wl_list_remove(&scene->request_activation.link);
        wl_list_remove(&scene->activation_destroy.link);
        scene->activation = NULL;
    }
    struct scene_view *view, *next_view;
    wl_list_for_each_safe(view, next_view, &scene->views, link) {
        destroy_view(view);
    }
    seaside_window_capture_destroy(scene->capture);
    scene->capture = NULL;
    while (!wl_list_empty(&scene->outputs)) {
        struct scene_output *output = wl_container_of(scene->outputs.next, output, link);
        wlr_scene_output_destroy(output->viewport);
    }
    seaside_scene_drop_image(scene->wallpaper);
    scene->wallpaper = NULL;
    wlr_scene_node_destroy(&scene->scene->tree.node);
    scene->scene = NULL;
    scene->shell = NULL;
    scene->attached = false;
}

static void shell_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_scene_bridge *scene = wl_container_of(listener, scene, shell_destroy);
    detach(scene);
}

static void activation_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_scene_bridge *scene = wl_container_of(listener, scene, activation_destroy);
    wl_list_remove(&scene->request_activation.link);
    wl_list_remove(&scene->activation_destroy.link);
    scene->activation = NULL;
}

static void request_activation(struct wl_listener *listener, void *data) {
    struct seaside_scene_bridge *scene = wl_container_of(listener, scene, request_activation);
    const struct wlr_xdg_activation_v1_request_activate_event *request = data;
    struct wlr_surface *root = wlr_surface_get_root_surface(request->surface);
    struct scene_view *view =
        root ? toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root)) : NULL;
    if (view && view->mapped && !view->placement.activated) {
        emit_window(view, SEASIDE_WINDOW_ATTENTION_REQUESTED, true, false);
    }
}

struct seaside_scene_bridge *seaside_scene_create(struct wl_display *display) {
    if (!display) {
        return NULL;
    }
    struct seaside_scene_bridge *scene = calloc(1, sizeof(*scene));
    if (!scene) {
        return NULL;
    }
    scene->display = display;
    wl_list_init(&scene->views);
    wl_list_init(&scene->popups);
    wl_list_init(&scene->outputs);
    scene->scene = wlr_scene_create();
    if (!scene->scene) {
        free(scene);
        return NULL;
    }
    scene->backgrounds = wlr_scene_tree_create(&scene->scene->tree);
    scene->windows = wlr_scene_tree_create(&scene->scene->tree);
    scene->drag_icons = wlr_scene_tree_create(&scene->scene->tree);
    scene->decorations = wlr_scene_tree_create(&scene->scene->tree);
    if (!scene->backgrounds || !scene->windows || !scene->drag_icons || !scene->decorations) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        free(scene);
        return NULL;
    }
    scene->shell = wlr_xdg_shell_create(display, 6);
    if (!scene->shell) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        free(scene);
        return NULL;
    }
    scene->activation = wlr_xdg_activation_v1_create(display);
    if (!scene->activation) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        free(scene);
        return NULL;
    }
    scene->new_toplevel.notify = new_toplevel;
    scene->new_popup.notify = new_popup;
    scene->shell_destroy.notify = shell_destroy;
    scene->activation_destroy.notify = activation_destroy;
    scene->request_activation.notify = request_activation;
    wl_signal_add(&scene->shell->events.new_toplevel, &scene->new_toplevel);
    wl_signal_add(&scene->shell->events.new_popup, &scene->new_popup);
    wl_signal_add(&scene->shell->events.destroy, &scene->shell_destroy);
    wl_signal_add(&scene->activation->events.destroy, &scene->activation_destroy);
    wl_signal_add(&scene->activation->events.request_activate, &scene->request_activation);
    scene->attached = true;
    return scene;
}

void seaside_scene_set_drag_icon(struct seaside_scene_bridge *scene, struct wlr_drag_icon *icon) {
    if (scene && scene->attached && icon) {
        wlr_scene_drag_icon_create(scene->drag_icons, icon);
    }
}

void seaside_scene_move_drag_icon(struct seaside_scene_bridge *scene, int x, int y) {
    if (scene && scene->attached && !wl_list_empty(&scene->drag_icons->children)) {
        wlr_scene_node_set_position(&scene->drag_icons->node, x, y);
    }
}

void seaside_scene_destroy(struct seaside_scene_bridge *scene) {
    if (scene) {
        detach(scene);
        free(scene);
    }
}

bool seaside_scene_add_output(struct seaside_scene_bridge *scene, struct wlr_output *output) {
    if (!scene || !scene->attached || !output || !output->enabled ||
        wlr_scene_get_scene_output(scene->scene, output)) {
        return false;
    }
    struct scene_output *view = calloc(1, sizeof(*view));
    if (!view) {
        return false;
    }
    view->owner = scene;
    view->viewport = wlr_scene_output_create(scene->scene, output);
    if (!view->viewport) {
        free(view);
        return false;
    }
    view->wallpaper = wlr_scene_buffer_create(scene->backgrounds, scene->wallpaper);
    view->banner = wlr_scene_buffer_create(scene->decorations, NULL);
    if (!view->wallpaper || !view->banner) {
        if (view->wallpaper) {
            wlr_scene_node_destroy(&view->wallpaper->node);
        }
        if (view->banner) {
            wlr_scene_node_destroy(&view->banner->node);
        }
        wlr_scene_output_destroy(view->viewport);
        free(view);
        return false;
    }
    view->frame.notify = output_frame;
    view->destroy.notify = output_destroy;
    view->commit.notify = output_commit;
    wl_signal_add(&output->events.frame, &view->frame);
    wl_signal_add(&view->viewport->events.destroy, &view->destroy);
    wl_signal_add(&output->events.commit, &view->commit);
    wl_list_insert(&scene->outputs, &view->link);
    scene->snapshot.outputs++;
    update_output_appearance(view);
    wlr_output_schedule_frame(output);
    return true;
}

bool seaside_scene_place_output(struct seaside_scene_bridge *scene, struct wlr_output *output,
                                int x, int y) {
    if (!scene || !scene->attached || !output) {
        return false;
    }
    struct scene_output *view = find_output(scene, output);
    if (!view) {
        return false;
    }
    wlr_scene_output_set_position(view->viewport, x, y);
    update_output_appearance(view);
    wlr_output_create_global(output, scene->display);
    return output->global != NULL;
}

void seaside_scene_set_wallpaper(struct seaside_scene_bridge *scene, struct wlr_buffer *buffer) {
    if (!scene || !scene->attached) {
        seaside_scene_drop_image(buffer);
        return;
    }
    struct wlr_buffer *previous = scene->wallpaper;
    scene->wallpaper = buffer;
    struct scene_output *view;
    wl_list_for_each(view, &scene->outputs, link) {
        wlr_scene_buffer_set_buffer(view->wallpaper, buffer);
        fit_wallpaper(view);
    }
    seaside_scene_drop_image(previous);
}

void seaside_scene_set_banner(struct seaside_scene_bridge *scene, struct wlr_output *output,
                              struct wlr_buffer *buffer, int logical_height) {
    if (scene && scene->attached) {
        struct scene_output *view = find_output(scene, output);
        if (view) {
            view->banner_width = buffer ? buffer->width : 0;
            view->banner_height = buffer ? buffer->height : 0;
            view->banner_logical_height = buffer ? logical_height : 0;
            wlr_scene_buffer_set_buffer(view->banner, buffer);
            fit_banner(view);
        }
    }
    seaside_scene_drop_image(buffer);
}

void seaside_scene_set_appearance_handler(struct seaside_scene_bridge *scene,
                                          seaside_scene_output_appearance_fn handler, void *data) {
    if (scene && scene->attached) {
        scene->appearance = handler;
        scene->appearance_data = data;
    }
}

void seaside_scene_remove_output(struct seaside_scene_bridge *scene, struct wlr_output *output) {
    if (!scene || !scene->attached || !output) {
        return;
    }
    struct wlr_scene_output *viewport = wlr_scene_get_scene_output(scene->scene, output);
    if (viewport) {
        wlr_scene_output_destroy(viewport);
        wlr_output_destroy_global(output);
    }
}

void seaside_scene_set_window_handler(struct seaside_scene_bridge *scene,
                                      seaside_scene_window_fn handler, void *data) {
    if (scene && scene->attached) {
        scene->window = handler;
        scene->window_data = data;
    }
}

bool seaside_scene_enable_window_capture(struct seaside_scene_bridge *scene,
                                         struct wlr_allocator *allocator,
                                         struct wlr_renderer *renderer) {
    if (!scene || !scene->attached || scene->capture) {
        return false;
    }
    scene->capture = seaside_window_capture_create(scene->display, allocator, renderer);
    return scene->capture != NULL;
}

bool seaside_scene_window_capture_client_ready(const struct seaside_scene_bridge *scene, pid_t pid,
                                               uid_t uid) {
    return scene && seaside_window_capture_client_ready(scene->capture, pid, uid);
}

static bool below(const struct scene_view *left, const struct scene_view *right) {
    if (left->placement.stacking != right->placement.stacking) {
        return left->placement.stacking < right->placement.stacking;
    }
    return left->id < right->id;
}

static void restack_view(struct scene_view *view) {
    struct scene_view *previous = NULL;
    struct scene_view *other;
    wl_list_for_each(other, &view->owner->views, link) {
        if (other->placement.visible && below(other, view) &&
            (!previous || below(previous, other))) {
            previous = other;
        }
    }
    if (previous) {
        wlr_scene_node_place_above(&view->tree->node, &previous->tree->node);
    } else {
        wlr_scene_node_lower_to_bottom(&view->tree->node);
    }
}

static void project_border(struct scene_view *view, size_t side, struct wlr_box bounds) {
    struct wlr_scene_rect *border = view->borders[side];
    if (bounds.width <= 0 || bounds.height <= 0) {
        if (border) {
            wlr_scene_node_set_enabled(&border->node, false);
        }
        return;
    }
    if (!border) {
        const float gray[4] = {0.35f, 0.35f, 0.35f, 1};
        border = wlr_scene_rect_create(view->tree, bounds.width, bounds.height, gray);
        if (!border) {
            view->owner->snapshot.failed = true;
            return;
        }
        view->borders[side] = border;
        wlr_scene_node_place_below(&border->node, &view->xdg_tree->node);
    }
    const struct seaside_scene_rect box = view->placement.geometry;
    wlr_scene_node_set_position(&border->node, clamp_coordinate((int64_t)bounds.x - box.x),
                                clamp_coordinate((int64_t)bounds.y - box.y));
    wlr_scene_rect_set_size(border, bounds.width, bounds.height);
    wlr_scene_node_set_enabled(&border->node, true);
}

static void project_borders(struct scene_view *view) {
    const struct seaside_scene_window_placement *placement = &view->placement;
    const struct seaside_scene_rect clip = placement->clip;
    const struct seaside_scene_rect box = placement->geometry;
    const int64_t width = placement->border_width;
    if (width <= 0 || !has_viewport(box) || !has_viewport(clip)) {
        for (size_t side = 0; side < sizeof(view->borders) / sizeof(view->borders[0]); ++side) {
            project_border(view, side, (struct wlr_box){0});
        }
        return;
    }
    const int64_t left = (int64_t)box.x - width;
    const int64_t top = (int64_t)box.y - width;
    const int64_t right = (int64_t)box.x + box.width;
    const int64_t bottom = (int64_t)box.y + box.height;
    const int64_t span = box.width + 2 * width;
    project_border(view, 0, intersection(clip, left, top, span, width));
    project_border(view, 1, intersection(clip, left, bottom, span, width));
    project_border(view, 2, intersection(clip, left, box.y, width, box.height));
    project_border(view, 3, intersection(clip, right, box.y, width, box.height));
}

void seaside_scene_place_window(struct seaside_scene_bridge *scene, uint64_t id,
                                const struct seaside_scene_window_placement *placement) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return;
    }
    struct seaside_scene_window_placement previous = view->placement;
    view->placement = *placement;
    const struct seaside_scene_rect box = placement->geometry;
    wlr_scene_node_set_position(&view->tree->node, box.x, box.y);
    clip_toplevel(view);
    clip_view_popups(view);
    project_borders(view);
    wlr_scene_node_set_enabled(&view->tree->node, view->mapped && placement->visible);
    seaside_window_capture_presented(view->capture,
                                     placement->visible && has_viewport(placement->clip));
    if (placement->visible && (!previous.visible || placement->stacking != previous.stacking)) {
        restack_view(view);
    }
    /* While the client is still applying a fullscreen configure, its old
     * buffer may cover only part of the output. Keep that output's backdrop
     * opaque throughout the transition. */
    if (placement->fullscreen && !view->fullscreen_background) {
        const float black[4] = {0, 0, 0, 1};
        view->fullscreen_background =
            wlr_scene_rect_create(view->tree, box.width, box.height, black);
        if (!view->fullscreen_background) {
            scene->snapshot.failed = true;
        } else {
            wlr_scene_node_lower_to_bottom(&view->fullscreen_background->node);
        }
    }
    if (view->fullscreen_background) {
        wlr_scene_node_set_enabled(&view->fullscreen_background->node, placement->fullscreen);
        if (placement->fullscreen) {
            wlr_scene_rect_set_size(view->fullscreen_background, box.width, box.height);
        }
    }
    struct wlr_xdg_toplevel *toplevel = view->toplevel;
    if (previous.visible && !placement->visible) {
        while (!wl_list_empty(&toplevel->base->popups)) {
            struct wlr_xdg_popup *popup = wl_container_of(toplevel->base->popups.next, popup, link);
            wlr_xdg_popup_destroy(popup);
        }
    }
    if (!toplevel->base->initialized) {
        return;
    }
    const bool maximized = placement->maximized && !placement->fullscreen;
    if (toplevel->scheduled.maximized != maximized) {
        wlr_xdg_toplevel_set_maximized(toplevel, maximized);
    }
    if (toplevel->scheduled.fullscreen != placement->fullscreen) {
        wlr_xdg_toplevel_set_fullscreen(toplevel, placement->fullscreen);
    }
    if (toplevel->scheduled.activated != placement->activated) {
        wlr_xdg_toplevel_set_activated(toplevel, placement->activated);
    }
    const uint32_t tiled =
        placement->tiled ? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT : 0;
    if (toplevel->scheduled.tiled != tiled) {
        wlr_xdg_toplevel_set_tiled(toplevel, tiled);
    }
    if (toplevel->scheduled.resizing != placement->resizing) {
        wlr_xdg_toplevel_set_resizing(toplevel, placement->resizing);
    }
    const bool managed = maximized || placement->fullscreen || placement->tiled;
    const bool restoring =
        !managed && (previous.maximized || previous.fullscreen || previous.tiled);
    if (restoring || (has_viewport(box) && (managed || placement->resize))) {
        /* A client that mapped straight into a managed state may have no
         * saved floating size. Zero lets that client choose its new size. */
        const int width = box.width > 0 ? box.width : 0;
        const int height = box.height > 0 ? box.height : 0;
        if (width != toplevel->scheduled.width || height != toplevel->scheduled.height ||
            restoring) {
            uint32_t serial = wlr_xdg_toplevel_set_size(toplevel, width, height);
            if (!managed) {
                view->restore_serial = serial;
            }
        }
    }
}

struct wlr_surface *seaside_scene_window_surface(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    return view && view->mapped ? view->toplevel->base->surface : NULL;
}

bool seaside_scene_close_window(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view || !view->mapped) {
        return false;
    }
    wlr_xdg_toplevel_send_close(view->toplevel);
    return true;
}

struct seaside_scene_size_limits
seaside_scene_window_size_limits(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return (struct seaside_scene_size_limits){0};
    }
    const struct wlr_xdg_toplevel_state *current = &view->toplevel->current;
    return (struct seaside_scene_size_limits){current->min_width, current->min_height,
                                              current->max_width, current->max_height};
}

struct seaside_scene_window_metadata
seaside_scene_get_window_metadata(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return (struct seaside_scene_window_metadata){0};
    }
    return (struct seaside_scene_window_metadata){view->toplevel->app_id, view->toplevel->title};
}

static struct scene_view *node_view(struct seaside_scene_bridge *scene,
                                    struct wlr_scene_node *node) {
    while (node->parent && node->parent != scene->windows) {
        node = &node->parent->node;
    }
    return node->parent ? node->data : NULL;
}

struct exact_target {
    struct seaside_scene_target *target;
    double x, y;
    bool found;
};

static void match_target(struct wlr_surface *surface, int x, int y, void *data) {
    struct exact_target *search = data;
    if (surface != search->target->surface) {
        return;
    }
    search->target->sx = search->x - x;
    search->target->sy = search->y - y;
    search->found = true;
}

static bool exact_surface_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target) {
    struct wlr_surface *root = wlr_surface_get_root_surface(target->surface);
    if (!root || !root->mapped) {
        return false;
    }
    struct scene_popup *popup = surface_popup(scene, root);
    struct scene_view *view =
        popup ? popup->view : toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root));
    if (!view || !view->mapped || (view->id && !view->placement.visible)) {
        return false;
    }
    /* An implicit grab still needs surface coordinates when a viewport shift
     * clips its original target. The protocol tree gives its uncropped origin. */
    int lx, ly;
    struct wlr_scene_node *node = popup ? &popup->surface_tree->node : &view->tree->node;
    wlr_scene_node_coords(node, &lx, &ly);
    const struct wlr_box geometry = popup ? (struct wlr_box){0} : view->toplevel->base->geometry;
    struct exact_target search = {
        .target = target, .x = x - lx + geometry.x, .y = y - ly + geometry.y};
    wlr_surface_for_each_surface(root, match_target, &search);
    if (search.found) {
        target->id = view->id;
    }
    return search.found;
}

bool seaside_scene_window_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target) {
    if (!scene || !scene->attached) {
        return false;
    }
    if (target->surface) {
        return exact_surface_at(scene, x, y, target);
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&scene->windows->node, x, y, &target->sx, &target->sy);
    if (!node) {
        return false;
    }
    struct scene_view *view = node_view(scene, node);
    if (!view || !view->mapped) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_RECT) {
        for (size_t side = 0; side < sizeof(view->borders) / sizeof(view->borders[0]); ++side) {
            if (view->borders[side] && node == &view->borders[side]->node) {
                target->id = view->id;
                return true;
            }
        }
        return false;
    }
    if (node->type != WLR_SCENE_NODE_BUFFER) {
        return false;
    }
    struct wlr_scene_surface *surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!surface) {
        return false;
    }
    target->id = view->id;
    target->surface = surface->surface;
    return true;
}

struct seaside_scene_snapshot seaside_scene_get_snapshot(const struct seaside_scene_bridge *scene) {
    return scene ? scene->snapshot : (struct seaside_scene_snapshot){0};
}

bool seaside_scene_watch_client(struct seaside_scene_bridge *scene, pid_t pid,
                                seaside_scene_focus_fn focus, void *data) {
    if (!scene || !scene->attached || scene->watched_pid != 0 || pid <= 0 || !focus) {
        return false;
    }
    scene->watched_pid = pid;
    scene->focus = focus;
    scene->focus_data = data;
    return true;
}

void seaside_scene_clear_client_watch(struct seaside_scene_bridge *scene) {
    if (scene) {
        end_focus(scene, true);
        scene->watched_pid = 0;
        scene->focus = NULL;
        scene->focus_data = NULL;
    }
}

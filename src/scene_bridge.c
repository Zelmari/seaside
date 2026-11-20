#define _POSIX_C_SOURCE 200809L
#include "scene_bridge.h"

#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

struct seaside_scene_bridge {
    struct wlr_scene *scene;
    struct wlr_xdg_shell *shell;
    struct wl_list views;
    struct wl_list outputs;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_listener shell_destroy;
    struct seaside_scene_snapshot snapshot;
    bool attached;
};

struct scene_view {
    struct seaside_scene_bridge *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree *tree;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    bool mapped;
};

struct scene_output {
    struct seaside_scene_bridge *owner;
    struct wlr_scene_output *viewport;
    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener destroy;
};

static void view_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, map);
    if (!view->mapped) {
        view->mapped = true;
        view->owner->snapshot.mapped_toplevels++;
        view->owner->snapshot.map_events++;
    }
    wlr_scene_node_set_enabled(&view->tree->node, true);
}

static void view_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, unmap);
    if (view->mapped) {
        view->mapped = false;
        view->owner->snapshot.mapped_toplevels--;
        view->owner->snapshot.unmap_events++;
    }
    wlr_scene_node_set_enabled(&view->tree->node, false);
}

static void view_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, commit);
    if (view->toplevel->base->initial_commit) {
        /* Let the client choose its initial size. Layout policy belongs to
         * later parts; advertised capabilities must match this minimal stage. */
        wlr_xdg_toplevel_set_wm_capabilities(view->toplevel, 0);
        wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        view->owner->snapshot.initial_configures++;
    }
    if (wlr_surface_has_buffer(view->toplevel->base->surface)) {
        view->owner->snapshot.buffer_commits++;
    }
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_maximize);
    /* XDG requires a configure response even when an unsupported state request
     * leaves the existing state unchanged. Do not invent maximize policy. */
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_fullscreen);
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void destroy_view(struct scene_view *view) {
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
    wl_list_remove(&view->link);
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
    view->tree = wlr_scene_tree_create(&scene->scene->tree);
    if (!view->tree || !wlr_scene_xdg_surface_create(view->tree, toplevel->base)) {
        if (view->tree) {
            wlr_scene_node_destroy(&view->tree->node);
        }
        free(view);
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
        return;
    }
    wlr_scene_node_set_enabled(&view->tree->node, false);
    view->commit.notify = view_commit;
    view->map.notify = view_map;
    view->unmap.notify = view_unmap;
    view->destroy.notify = view_destroy;
    view->request_maximize.notify = view_request_maximize;
    view->request_fullscreen.notify = view_request_fullscreen;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    wl_list_insert(&scene->views, &view->link);
    scene->snapshot.live_toplevels++;
}

static void new_popup(struct wl_listener *listener, void *data) {
    (void)listener;
    /* Popup positioning/grabs depend on the future seat/focus integration.
     * Dismiss rather than advertising a configured but unmanaged popup. */
    wlr_xdg_popup_destroy(data);
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
    output->owner->snapshot.outputs--;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);
    free(output);
}

static void detach(struct seaside_scene_bridge *scene) {
    if (!scene->attached) {
        return;
    }
    wl_list_remove(&scene->new_toplevel.link);
    wl_list_remove(&scene->new_popup.link);
    wl_list_remove(&scene->shell_destroy.link);
    struct scene_view *view, *next_view;
    wl_list_for_each_safe(view, next_view, &scene->views, link) {
        destroy_view(view);
    }
    while (!wl_list_empty(&scene->outputs)) {
        struct scene_output *output = wl_container_of(scene->outputs.next, output, link);
        wlr_scene_output_destroy(output->viewport);
    }
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

struct seaside_scene_bridge *seaside_scene_create(struct wl_display *display) {
    if (!display) {
        return NULL;
    }
    struct seaside_scene_bridge *scene = calloc(1, sizeof(*scene));
    if (!scene) {
        return NULL;
    }
    wl_list_init(&scene->views);
    wl_list_init(&scene->outputs);
    scene->scene = wlr_scene_create();
    if (!scene->scene) {
        free(scene);
        return NULL;
    }
    scene->shell = wlr_xdg_shell_create(display, 6);
    if (!scene->shell) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        free(scene);
        return NULL;
    }
    scene->new_toplevel.notify = new_toplevel;
    scene->new_popup.notify = new_popup;
    scene->shell_destroy.notify = shell_destroy;
    wl_signal_add(&scene->shell->events.new_toplevel, &scene->new_toplevel);
    wl_signal_add(&scene->shell->events.new_popup, &scene->new_popup);
    wl_signal_add(&scene->shell->events.destroy, &scene->shell_destroy);
    scene->attached = true;
    return scene;
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
    view->frame.notify = output_frame;
    view->destroy.notify = output_destroy;
    wl_signal_add(&output->events.frame, &view->frame);
    wl_signal_add(&view->viewport->events.destroy, &view->destroy);
    wl_list_insert(&scene->outputs, &view->link);
    scene->snapshot.outputs++;
    wlr_output_schedule_frame(output);
    return true;
}

struct seaside_scene_snapshot seaside_scene_get_snapshot(const struct seaside_scene_bridge *scene) {
    return scene ? scene->snapshot : (struct seaside_scene_snapshot){0};
}

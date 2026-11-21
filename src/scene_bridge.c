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
    struct wl_display *display;
    struct wlr_scene *scene;
    struct wlr_xdg_shell *shell;
    struct wl_list views;
    struct wl_list popups;
    struct wl_list outputs;
    struct wl_listener new_toplevel;
    struct wl_listener new_popup;
    struct wl_listener shell_destroy;
    struct seaside_scene_snapshot snapshot;
    seaside_scene_window_fn window;
    void *window_data;
    struct scene_view *focused_view;
    pid_t watched_pid;
    seaside_scene_focus_fn focus;
    void *focus_data;
    bool attached;
};

struct scene_view {
    struct seaside_scene_bridge *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *xdg_tree;
    struct wlr_scene_rect *fullscreen_background;
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
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener destroy;
    struct wl_listener tree_destroy;
};

struct scene_output {
    struct seaside_scene_bridge *owner;
    struct wlr_scene_output *viewport;
    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener destroy;
};

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
        /* A commit carrying the previous special configure must not replace
         * the ordinary rectangle after the compositor requests restoration. */
        uint32_t serial = view->toplevel->base->current.configure_serial;
        bool ordinary =
            !view->toplevel->current.maximized && !view->toplevel->current.fullscreen &&
            (!view->restore_serial ||
             (serial && (uint32_t)(serial - view->restore_serial) < UINT32_C(0x80000000)));
        if (ordinary) {
            view->restore_serial = 0;
        }
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
    view->tree = wlr_scene_tree_create(&scene->scene->tree);
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

static void popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->popup->base->initial_commit) {
        unconstrain_popup(popup);
        wlr_xdg_surface_schedule_configure(popup->popup->base);
    }
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
    struct wlr_scene_tree *parent_tree = view ? view->xdg_tree : NULL;
    if (!parent_tree) {
        struct scene_popup *parent;
        wl_list_for_each(parent, &scene->popups, link) {
            if (parent->popup->base->surface == xdg->parent) {
                view = parent->view;
                parent_tree = parent->tree;
                break;
            }
        }
    }
    if (!parent_tree) {
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    struct scene_popup *popup = calloc(1, sizeof(*popup));
    if (popup) {
        popup->tree = wlr_scene_xdg_surface_create(parent_tree, xdg->base);
    }
    if (!popup || !popup->tree) {
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
    end_focus(scene, false);
    scene->window = NULL;
    scene->window_data = NULL;
    scene->watched_pid = 0;
    scene->focus = NULL;
    scene->focus_data = NULL;
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
    scene->display = display;
    wl_list_init(&scene->views);
    wl_list_init(&scene->popups);
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

bool seaside_scene_place_output(struct seaside_scene_bridge *scene, struct wlr_output *output,
                                int x, int y) {
    if (!scene || !scene->attached || !output) {
        return false;
    }
    struct wlr_scene_output *viewport = wlr_scene_get_scene_output(scene->scene, output);
    if (!viewport) {
        return false;
    }
    wlr_scene_output_set_position(viewport, x, y);
    wlr_output_create_global(output, scene->display);
    return output->global != NULL;
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
    wlr_scene_node_set_enabled(&view->tree->node, view->mapped && placement->visible);
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
    const bool special = maximized || placement->fullscreen;
    const bool restoring = !special && (previous.maximized || previous.fullscreen);
    if (special || restoring || placement->resize) {
        if (box.width != toplevel->scheduled.width || box.height != toplevel->scheduled.height ||
            restoring) {
            uint32_t serial = wlr_xdg_toplevel_set_size(toplevel, box.width, box.height);
            if (!special) {
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

static struct scene_view *node_view(struct seaside_scene_bridge *scene,
                                    struct wlr_scene_node *node) {
    while (node->parent && node->parent != &scene->scene->tree) {
        node = &node->parent->node;
    }
    return node->parent ? node->data : NULL;
}

struct exact_target {
    struct seaside_scene_bridge *scene;
    struct seaside_scene_target *target;
    double x, y;
    bool found;
};

static void match_target(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    struct exact_target *search = data;
    struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
    if (!surface || surface->surface != search->target->surface) {
        return;
    }
    struct scene_view *view = node_view(search->scene, &buffer->node);
    if (view && view->mapped) {
        search->target->id = view->id;
        search->target->sx = search->x - x;
        search->target->sy = search->y - y;
        search->found = true;
    }
}

bool seaside_scene_window_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target) {
    if (!scene || !scene->attached) {
        return false;
    }
    if (target->surface) {
        struct exact_target search = {.scene = scene, .target = target, .x = x, .y = y};
        wlr_scene_node_for_each_buffer(&scene->scene->tree.node, match_target, &search);
        return search.found;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&scene->scene->tree.node, x, y, &target->sx, &target->sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return false;
    }
    struct wlr_scene_surface *surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    struct scene_view *view = node_view(scene, node);
    if (!surface || !view || !view->mapped) {
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

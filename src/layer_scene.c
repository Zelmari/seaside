#include "layer_scene.h"
#include "popup_grab.h"

#include <limits.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

enum { LAYER_COUNT = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY + 1 };

struct seaside_layer_scene {
    struct wl_event_loop *loop;
    struct wlr_scene_tree *trees[LAYER_COUNT];
    struct wl_list outputs, surfaces, popups;
    struct wl_listener new_surface, shell_destroy;
    struct wl_event_source *update;
    seaside_layer_output_fn focused_output;
    seaside_layer_changed_fn changed;
    seaside_layer_hidden_fn hidden;
    void *data;
    struct layer_surface *selected;
};

struct layer_output {
    struct wlr_output *output;
    struct seaside_scene_rect bounds, work;
    int top_reservation;
    bool fullscreen;
    struct wl_list link;
};

struct layer_surface {
    struct seaside_layer_scene *owner;
    struct layer_output *output;
    struct wlr_layer_surface_v1 *role;
    struct wlr_scene_tree *tree, *surface_tree;
    struct wl_listener commit, unmap, destroy, new_popup;
    struct wl_list link;
    uint32_t width, height;
    bool configured, visible, mapped;
};

struct layer_popup {
    struct layer_surface *layer;
    struct wlr_xdg_popup *role;
    struct seaside_popup_focus focus;
    struct wlr_scene_tree *tree, *surface_tree;
    struct wl_listener commit, reposition, destroy, tree_destroy;
    struct wl_list link;
};

static int coordinate(int64_t value) {
    return value < INT_MIN ? INT_MIN : value > INT_MAX ? INT_MAX : (int)value;
}

static struct layer_output *find_output(struct seaside_layer_scene *layers,
                                        struct wlr_output *output) {
    struct layer_output *record;
    wl_list_for_each(record, &layers->outputs, link) {
        if (record->output == output) {
            return record;
        }
    }
    return NULL;
}

static bool sibling_layer(const struct layer_surface *surface, unsigned level) {
    return (unsigned)surface->role->current.layer == level;
}

static bool exclusive_keyboard(const struct layer_surface *surface) {
    return surface->role->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP &&
           surface->role->current.keyboard_interactive ==
               ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
}

static void dismiss_popups(struct layer_surface *surface) {
    /* Destroying an XDG popup also releases its protocol-owned seat grabs. */
    while (!wl_list_empty(&surface->role->popups)) {
        struct wlr_xdg_popup *popup = wl_container_of(surface->role->popups.next, popup, link);
        wlr_xdg_popup_destroy(popup);
    }
}

static void hide_surface(struct layer_surface *surface) {
    struct seaside_layer_scene *layers = surface->owner;
    if (layers->hidden) {
        layers->hidden(layers->data, surface->role->surface);
        struct layer_popup *popup;
        wl_list_for_each(popup, &layers->popups, link) {
            if (popup->layer == surface) {
                layers->hidden(layers->data, popup->role->base->surface);
            }
        }
    }
    dismiss_popups(surface);
}

static void set_visibility(struct layer_surface *surface) {
    const bool visible = surface->role->surface->mapped &&
                         (!surface->output->fullscreen || exclusive_keyboard(surface));
    wlr_scene_node_set_enabled(&surface->tree->node, visible);
    const bool was_visible = surface->visible;
    surface->visible = visible;
    if (!visible && was_visible) {
        hide_surface(surface);
    } else if (surface->role->current.committed &
                   WLR_LAYER_SURFACE_V1_STATE_KEYBOARD_INTERACTIVITY &&
               surface->role->current.keyboard_interactive ==
                   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        dismiss_popups(surface);
    }
    if (surface->owner->selected == surface &&
        (!visible || surface->role->current.keyboard_interactive ==
                         ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)) {
        surface->owner->selected = NULL;
    }
}

static void arrange_axis(int origin, int extent, uint32_t requested, bool leading, bool trailing,
                         int before, int after, int *position, int *size) {
    const int64_t low = (int64_t)origin + (leading ? before : 0);
    const int64_t high = (int64_t)origin + extent - (trailing ? after : 0);
    int64_t available = high - low;
    /* Clamp sizes and positions from client input before entering wlroots'
     * int-based scene geometry. 0 on the wire means the client chooses. */
    if (available < 1) {
        available = 1;
    }
    const int64_t desired = requested ? requested : available;
    *size = coordinate(desired < extent ? desired : extent);
    int64_t offset = leading ? low : high - *size;
    if (leading == trailing) {
        offset = low + (available - *size) / 2;
    }
    const int64_t first = (int64_t)origin - *size;
    const int64_t last = (int64_t)origin + extent;
    *position = coordinate(offset < first ? first : offset > last ? last : offset);
}

static void reserve_edge(struct seaside_scene_rect *box,
                         const struct wlr_layer_surface_v1_state *state, enum wlr_edges edge) {
    int margin, available;
    switch (edge) {
    case WLR_EDGE_TOP:
        margin = state->margin.top;
        available = box->height;
        break;
    case WLR_EDGE_BOTTOM:
        margin = state->margin.bottom;
        available = box->height;
        break;
    case WLR_EDGE_LEFT:
        margin = state->margin.left;
        available = box->width;
        break;
    case WLR_EDGE_RIGHT:
        margin = state->margin.right;
        available = box->width;
        break;
    default:
        return;
    }
    const int64_t requested = (int64_t)state->exclusive_zone + margin;
    // Keep a non-empty work area even if reservations exhaust an output.
    const int inset = requested < 0 ? 0 : requested >= available ? available - 1 : (int)requested;
    if (edge == WLR_EDGE_LEFT || edge == WLR_EDGE_RIGHT) {
        box->width -= inset;
        if (edge == WLR_EDGE_LEFT) {
            box->x += inset;
        }
    } else {
        box->height -= inset;
        if (edge == WLR_EDGE_TOP) {
            box->y += inset;
        }
    }
}

static void clip_tree(struct wlr_scene_tree *tree, struct seaside_scene_rect output) {
    int x, y;
    wlr_scene_node_coords(&tree->node, &x, &y);
    const struct wlr_box clip = {coordinate((int64_t)output.x - x),
                                 coordinate((int64_t)output.y - y), output.width, output.height};
    wlr_scene_subsurface_tree_set_clip(&tree->node, &clip);
}

static void configure_surface(struct layer_surface *surface, struct seaside_scene_rect *work,
                              enum wlr_edges reservation) {
    struct wlr_layer_surface_v1 *role = surface->role;
    const struct wlr_layer_surface_v1_state *state = &role->current;
    const struct seaside_scene_rect full = surface->output->bounds;
    const struct seaside_scene_rect bounds = state->exclusive_zone == -1 ? full : *work;
    const uint32_t anchor = state->anchor;
    struct wlr_box box;
    arrange_axis(bounds.x, bounds.width, state->desired_width,
                 anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
                 anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, state->margin.left,
                 state->margin.right, &box.x, &box.width);
    arrange_axis(bounds.y, bounds.height, state->desired_height,
                 anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
                 anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, state->margin.top,
                 state->margin.bottom, &box.y, &box.height);
    wlr_scene_node_set_position(&surface->tree->node, box.x, box.y);
    clip_tree(surface->surface_tree, full);
    struct layer_popup *popup;
    wl_list_for_each(popup, &surface->owner->popups, link) {
        if (popup->layer == surface) {
            clip_tree(popup->surface_tree, full);
        }
    }

    if (!surface->configured || surface->width != (uint32_t)box.width ||
        surface->height != (uint32_t)box.height) {
        surface->configured = true;
        surface->width = (uint32_t)box.width;
        surface->height = (uint32_t)box.height;
        wlr_layer_surface_v1_configure(role, surface->width, surface->height);
    }
    if (surface->visible) {
        reserve_edge(work, state, reservation);
    }
}

static void arrange_output(struct seaside_layer_scene *layers, struct layer_output *output) {
    struct seaside_scene_rect work = output->bounds;
    if (!output->fullscreen && work.height > 1) {
        const int height =
            output->top_reservation < work.height ? output->top_reservation : work.height - 1;
        work.y = coordinate((int64_t)work.y + height);
        work.height -= height;
    }
    struct layer_surface *surface;
    wl_list_for_each(surface, &layers->surfaces, link) {
        if (surface->output != output) {
            continue;
        }
        struct wlr_scene_tree *parent = layers->trees[surface->role->current.layer];
        wlr_scene_node_reparent(&surface->tree->node, parent);
        set_visibility(surface);
    }
    // First place mapped reservations, then place other surfaces in what remains.
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = LAYER_COUNT; i > 0; --i) {
            wl_list_for_each(surface, &layers->surfaces, link) {
                if (surface->output != output || !surface->role->initialized ||
                    !sibling_layer(surface, i - 1)) {
                    continue;
                }
                const enum wlr_edges edge =
                    surface->visible ? wlr_layer_surface_v1_get_exclusive_edge(surface->role)
                                     : WLR_EDGE_NONE;
                if ((edge == WLR_EDGE_NONE) == (pass == 1)) {
                    configure_surface(surface, &work, edge);
                }
            }
        }
    }
    output->work = work;
}

static void update_layers(void *data) {
    struct seaside_layer_scene *layers = data;
    layers->update = NULL;
    struct layer_output *output;
    wl_list_for_each(output, &layers->outputs, link) {
        arrange_output(layers, output);
    }
    if (layers->changed) {
        layers->changed(layers->data);
    }
}

static void schedule_update(struct seaside_layer_scene *layers) {
    if (!layers->update) {
        layers->update = wl_event_loop_add_idle(layers->loop, update_layers, layers);
    }
}

static void surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_surface *surface = wl_container_of(listener, surface, commit);
    struct wlr_layer_surface_v1 *role = surface->role;
    const bool mapped = role->surface->mapped;
    if (role->initial_commit || role->current.committed || surface->mapped != mapped) {
        schedule_update(surface->owner);
    }
    surface->mapped = mapped;
}

static void surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_surface *surface = wl_container_of(listener, surface, unmap);
    surface->configured = false;
    surface->mapped = false;
    wlr_scene_node_set_enabled(&surface->tree->node, false);
    if (surface->visible) {
        surface->visible = false;
        hide_surface(surface);
    }
    if (surface->owner->selected == surface) {
        surface->owner->selected = NULL;
    }
    schedule_update(surface->owner);
}

static void surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_surface *surface = wl_container_of(listener, surface, destroy);
    struct seaside_layer_scene *layers = surface->owner;
    if (layers->selected == surface) {
        layers->selected = NULL;
    }
    wl_list_remove(&surface->commit.link);
    wl_list_remove(&surface->unmap.link);
    wl_list_remove(&surface->destroy.link);
    wl_list_remove(&surface->new_popup.link);
    wl_list_remove(&surface->link);
    wlr_scene_node_destroy(&surface->tree->node);
    free(surface);
    schedule_update(layers);
}

static void popup_created(struct wl_listener *listener, void *data) {
    struct layer_surface *surface = wl_container_of(listener, surface, new_popup);
    (void)seaside_layer_scene_add_popup(surface->owner, data);
}

static void new_surface(struct wl_listener *listener, void *data) {
    struct seaside_layer_scene *layers = wl_container_of(listener, layers, new_surface);
    struct wlr_layer_surface_v1 *role = data;
    struct layer_output *output = find_output(layers, role->output);
    if (!role->output && !wl_list_empty(&layers->outputs)) {
        if (layers->focused_output) {
            output = find_output(layers, layers->focused_output(layers->data));
        }
        if (!output) {
            output = wl_container_of(layers->outputs.next, output, link);
        }
    }
    if (!output) {
        wlr_layer_surface_v1_destroy(role);
        return;
    }

    struct layer_surface *surface = calloc(1, sizeof(*surface));
    if (surface) {
        surface->tree = wlr_scene_tree_create(layers->trees[role->current.layer]);
        if (surface->tree) {
            surface->surface_tree = wlr_scene_subsurface_tree_create(surface->tree, role->surface);
        }
    }
    if (!surface || !surface->surface_tree) {
        if (surface && surface->tree) {
            wlr_scene_node_destroy(&surface->tree->node);
        }
        free(surface);
        wl_client_post_no_memory(wl_resource_get_client(role->resource));
        wlr_layer_surface_v1_destroy(role);
        return;
    }
    role->output = output->output;
    surface->role = role;
    surface->owner = layers;
    surface->output = output;
    wlr_scene_node_set_enabled(&surface->tree->node, false);
    surface->commit.notify = surface_commit;
    surface->unmap.notify = surface_unmap;
    surface->destroy.notify = surface_destroy;
    surface->new_popup.notify = popup_created;
    wl_signal_add(&role->surface->events.commit, &surface->commit);
    wl_signal_add(&role->surface->events.unmap, &surface->unmap);
    wl_signal_add(&role->events.destroy, &surface->destroy);
    wl_signal_add(&role->events.new_popup, &surface->new_popup);
    wl_list_insert(layers->surfaces.prev, &surface->link);
}

static void shell_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_layer_scene *layers = wl_container_of(listener, layers, shell_destroy);
    wl_list_remove(&layers->new_surface.link);
    wl_list_remove(&layers->shell_destroy.link);
    wl_list_init(&layers->new_surface.link);
    wl_list_init(&layers->shell_destroy.link);
}

struct seaside_layer_scene *seaside_layer_scene_create(struct wl_display *display,
                                                       struct wlr_scene_tree *windows,
                                                       struct wlr_scene_tree *drag_icons) {
    struct seaside_layer_scene *layers = calloc(1, sizeof(*layers));
    if (!layers) {
        return NULL;
    }
    layers->loop = wl_display_get_event_loop(display);
    wl_list_init(&layers->outputs);
    wl_list_init(&layers->surfaces);
    wl_list_init(&layers->popups);
    wl_list_init(&layers->new_surface.link);
    wl_list_init(&layers->shell_destroy.link);
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        layers->trees[i] = wlr_scene_tree_create(windows->node.parent);
        if (!layers->trees[i]) {
            seaside_layer_scene_destroy(layers);
            return NULL;
        }
        struct wlr_scene_node *above =
            i < ZWLR_LAYER_SHELL_V1_LAYER_TOP ? &windows->node : &drag_icons->node;
        wlr_scene_node_place_below(&layers->trees[i]->node, above);
    }
    struct wlr_layer_shell_v1 *shell = wlr_layer_shell_v1_create(display, 5);
    if (!shell) {
        seaside_layer_scene_destroy(layers);
        return NULL;
    }
    layers->new_surface.notify = new_surface;
    layers->shell_destroy.notify = shell_destroy;
    wl_signal_add(&shell->events.new_surface, &layers->new_surface);
    wl_signal_add(&shell->events.destroy, &layers->shell_destroy);
    return layers;
}

void seaside_layer_scene_destroy(struct seaside_layer_scene *layers) {
    if (!layers) {
        return;
    }
    while (!wl_list_empty(&layers->outputs)) {
        struct layer_output *output = wl_container_of(layers->outputs.next, output, link);
        seaside_layer_scene_remove_output(layers, output->output);
    }
    if (layers->update) {
        wl_event_source_remove(layers->update);
    }
    wl_list_remove(&layers->new_surface.link);
    wl_list_remove(&layers->shell_destroy.link);
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        if (layers->trees[i]) {
            wlr_scene_node_destroy(&layers->trees[i]->node);
        }
    }
    free(layers);
}

void seaside_layer_scene_set_handler(struct seaside_layer_scene *layers,
                                     seaside_layer_output_fn focused_output,
                                     seaside_layer_changed_fn changed,
                                     seaside_layer_hidden_fn hidden, void *data) {
    layers->focused_output = focused_output;
    layers->changed = changed;
    layers->hidden = hidden;
    layers->data = data;
}

bool seaside_layer_scene_add_output(struct seaside_layer_scene *layers, struct wlr_output *output,
                                    struct seaside_scene_rect bounds) {
    struct layer_output *record = calloc(1, sizeof(*record));
    if (!record) {
        return false;
    }
    record->output = output;
    record->bounds = record->work = bounds;
    wl_list_insert(layers->outputs.prev, &record->link);
    return true;
}

void seaside_layer_scene_update_output(struct seaside_layer_scene *layers,
                                       struct wlr_output *output,
                                       struct seaside_scene_rect bounds) {
    struct layer_output *record = find_output(layers, output);
    if (record &&
        (record->bounds.x != bounds.x || record->bounds.y != bounds.y ||
         record->bounds.width != bounds.width || record->bounds.height != bounds.height)) {
        record->bounds = bounds;
        arrange_output(layers, record);
    }
}

void seaside_layer_scene_reserve_top(struct seaside_layer_scene *layers, struct wlr_output *output,
                                     int height, bool project_now) {
    struct layer_output *record = find_output(layers, output);
    if (record && record->top_reservation != height) {
        record->top_reservation = height;
        arrange_output(layers, record);
        if (project_now && layers->changed) {
            layers->changed(layers->data);
        } else {
            schedule_update(layers);
        }
    }
}

void seaside_layer_scene_remove_output(struct seaside_layer_scene *layers,
                                       struct wlr_output *output) {
    struct layer_output *record = find_output(layers, output);
    if (!record) {
        return;
    }
    struct layer_surface *surface, *next;
    wl_list_for_each_safe(surface, next, &layers->surfaces, link) {
        if (surface->output == record) {
            wlr_layer_surface_v1_destroy(surface->role);
        }
    }
    wl_list_remove(&record->link);
    free(record);
    schedule_update(layers);
}

struct seaside_scene_rect seaside_layer_scene_work_area(struct seaside_layer_scene *layers,
                                                        struct wlr_output *output,
                                                        bool fullscreen) {
    struct layer_output *record = find_output(layers, output);
    if (!record) {
        return (struct seaside_scene_rect){0};
    }
    if (record->fullscreen != fullscreen) {
        record->fullscreen = fullscreen;
        arrange_output(layers, record);
    }
    return record->work;
}

static struct layer_popup *find_popup(struct seaside_layer_scene *layers,
                                      struct wlr_surface *surface) {
    struct layer_popup *popup;
    wl_list_for_each(popup, &layers->popups, link) {
        if (popup->role->base->surface == surface) {
            return popup;
        }
    }
    return NULL;
}

static struct layer_surface *find_surface(struct seaside_layer_scene *layers,
                                          struct wlr_surface *surface) {
    struct layer_surface *layer;
    wl_list_for_each(layer, &layers->surfaces, link) {
        if (layer->role->surface == surface) {
            return layer;
        }
    }
    return NULL;
}

static struct layer_surface *find_owner(struct seaside_layer_scene *layers,
                                        struct wlr_surface *surface) {
    if (!surface) {
        return NULL;
    }
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    struct layer_popup *popup = find_popup(layers, root);
    return popup ? popup->layer : find_surface(layers, root);
}

static void popup_unconstrain(struct layer_popup *popup) {
    const struct seaside_scene_rect output = popup->layer->output->bounds;
    const struct wlr_scene_node *layer = &popup->layer->tree->node;
    struct wlr_box bounds = {coordinate((int64_t)output.x - layer->x),
                             coordinate((int64_t)output.y - layer->y), output.width, output.height};
    wlr_xdg_popup_unconstrain_from_box(popup->role, &bounds);
}

static void popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->role->seat && popup->layer->role->current.keyboard_interactive ==
                                 ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
        wlr_xdg_popup_destroy(popup->role);
        return;
    }
    if (popup->role->base->initial_commit) {
        popup_unconstrain(popup);
        wlr_xdg_surface_schedule_configure(popup->role->base);
    }
    const struct wlr_box position = popup->role->current.geometry;
    const struct wlr_box geometry = popup->role->base->geometry;
    wlr_scene_node_set_position(&popup->tree->node, position.x, position.y);
    wlr_scene_node_set_position(&popup->surface_tree->node, coordinate(-(int64_t)geometry.x),
                                coordinate(-(int64_t)geometry.y));
    struct layer_popup *child;
    wl_list_for_each(child, &popup->layer->owner->popups, link) {
        if (child->layer == popup->layer) {
            clip_tree(child->surface_tree, popup->layer->output->bounds);
        }
    }
}

static void popup_reposition(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_popup *popup = wl_container_of(listener, popup, reposition);
    popup_unconstrain(popup);
}

static void popup_tree_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_popup *popup = wl_container_of(listener, popup, tree_destroy);
    seaside_popup_focus_finish(&popup->focus);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->tree_destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct layer_popup *popup = wl_container_of(listener, popup, destroy);
    wlr_scene_node_destroy(&popup->tree->node);
}

bool seaside_layer_scene_add_popup(struct seaside_layer_scene *layers, struct wlr_xdg_popup *role) {
    struct layer_popup *parent = find_popup(layers, role->parent);
    struct layer_surface *layer = parent ? parent->layer : find_surface(layers, role->parent);
    if (!layer) {
        return false;
    }
    if (!layer->visible) {
        // Do not allow a popup grab on a layer that is hidden or unmapped.
        wlr_xdg_popup_destroy(role);
        return true;
    }
    struct layer_popup *popup = calloc(1, sizeof(*popup));
    if (popup) {
        popup->tree = wlr_scene_tree_create(parent ? parent->tree : layer->tree);
        if (popup->tree) {
            popup->surface_tree =
                wlr_scene_subsurface_tree_create(popup->tree, role->base->surface);
        }
    }
    if (!popup || !popup->tree || !popup->surface_tree) {
        if (popup && popup->tree) {
            wlr_scene_node_destroy(&popup->tree->node);
        }
        free(popup);
        wl_client_post_no_memory(wl_resource_get_client(role->resource));
        wlr_xdg_popup_destroy(role);
        return true;
    }
    popup->layer = layer;
    popup->role = role;
    seaside_popup_focus_init(&popup->focus, role);
    popup->commit.notify = popup_commit;
    popup->reposition.notify = popup_reposition;
    popup->destroy.notify = popup_destroy;
    popup->tree_destroy.notify = popup_tree_destroy;
    wl_list_insert(&layers->popups, &popup->link);
    wl_signal_add(&role->base->surface->events.commit, &popup->commit);
    wl_signal_add(&role->events.reposition, &popup->reposition);
    wl_signal_add(&role->events.destroy, &popup->destroy);
    wl_signal_add(&popup->tree->node.events.destroy, &popup->tree_destroy);
    return true;
}

bool seaside_layer_scene_at(struct seaside_layer_scene *layers, bool above_windows, double x,
                            double y, struct seaside_scene_target *target) {
    const unsigned first = above_windows ? ZWLR_LAYER_SHELL_V1_LAYER_TOP : 0;
    const unsigned end = above_windows ? LAYER_COUNT : ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    for (unsigned i = end; i > first; --i) {
        struct wlr_scene_node *node =
            wlr_scene_node_at(&layers->trees[i - 1]->node, x, y, &target->sx, &target->sy);
        if (node && node->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_surface *surface =
                wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
            if (surface) {
                target->surface = surface->surface;
                target->id = 0;
                return true;
            }
        }
    }
    return false;
}

struct pointer_position {
    struct seaside_scene_target *target;
    double x, y;
    bool found;
};

static void exact_position(struct wlr_surface *surface, int x, int y, void *data) {
    struct pointer_position *position = data;
    if (surface == position->target->surface) {
        position->target->id = 0;
        position->target->sx = position->x - x;
        position->target->sy = position->y - y;
        position->found = true;
    }
}

bool seaside_layer_scene_position(struct seaside_layer_scene *layers, double x, double y,
                                  struct seaside_scene_target *target) {
    struct layer_surface *layer = find_owner(layers, target->surface);
    if (!layer || !layer->visible) {
        return false;
    }
    struct pointer_position position = {
        .target = target, .x = x - layer->tree->node.x, .y = y - layer->tree->node.y};
    wlr_layer_surface_v1_for_each_surface(layer->role, exact_position, &position);
    return position.found;
}

void seaside_layer_scene_select(struct seaside_layer_scene *layers, struct wlr_surface *surface) {
    struct layer_surface *layer = find_owner(layers, surface);
    layers->selected = layer && layer->visible &&
                               layer->role->current.keyboard_interactive !=
                                   ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
                           ? layer
                           : NULL;
}

struct wlr_surface *seaside_layer_scene_keyboard_focus(struct seaside_layer_scene *layers,
                                                       struct wlr_output *focused_output) {
    for (unsigned i = LAYER_COUNT; i > ZWLR_LAYER_SHELL_V1_LAYER_TOP; --i) {
        struct layer_surface *layer, *candidate = NULL;
        wl_list_for_each_reverse(layer, &layers->surfaces, link) {
            if (!sibling_layer(layer, i - 1) || !layer->visible || !exclusive_keyboard(layer)) {
                continue;
            }
            if (!candidate || layer->output->output == focused_output) {
                candidate = layer;
            }
            if (layer->output->output == focused_output) {
                break;
            }
        }
        if (candidate) {
            return candidate->role->surface;
        }
    }
    if (layers->selected && layers->selected->output->output != focused_output) {
        layers->selected = NULL;
    }
    return layers->selected ? layers->selected->role->surface : NULL;
}

struct intersection_search {
    struct wlr_surface *surface;
    struct seaside_scene_rect viewport;
    bool intersects;
};

static void test_buffer_intersection(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    struct intersection_search *search = data;
    struct wlr_scene_surface *surface = wlr_scene_surface_try_from_buffer(buffer);
    if (surface && surface->surface == search->surface && buffer->buffer &&
        buffer->primary_output && wlr_scene_node_coords(&buffer->node, &x, &y)) {
        // A popup's scene tree can be anchored under a translated parent.
        // Compare its absolute buffer bounds to the output layout coordinates.
        const struct seaside_scene_rect viewport = search->viewport;
        const int width = buffer->dst_width, height = buffer->dst_height;
        search->intersects |= width > 0 && height > 0 && x < (int64_t)viewport.x + viewport.width &&
                              y < (int64_t)viewport.y + viewport.height &&
                              (int64_t)x + width > viewport.x && (int64_t)y + height > viewport.y;
    }
}

bool seaside_scene_surface_intersects(struct wlr_scene_tree *tree, struct wlr_surface *surface,
                                      struct seaside_scene_rect viewport) {
    struct intersection_search search = {.surface = surface, .viewport = viewport};
    wlr_scene_node_for_each_buffer(&tree->node, test_buffer_intersection, &search);
    return search.intersects;
}

struct wlr_output *seaside_layer_scene_visible_output(struct seaside_layer_scene *layers,
                                                      struct wlr_surface *surface) {
    struct layer_surface *layer = find_owner(layers, surface);
    return layer && layer->visible &&
                   seaside_scene_surface_intersects(layer->tree, surface, layer->output->bounds)
               ? layer->output->output
               : NULL;
}

bool seaside_layer_scene_has_overlay(const struct seaside_layer_scene *layers,
                                     const struct wlr_output *output) {
    const struct layer_surface *layer;
    wl_list_for_each(layer, &layers->surfaces, link) {
        if (layer->output->output == output && layer->visible &&
            layer->role->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
            return true;
        }
    }
    return false;
}

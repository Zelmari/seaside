#include "idle_inhibit.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>

struct seaside_idle_inhibit {
    struct wlr_idle_inhibit_manager_v1 *manager;
    struct wl_listener manager_destroy, new_inhibitor;
    struct wl_list watches;
    void (*changed)(void *);
    void *data;
};

struct inhibitor_watch {
    struct seaside_idle_inhibit *owner;
    struct wl_listener destroy, map, unmap;
    struct wl_list link;
};

static void remove_watch(struct inhibitor_watch *watch) {
    wl_list_remove(&watch->destroy.link);
    wl_list_remove(&watch->map.link);
    wl_list_remove(&watch->unmap.link);
    wl_list_remove(&watch->link);
    free(watch);
}

static void inhibitor_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct inhibitor_watch *watch = wl_container_of(listener, watch, destroy);
    struct seaside_idle_inhibit *owner = watch->owner;
    remove_watch(watch);
    // wlroots unlinks the inhibitor after delivering its destroy signal.
    // The owner defers its next walk until the event-loop callback returns.
    owner->changed(owner->data);
}

static void surface_mapped(struct wl_listener *listener, void *data) {
    (void)data;
    struct inhibitor_watch *watch = wl_container_of(listener, watch, map);
    watch->owner->changed(watch->owner->data);
}

static void surface_unmapped(struct wl_listener *listener, void *data) {
    (void)data;
    struct inhibitor_watch *watch = wl_container_of(listener, watch, unmap);
    watch->owner->changed(watch->owner->data);
}

static void new_inhibitor(struct wl_listener *listener, void *data) {
    struct seaside_idle_inhibit *owner = wl_container_of(listener, owner, new_inhibitor);
    struct wlr_idle_inhibitor_v1 *inhibitor = data;
    struct inhibitor_watch *watch = calloc(1, sizeof(*watch));
    if (!watch) {
        wl_resource_post_no_memory(inhibitor->resource);
        return;
    }
    watch->owner = owner;
    watch->destroy.notify = inhibitor_destroyed;
    watch->map.notify = surface_mapped;
    watch->unmap.notify = surface_unmapped;
    wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
    // A subsurface can change its mapped state without new desktop projection.
    wl_signal_add(&inhibitor->surface->events.map, &watch->map);
    wl_signal_add(&inhibitor->surface->events.unmap, &watch->unmap);
    wl_list_insert(&owner->watches, &watch->link);
    owner->changed(owner->data);
}

static void detach(struct seaside_idle_inhibit *owner) {
    if (owner->manager) {
        wl_list_remove(&owner->new_inhibitor.link);
        wl_list_remove(&owner->manager_destroy.link);
        owner->manager = NULL;
    }
    struct inhibitor_watch *watch, *next;
    wl_list_for_each_safe(watch, next, &owner->watches, link) {
        remove_watch(watch);
    }
}

static void manager_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_idle_inhibit *owner = wl_container_of(listener, owner, manager_destroy);
    detach(owner);
}

struct seaside_idle_inhibit *seaside_idle_inhibit_create(struct wl_display *display,
                                                         void (*changed)(void *), void *data) {
    struct seaside_idle_inhibit *owner = calloc(1, sizeof(*owner));
    if (!owner) {
        return NULL;
    }
    owner->manager = wlr_idle_inhibit_v1_create(display);
    if (!owner->manager) {
        free(owner);
        return NULL;
    }
    owner->changed = changed;
    owner->data = data;
    wl_list_init(&owner->watches);
    owner->new_inhibitor.notify = new_inhibitor;
    owner->manager_destroy.notify = manager_destroyed;
    wl_signal_add(&owner->manager->events.new_inhibitor, &owner->new_inhibitor);
    wl_signal_add(&owner->manager->events.destroy, &owner->manager_destroy);
    return owner;
}

void seaside_idle_inhibit_destroy(struct seaside_idle_inhibit *inhibit) {
    if (inhibit) {
        detach(inhibit);
        free(inhibit);
    }
}

void seaside_idle_inhibit_each(const struct seaside_idle_inhibit *inhibit,
                               void (*visit)(void *, struct wlr_surface *), void *data) {
    if (inhibit && inhibit->manager) {
        const struct wlr_idle_inhibitor_v1 *item;
        wl_list_for_each(item, &inhibit->manager->inhibitors, link) {
            visit(data, item->surface);
        }
    }
}

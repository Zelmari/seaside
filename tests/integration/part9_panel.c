#define _GNU_SOURCE
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

struct panel {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *shell;
    struct wl_surface *surface;
    bool closed, failed;
};

static void global(void *data, struct wl_registry *registry, uint32_t id, const char *interface,
                   uint32_t version) {
    struct panel *panel = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        panel->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        panel->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        panel->shell = wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface,
                                        version < 3 ? version : 3);
    }
}

static void removed(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data;
    (void)registry;
    (void)id;
}

static const struct wl_registry_listener registry_listener = {.global = global,
                                                              .global_remove = removed};

static struct wl_buffer *create_buffer(struct wl_shm *shm, uint32_t width, uint32_t height) {
    if (!width || !height || width > 8192 || height > 128) {
        return NULL;
    }
    const size_t size = (size_t)width * height * sizeof(uint32_t);
    const int fd = memfd_create("seaside-part9-panel", MFD_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }
    struct wl_buffer *buffer = NULL;
    if (ftruncate(fd, (off_t)size) == 0) {
        uint32_t *pixels = mmap(NULL, size, PROT_WRITE, MAP_SHARED, fd, 0);
        if (pixels != MAP_FAILED) {
            for (size_t i = 0; i < size / sizeof(*pixels); ++i) {
                pixels[i] = 0xff2a8060;
            }
            munmap(pixels, size);
            struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
            if (pool) {
                buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)width, (int32_t)height,
                                                   (int32_t)width * 4, WL_SHM_FORMAT_XRGB8888);
                wl_shm_pool_destroy(pool);
            }
        }
    }
    close(fd);
    return buffer;
}

static void configure(void *data, struct zwlr_layer_surface_v1 *layer, uint32_t serial,
                      uint32_t width, uint32_t height) {
    struct panel *panel = data;
    zwlr_layer_surface_v1_ack_configure(layer, serial);
    struct wl_buffer *buffer = create_buffer(panel->shm, width, height);
    if (!buffer) {
        panel->failed = true;
        return;
    }
    wl_surface_attach(panel->surface, buffer, 0, 0);
    wl_surface_damage(panel->surface, 0, 0, (int32_t)width, (int32_t)height);
    wl_surface_commit(panel->surface);
    wl_buffer_destroy(buffer);
    printf("panel %u %u\n", width, height);
    fflush(stdout);
}

static void closed(void *data, struct zwlr_layer_surface_v1 *layer) {
    (void)layer;
    ((struct panel *)data)->closed = true;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {.configure = configure,
                                                                     .closed = closed};

int main(void) {
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        return EXIT_FAILURE;
    }
    struct panel panel = {0};
    struct wl_registry *registry = wl_display_get_registry(display);
    struct zwlr_layer_surface_v1 *layer = NULL;
    if (registry && wl_registry_add_listener(registry, &registry_listener, &panel) == 0 &&
        wl_display_roundtrip(display) >= 0 && panel.compositor && panel.shm && panel.shell) {
        panel.surface = wl_compositor_create_surface(panel.compositor);
        if (panel.surface) {
            layer = zwlr_layer_shell_v1_get_layer_surface(panel.shell, panel.surface, NULL,
                                                          ZWLR_LAYER_SHELL_V1_LAYER_TOP,
                                                          "seaside-part9-test-panel");
        }
    }
    if (layer) {
        zwlr_layer_surface_v1_add_listener(layer, &layer_listener, &panel);
        zwlr_layer_surface_v1_set_size(layer, 0, 32);
        zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
        zwlr_layer_surface_v1_set_exclusive_zone(layer, 32);
        wl_surface_commit(panel.surface);
        while (!panel.closed && !panel.failed && wl_display_dispatch(display) >= 0) {
        }
        zwlr_layer_surface_v1_destroy(layer);
    } else {
        panel.failed = true;
    }
    if (panel.surface) {
        wl_surface_destroy(panel.surface);
    }
    if (panel.shell) {
        if (zwlr_layer_shell_v1_get_version(panel.shell) >=
            ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION) {
            zwlr_layer_shell_v1_destroy(panel.shell);
        } else {
            wl_proxy_destroy((struct wl_proxy *)panel.shell);
        }
    }
    if (panel.shm) {
        wl_shm_destroy(panel.shm);
    }
    if (panel.compositor) {
        wl_compositor_destroy(panel.compositor);
    }
    if (registry) {
        wl_registry_destroy(registry);
    }
    wl_display_disconnect(display);
    return panel.closed && !panel.failed ? EXIT_SUCCESS : EXIT_FAILURE;
}

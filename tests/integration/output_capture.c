#define _GNU_SOURCE

#include "output_capture.h"
#include "scene_bridge.h"

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/pixman.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>

enum { WIDTH = 48, HEIGHT = 42, CURSOR_SIZE = 4 };
static const uint32_t BACKGROUND_RGB = 0x00ff00;
static const uint32_t CURSOR_RGB = 0xff0000;
static const uint32_t SECOND_RGB = 0x0000ff;

struct listed_window {
    struct ext_foreign_toplevel_handle_v1 *handle;
    char title[600], app_id[600], identifier[64];
    bool done, closed;
};

struct fixture {
    struct wl_display *server;
    struct wl_display *client;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_output *output;
    struct wlr_output_cursor *cursor;
    struct seaside_output_capture *capture;
    struct wl_listener output_frame;
    struct wl_registry *registry;
    struct wl_shm *shm;
    struct wl_output *client_output;
    struct wl_output *other_output;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct xdg_wm_base *shell;
    struct ext_foreign_toplevel_list_v1 *list;
    struct ext_foreign_toplevel_image_capture_source_manager_v1 *window_manager;
    struct zxdg_output_manager_v1 *layout_manager;
    struct zxdg_output_v1 *layout_output;
    struct ext_output_image_capture_source_manager_v1 *output_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;
    struct zwlr_screencopy_manager_v1 *legacy_manager;
    struct ext_image_capture_source_v1 *source;
    uint32_t output_name;
    uint32_t other_name;
    int32_t layout_x, layout_y, layout_width, layout_height;
    bool layout_named, layout_done;
    bool render_failed;
    bool blue_output;
    bool empty_output_damage;
    struct listed_window windows[3];
};

struct capture_client {
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    uint32_t *pixels;
    size_t mapped;
    uint32_t width, height, format;
    bool constrained, stopped, ready, failed, full_damage, timestamp;
};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "capture check failed at %d: %s\n", __LINE__, #condition);             \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)

static bool transfer_messages(struct fixture *fixture) {
    if ((wl_display_flush(fixture->client) < 0 && errno != EAGAIN) ||
        wl_event_loop_dispatch(wl_display_get_event_loop(fixture->server), 5) < 0) {
        return false;
    }
    wl_display_flush_clients(fixture->server);
    if (wl_display_dispatch_pending(fixture->client) < 0 ||
        wl_display_prepare_read(fixture->client) < 0) {
        return false;
    }
    struct pollfd fd = {.fd = wl_display_get_fd(fixture->client), .events = POLLIN};
    if (poll(&fd, 1, 0) == 1 && (fd.revents & POLLIN)) {
        return wl_display_read_events(fixture->client) >= 0 &&
               wl_display_dispatch_pending(fixture->client) >= 0;
    }
    wl_display_cancel_read(fixture->client);
    return fd.revents == 0;
}

static void sync_done(void *data, struct wl_callback *callback, uint32_t serial) {
    (void)serial;
    *(bool *)data = true;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {.done = sync_done};

static bool pump_until(struct fixture *fixture, const bool *ready) {
    for (unsigned i = 0; !*ready && i < 400; ++i) {
        if (fixture->render_failed || !transfer_messages(fixture)) {
            return false;
        }
    }
    return *ready;
}

static bool sync_client(struct fixture *fixture) {
    bool ready = false;
    struct wl_callback *callback = wl_display_sync(fixture->client);
    if (!callback) {
        return false;
    }
    wl_callback_add_listener(callback, &sync_listener, &ready);
    if (!pump_until(fixture, &ready)) {
        wl_callback_destroy(callback);
        return false;
    }
    return true;
}

static void window_closed(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    ((struct listed_window *)data)->closed = true;
}

static void window_done(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    ((struct listed_window *)data)->done = true;
}

static void window_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                         const char *title) {
    (void)handle;
    struct listed_window *window = data;
    snprintf(window->title, sizeof(window->title), "%s", title);
}

static void window_identifier(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                              const char *identifier) {
    (void)handle;
    struct listed_window *window = data;
    snprintf(window->identifier, sizeof(window->identifier), "%s", identifier);
}

static void window_app_id(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
                          const char *app_id) {
    (void)handle;
    struct listed_window *window = data;
    snprintf(window->app_id, sizeof(window->app_id), "%s", app_id);
}

static const struct ext_foreign_toplevel_handle_v1_listener window_listener = {
    .closed = window_closed,
    .done = window_done,
    .title = window_title,
    .identifier = window_identifier,
    .app_id = window_app_id,
};

static void foreign_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *list,
                             struct ext_foreign_toplevel_handle_v1 *handle) {
    (void)list;
    struct fixture *fixture = data;
    for (size_t i = 0; i < sizeof(fixture->windows) / sizeof(fixture->windows[0]); ++i) {
        struct listed_window *window = &fixture->windows[i];
        if (!window->handle) {
            window->handle = handle;
            ext_foreign_toplevel_handle_v1_add_listener(handle, &window_listener, window);
            return;
        }
    }
    ext_foreign_toplevel_handle_v1_destroy(handle);
    fixture->render_failed = true;
}

static void foreign_finished(void *data, struct ext_foreign_toplevel_list_v1 *list) {
    (void)data;
    (void)list;
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
    .toplevel = foreign_toplevel,
    .finished = foreign_finished,
};

static void registry_add(void *data, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version) {
    (void)version;
    struct fixture *fixture = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        fixture->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!fixture->client_output) {
            fixture->client_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
            fixture->output_name = name;
        } else {
            fixture->other_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
            fixture->other_name = name;
        }
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        fixture->layout_manager =
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        fixture->output_manager = wl_registry_bind(
            registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        fixture->copy_manager =
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        fixture->legacy_manager =
            wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 3);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        fixture->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        fixture->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        fixture->shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        fixture->list =
            wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
        ext_foreign_toplevel_list_v1_add_listener(fixture->list, &list_listener, fixture);
    } else if (strcmp(interface,
                      ext_foreign_toplevel_image_capture_source_manager_v1_interface.name) == 0) {
        fixture->window_manager = wl_registry_bind(
            registry, name, &ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)registry;
    struct fixture *fixture = data;
    if (fixture->client_output && fixture->output_name == name) {
        if (fixture->layout_output) {
            zxdg_output_v1_destroy(fixture->layout_output);
            fixture->layout_output = NULL;
        }
        wl_output_release(fixture->client_output);
        fixture->client_output = NULL;
    } else if (fixture->other_output && fixture->other_name == name) {
        wl_output_release(fixture->other_output);
        fixture->other_output = NULL;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_add,
    .global_remove = registry_remove,
};

static void layout_position(void *data, struct zxdg_output_v1 *output, int32_t x, int32_t y) {
    (void)output;
    struct fixture *fixture = data;
    fixture->layout_x = x;
    fixture->layout_y = y;
}

static void layout_size(void *data, struct zxdg_output_v1 *output, int32_t width, int32_t height) {
    (void)output;
    struct fixture *fixture = data;
    fixture->layout_width = width;
    fixture->layout_height = height;
}

static void layout_done(void *data, struct zxdg_output_v1 *output) {
    (void)output;
    struct fixture *fixture = data;
    fixture->layout_done = true;
}

static void layout_name(void *data, struct zxdg_output_v1 *output, const char *name) {
    (void)output;
    struct fixture *fixture = data;
    fixture->layout_named = fixture->output && strcmp(name, fixture->output->name) == 0;
}

static void layout_description(void *data, struct zxdg_output_v1 *output, const char *description) {
    (void)data;
    (void)output;
    (void)description;
}

static const struct zxdg_output_v1_listener layout_listener = {
    .logical_position = layout_position,
    .logical_size = layout_size,
    .done = layout_done,
    .name = layout_name,
    .description = layout_description,
};

static bool verify_layout(struct fixture *fixture) {
    if (!fixture->layout_manager || !fixture->client_output || fixture->layout_output) {
        return false;
    }
    fixture->layout_done = false;
    fixture->layout_named = false;
    fixture->layout_x = -1;
    fixture->layout_y = -1;
    fixture->layout_width = 0;
    fixture->layout_height = 0;
    fixture->layout_output =
        zxdg_output_manager_v1_get_xdg_output(fixture->layout_manager, fixture->client_output);
    if (!fixture->layout_output) {
        return false;
    }
    zxdg_output_v1_add_listener(fixture->layout_output, &layout_listener, fixture);
    return pump_until(fixture, &fixture->layout_done) && fixture->layout_named &&
           fixture->layout_x == 0 && fixture->layout_y == 0 && fixture->layout_width == WIDTH &&
           fixture->layout_height == HEIGHT;
}

static void render_output(struct wl_listener *listener, void *data) {
    struct fixture *fixture = wl_container_of(listener, fixture, output_frame);
    struct wlr_output *output = data;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (fixture->empty_output_damage) {
        pixman_region32_t damage;
        pixman_region32_init(&damage);
        wlr_output_state_set_damage(&state, &damage);
        pixman_region32_fini(&damage);
    }
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(output, &state, NULL);
    if (!pass) {
        fixture->render_failed = true;
    } else {
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {.width = WIDTH, .height = HEIGHT},
                                           .color = fixture->blue_output
                                                        ? (struct wlr_render_color){.b = 1, .a = 1}
                                                        : (struct wlr_render_color){.g = 1, .a = 1},
                                       });
        fixture->render_failed =
            !wlr_render_pass_submit(pass) || !wlr_output_commit_state(output, &state);
    }
    wlr_output_state_finish(&state);
}

static bool stage_cursor(struct fixture *fixture) {
    struct wlr_buffer *buffer = wlr_allocator_create_buffer(
        fixture->allocator, CURSOR_SIZE, CURSOR_SIZE, &fixture->output->swapchain->format);
    if (!buffer) {
        return false;
    }
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(fixture->renderer, buffer, NULL);
    if (pass) {
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {.width = CURSOR_SIZE, .height = CURSOR_SIZE},
                                           .color = {.r = 1, .a = 1},
                                       });
    }
    const bool rendered = pass && wlr_render_pass_submit(pass);
    if (rendered) {
        fixture->cursor = wlr_output_cursor_create(fixture->output);
    }
    const bool ok = fixture->cursor && wlr_output_cursor_move(fixture->cursor, 12, 21) &&
                    wlr_output_cursor_set_buffer(fixture->cursor, buffer, 1, 1);
    wlr_buffer_drop(buffer);
    return ok && fixture->output->hardware_cursor == fixture->cursor;
}

static bool add_output(struct fixture *fixture) {
    fixture->output = wlr_headless_add_output(fixture->backend, WIDTH, HEIGHT);
    if (!fixture->output) {
        return false;
    }
    fixture->output_frame.notify = render_output;
    wl_signal_add(&fixture->output->events.frame, &fixture->output_frame);

    if (!wlr_output_init_render(fixture->output, fixture->allocator, fixture->renderer)) {
        return false;
    }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, WIDTH, HEIGHT, 60000);
    const bool committed = wlr_output_commit_state(fixture->output, &state);
    wlr_output_state_finish(&state);
    if (!committed || !stage_cursor(fixture)) {
        return false;
    }
    wlr_output_create_global(fixture->output, fixture->server);
    return true;
}

static void remove_output(struct fixture *fixture) {
    if (fixture->output) {
        wl_list_remove(&fixture->output_frame.link);
        wlr_output_destroy(fixture->output);
        fixture->output = NULL;
        fixture->cursor = NULL;
    }
}

static void on_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
                           uint32_t width, uint32_t height) {
    (void)session;
    struct capture_client *client = data;
    client->width = width;
    client->height = height;
}

static void on_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
                          uint32_t format) {
    (void)session;
    struct capture_client *client = data;
    client->format = format;
}

static void on_constraints(void *data, struct ext_image_copy_capture_session_v1 *session) {
    (void)session;
    struct capture_client *client = data;
    client->constrained = true;
}

static void on_stopped(void *data, struct ext_image_copy_capture_session_v1 *session) {
    (void)session;
    struct capture_client *client = data;
    client->stopped = true;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = on_buffer_size,
    .shm_format = on_shm_format,
    .done = on_constraints,
    .stopped = on_stopped,
};

static bool start_session(struct fixture *fixture, struct capture_client *client,
                          uint32_t options) {
    client->format = UINT32_MAX;
    client->session = ext_image_copy_capture_manager_v1_create_session(fixture->copy_manager,
                                                                       fixture->source, options);
    if (!client->session) {
        return false;
    }
    ext_image_copy_capture_session_v1_add_listener(client->session, &session_listener, client);
    return true;
}

static bool create_buffer(struct fixture *fixture, struct capture_client *client) {
    if (client->width == 0 || client->height == 0 || client->width > 128 || client->height > 128 ||
        (client->format != WL_SHM_FORMAT_XRGB8888 && client->format != WL_SHM_FORMAT_ARGB8888)) {
        return false;
    }
    const int32_t width = (int32_t)client->width;
    const int32_t height = (int32_t)client->height;
    const int32_t stride = width * (int32_t)sizeof(uint32_t);
    const int32_t size = stride * height;
    int fd = memfd_create("seaside-capture-fixture", MFD_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return false;
    }
    void *mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        close(fd);
        return false;
    }
    client->pixels = mapping;
    client->mapped = size;
    struct wl_shm_pool *pool = wl_shm_create_pool(fixture->shm, fd, size);
    close(fd);
    if (!pool) {
        return false;
    }
    client->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, client->format);
    wl_shm_pool_destroy(pool);
    return client->buffer != NULL;
}

static void on_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                         uint32_t transform) {
    (void)frame;
    struct capture_client *client = data;
    client->failed |= transform != WL_OUTPUT_TRANSFORM_NORMAL;
}

static void on_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame, int32_t x,
                      int32_t y, int32_t width, int32_t height) {
    (void)frame;
    struct capture_client *client = data;
    client->full_damage |=
        x == 0 && y == 0 && width == (int32_t)client->width && height == (int32_t)client->height;
}

static void on_time(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t sec_hi,
                    uint32_t sec_lo, uint32_t nsec) {
    (void)frame;
    struct capture_client *client = data;
    client->timestamp = (sec_hi || sec_lo || nsec) && nsec < 1000000000U;
}

static void on_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame) {
    struct capture_client *client = data;
    client->ready = true;
    client->frame = NULL;
    ext_image_copy_capture_frame_v1_destroy(frame);
}

static void on_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason) {
    struct capture_client *client = data;
    fprintf(stderr, "capture frame failed with reason %u\n", reason);
    client->failed = true;
    client->ready = true;
    client->frame = NULL;
    ext_image_copy_capture_frame_v1_destroy(frame);
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = on_transform,
    .damage = on_damage,
    .presentation_time = on_time,
    .ready = on_ready,
    .failed = on_failed,
};

static bool request_capture(struct capture_client *client) {
    client->ready = false;
    client->full_damage = false;
    client->timestamp = false;
    client->frame = ext_image_copy_capture_session_v1_create_frame(client->session);
    if (!client->frame) {
        return false;
    }
    ext_image_copy_capture_frame_v1_add_listener(client->frame, &frame_listener, client);
    ext_image_copy_capture_frame_v1_attach_buffer(client->frame, client->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(client->frame, 0, 0, (int32_t)client->width,
                                                  (int32_t)client->height);
    ext_image_copy_capture_frame_v1_capture(client->frame);
    return true;
}

static bool check_pixels(const struct capture_client *client, bool cursors, int x, int y) {
    if (client->failed || !client->full_damage || !client->timestamp) {
        return false;
    }
    for (int row = 0; row < HEIGHT; ++row) {
        for (int column = 0; column < WIDTH; ++column) {
            const bool inside = cursors && row >= y - 1 && row < y - 1 + CURSOR_SIZE &&
                                column >= x - 1 && column < x - 1 + CURSOR_SIZE;
            const uint32_t want = inside ? CURSOR_RGB : BACKGROUND_RGB;
            const uint32_t actual = client->pixels[row * WIDTH + column] & 0xffffff;
            if (actual != want) {
                fprintf(stderr, "capture pixel at (%d,%d) expected %06x, got %06x\n", column, row,
                        want, actual);
                return false;
            }
        }
    }
    return true;
}

static void release_buffer(struct capture_client *client) {
    if (client->buffer) {
        wl_buffer_destroy(client->buffer);
        client->buffer = NULL;
    }
    if (client->pixels) {
        munmap(client->pixels, client->mapped);
        client->pixels = NULL;
    }
}

static void finish_session(struct capture_client *client) {
    if (client->frame) {
        ext_image_copy_capture_frame_v1_destroy(client->frame);
    }
    if (client->session) {
        ext_image_copy_capture_session_v1_destroy(client->session);
    }
    release_buffer(client);
    *client = (struct capture_client){0};
}

static bool capture_pair(struct fixture *fixture, struct capture_client *hidden,
                         struct capture_client *embedded, int x, int y) {
    return request_capture(hidden) && request_capture(embedded) &&
           pump_until(fixture, &hidden->ready) && pump_until(fixture, &embedded->ready) &&
           check_pixels(hidden, false, x, y) && check_pixels(embedded, true, x, y) &&
           fixture->output->hardware_cursor == fixture->cursor &&
           fixture->output->software_cursor_locks == 0 && fixture->output->attach_render_locks == 1;
}

static bool plain_frame(const struct capture_client *client, uint32_t rgb) {
    if (!client->ready || client->failed || !client->timestamp || !client->full_damage) {
        return false;
    }
    for (size_t i = 0; i < (size_t)client->width * client->height; ++i) {
        if ((client->pixels[i] & 0xffffff) != rgb) {
            return false;
        }
    }
    return true;
}

static bool check_two_outputs(struct fixture *fixture) {
    bool passed = false;
    struct fixture second = {.server = fixture->server,
                             .backend = fixture->backend,
                             .allocator = fixture->allocator,
                             .renderer = fixture->renderer,
                             .blue_output = true};
    struct capture_client first_frames = {0}, second_frames = {0};
    struct ext_image_capture_source_v1 *first_source = NULL;
    CHECK(add_output(&second));
    CHECK(seaside_output_capture_add_output(fixture->capture, second.output, WIDTH, -3));
    CHECK(sync_client(fixture) && sync_client(fixture) && fixture->other_output);
    first_source = fixture->source;
    CHECK(start_session(fixture, &first_frames, 0));
    fixture->source = ext_output_image_capture_source_manager_v1_create_source(
        fixture->output_manager, fixture->other_output);
    CHECK(fixture->source && start_session(fixture, &second_frames, 0));
    CHECK(pump_until(fixture, &first_frames.constrained) && create_buffer(fixture, &first_frames));
    CHECK(pump_until(fixture, &second_frames.constrained) &&
          create_buffer(fixture, &second_frames));
    CHECK(request_capture(&first_frames) && request_capture(&second_frames));
    CHECK(pump_until(fixture, &first_frames.ready) && plain_frame(&first_frames, BACKGROUND_RGB));
    CHECK(pump_until(fixture, &second_frames.ready) && plain_frame(&second_frames, SECOND_RGB));
    remove_output(&second);
    CHECK(pump_until(fixture, &second_frames.stopped) && !first_frames.stopped);
    CHECK(request_capture(&first_frames) && pump_until(fixture, &first_frames.ready) &&
          plain_frame(&first_frames, BACKGROUND_RGB));
    passed = !second.render_failed;

done:
    finish_session(&first_frames);
    finish_session(&second_frames);
    if (first_source) {
        if (fixture->source) {
            ext_image_capture_source_v1_destroy(fixture->source);
        }
        fixture->source = first_source;
    }
    remove_output(&second);
    return passed;
}

struct painted_window {
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    bool configured;
};

static void xdg_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    ((struct painted_window *)data)->configured = true;
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_listener = {.configure = xdg_configure};

static void top_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                          struct wl_array *states) {
    (void)data;
    (void)toplevel;
    (void)width;
    (void)height;
    (void)states;
}

static const struct xdg_toplevel_listener top_listener = {.configure = top_configure};

static bool paint_pattern(struct fixture *fixture, struct wl_surface *surface, uint32_t width,
                          uint32_t height, uint32_t rgb, bool quadrants) {
    struct capture_client paint = {
        .width = width, .height = height, .format = WL_SHM_FORMAT_XRGB8888};
    if (!create_buffer(fixture, &paint)) {
        release_buffer(&paint);
        return false;
    }
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            uint32_t value = rgb;
            if (quadrants) {
                value = x < width / 2 ? (y < height / 2 ? CURSOR_RGB : SECOND_RGB)
                                      : (y < height / 2 ? BACKGROUND_RGB : 0xffff00);
            }
            paint.pixels[y * width + x] = value;
        }
    }
    wl_surface_attach(surface, paint.buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, (int32_t)width, (int32_t)height);
    wl_surface_commit(surface);
    release_buffer(&paint);
    return true;
}

static bool paint(struct fixture *fixture, struct wl_surface *surface, uint32_t width,
                  uint32_t height, uint32_t rgb) {
    return paint_pattern(fixture, surface, width, height, rgb, false);
}

static bool map_painted_window(struct fixture *fixture, struct painted_window *window,
                               const char *name, uint32_t rgb) {
    window->surface = wl_compositor_create_surface(fixture->compositor);
    if (!window->surface) {
        return false;
    }
    window->xdg = xdg_wm_base_get_xdg_surface(fixture->shell, window->surface);
    if (!window->xdg) {
        return false;
    }
    window->toplevel = xdg_surface_get_toplevel(window->xdg);
    if (!window->toplevel) {
        return false;
    }
    xdg_surface_add_listener(window->xdg, &xdg_listener, window);
    xdg_toplevel_add_listener(window->toplevel, &top_listener, window);
    xdg_toplevel_set_title(window->toplevel, name);
    xdg_toplevel_set_app_id(window->toplevel, "seaside-capture-fixture");
    wl_surface_commit(window->surface);
    return pump_until(fixture, &window->configured) && paint(fixture, window->surface, 20, 14, rgb);
}

static void close_painted_window(struct painted_window *window) {
    if (window->toplevel) {
        xdg_toplevel_destroy(window->toplevel);
    }
    if (window->xdg) {
        xdg_surface_destroy(window->xdg);
    }
    if (window->surface) {
        wl_surface_destroy(window->surface);
    }
    *window = (struct painted_window){0};
}

struct window_placement {
    struct seaside_scene_bridge *scene;
    uint64_t next_id;
};

static void place_window(struct seaside_scene_bridge *scene, uint64_t id, int x, int y,
                         bool visible) {
    const struct seaside_scene_window_placement placement = {
        .geometry = {x, y, 20, 14},
        .output = {0, 0, WIDTH, HEIGHT},
        .clip = {0, 0, WIDTH, HEIGHT},
        .stacking = id,
        .visible = visible,
    };
    seaside_scene_place_window(scene, id, &placement);
}

static uint64_t window_event(void *data, const struct seaside_scene_window_event *event) {
    struct window_placement *placement = data;
    if (event->kind == SEASIDE_WINDOW_CREATED) {
        return ++placement->next_id;
    }
    if (event->kind == SEASIDE_WINDOW_MAPPED) {
        place_window(placement->scene, event->id, 2, 3, true);
    }
    return 0;
}

static bool pump_for(struct fixture *fixture, long milliseconds) {
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return false;
    }
    do {
        if (!transfer_messages(fixture) || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return false;
        }
    } while ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 <
             milliseconds);
    return true;
}

static void legacy_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
                          uint32_t width, uint32_t height, uint32_t stride) {
    (void)frame;
    struct capture_client *client = data;
    client->format = format;
    client->width = width;
    client->height = height;
    client->failed |= format != WL_SHM_FORMAT_XRGB8888 || stride != width * sizeof(uint32_t);
}

static void legacy_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
    (void)frame;
    struct capture_client *client = data;
    client->failed |= flags != 0;
}

static void legacy_ready(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t sec_hi,
                         uint32_t sec_lo, uint32_t nsec) {
    (void)frame;
    struct capture_client *client = data;
    client->timestamp = (sec_hi || sec_lo || nsec) && nsec < 1000000000U;
    client->ready = true;
}

static void legacy_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)frame;
    struct capture_client *client = data;
    client->failed = true;
    client->ready = true;
}

static void legacy_damage(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t x,
                          uint32_t y, uint32_t width, uint32_t height) {
    (void)frame;
    struct capture_client *client = data;
    client->full_damage = x == 0 && y == 0 && width == client->width && height == client->height;
}

static void legacy_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
                          uint32_t width, uint32_t height) {
    (void)frame;
    (void)format;
    (void)width;
    (void)height;
    struct capture_client *client = data;
    client->failed = true;
}

static void legacy_buffers_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)frame;
    struct capture_client *client = data;
    client->constrained = true;
}

static const struct zwlr_screencopy_frame_v1_listener legacy_listener = {
    .buffer = legacy_buffer,
    .flags = legacy_flags,
    .ready = legacy_ready,
    .failed = legacy_failed,
    .damage = legacy_damage,
    .linux_dmabuf = legacy_dmabuf,
    .buffer_done = legacy_buffers_done,
};

static struct zwlr_screencopy_frame_v1 *legacy_region(struct fixture *fixture,
                                                      struct capture_client *client) {
    client->constrained = client->ready = client->failed = client->full_damage = false;
    struct zwlr_screencopy_frame_v1 *frame = zwlr_screencopy_manager_v1_capture_output_region(
        fixture->legacy_manager, 1, fixture->client_output, 23, 28, 8, 8);
    if (frame) {
        zwlr_screencopy_frame_v1_add_listener(frame, &legacy_listener, client);
    }
    return frame;
}

static bool check_legacy_capture(struct fixture *fixture) {
    bool passed = false;
    struct capture_client buffer = {0};
    struct zwlr_screencopy_frame_v1 *frame = NULL;
    CHECK(fixture->legacy_manager && wlr_output_cursor_move(fixture->cursor, 25, 30));
    fixture->empty_output_damage = true;
    frame = legacy_region(fixture, &buffer);
    CHECK(frame && pump_until(fixture, &buffer.constrained));
    CHECK(!buffer.failed && buffer.width == 8 && buffer.height == 8 &&
          create_buffer(fixture, &buffer));
    zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.buffer);
    CHECK(pump_until(fixture, &buffer.ready) && !buffer.failed && buffer.full_damage &&
          buffer.timestamp);
    CHECK((buffer.pixels[9] & 0xffffff) == CURSOR_RGB &&
          (buffer.pixels[0] & 0xffffff) == BACKGROUND_RGB &&
          (buffer.pixels[63] & 0xffffff) == BACKGROUND_RGB);
    zwlr_screencopy_frame_v1_destroy(frame);

    frame = legacy_region(fixture, &buffer);
    CHECK(frame && pump_until(fixture, &buffer.constrained));
    zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.buffer);
    CHECK(pump_for(fixture, 50) && !buffer.ready && fixture->output->attach_render_locks == 1);
    CHECK(wlr_output_cursor_move(fixture->cursor, 29, 33));
    seaside_output_capture_cursor_changed(fixture->capture);
    CHECK(pump_until(fixture, &buffer.ready) && !buffer.failed && buffer.full_damage &&
          (buffer.pixels[9] & 0xffffff) == BACKGROUND_RGB &&
          (buffer.pixels[37] & 0xffffff) == CURSOR_RGB);
    CHECK(fixture->output->attach_render_locks == 0 &&
          fixture->output->hardware_cursor == fixture->cursor &&
          fixture->output->software_cursor_locks == 0);
    zwlr_screencopy_frame_v1_destroy(frame);

    frame = legacy_region(fixture, &buffer);
    CHECK(frame && pump_until(fixture, &buffer.constrained));
    zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.buffer);
    CHECK(sync_client(fixture) && fixture->output->attach_render_locks == 1);
    zwlr_screencopy_frame_v1_destroy(frame);
    frame = NULL;
    CHECK(sync_client(fixture) && fixture->output->attach_render_locks == 0 && !buffer.ready);
    passed = true;

done:
    fixture->empty_output_damage = false;
    if (frame) {
        zwlr_screencopy_frame_v1_destroy(frame);
    }
    release_buffer(&buffer);
    return passed;
}

static bool check_selected_window(struct fixture *fixture) {
    bool passed = false;
    struct painted_window selected = {0}, occluding = {0};
    struct seaside_scene_bridge *scene = seaside_scene_create(fixture->server);
    struct window_placement placement = {.scene = scene};
    struct capture_client window = {0}, painted = {0}, stale = {0};
    struct wl_surface *child = NULL;
    struct wl_subsurface *subsurface = NULL;
    struct ext_image_capture_source_v1 *selected_source = NULL, *stale_source = NULL;
    struct wl_callback *idle_callback = NULL;
    bool idle_frame = false, hidden_frame = false;
    CHECK(scene && wlr_compositor_create(fixture->server, 6, fixture->renderer));
    CHECK(wlr_subcompositor_create(fixture->server));
    CHECK(
        seaside_scene_enable_window_capture(scene, fixture->allocator, fixture->renderer,
                                            seaside_output_capture_copy_manager(fixture->capture)));
    wl_list_remove(&fixture->output_frame.link);
    wl_list_init(&fixture->output_frame.link);
    CHECK(seaside_scene_add_output(scene, fixture->output));
    CHECK(seaside_scene_place_output(scene, fixture->output, 0, 0));
    seaside_scene_set_window_handler(scene, window_event, &placement);
    CHECK(!seaside_scene_window_capture_client_ready(scene, getpid(), getuid()));
    CHECK(sync_client(fixture) && sync_client(fixture));
    CHECK(fixture->compositor && fixture->subcompositor && fixture->shell &&
          fixture->window_manager && fixture->list);
    CHECK(seaside_scene_window_capture_client_ready(scene, getpid(), getuid()));
    CHECK(!seaside_scene_window_capture_client_ready(scene, getpid(), getuid() + 1));
    CHECK(!seaside_scene_window_capture_client_ready(scene, getpid() + 1, getuid()));

    const uint32_t selected_rgb = 0xa84819;
    CHECK(map_painted_window(fixture, &selected, "selected", selected_rgb));
    CHECK(map_painted_window(fixture, &occluding, "overlap", SECOND_RGB));
    CHECK(pump_until(fixture, &fixture->windows[0].done) &&
          pump_until(fixture, &fixture->windows[1].done));
    CHECK(placement.next_id == 2 && strcmp(fixture->windows[0].title, "selected") == 0 &&
          strcmp(fixture->windows[1].title, "overlap") == 0 &&
          strcmp(fixture->windows[0].identifier, fixture->windows[1].identifier) != 0);
    char identifier[sizeof(fixture->windows[0].identifier)];
    memcpy(identifier, fixture->windows[0].identifier, sizeof(identifier));
    fixture->windows[0].done = false;
    xdg_toplevel_set_title(selected.toplevel, "two\nlines\x1b\xe2\x80\xa8 \xce\xbb");
    xdg_toplevel_set_app_id(selected.toplevel, "app\t\xe2\x80\xaeid");
    CHECK(pump_until(fixture, &fixture->windows[0].done) && sync_client(fixture));
    CHECK(strcmp(fixture->windows[0].title, "two lines   \xce\xbb") == 0);
    CHECK(strcmp(fixture->windows[0].app_id, "app  id") == 0);
    char long_title[900];
    memset(long_title, 'x', sizeof(long_title) - 1);
    memcpy(long_title + 511, "\xe2\x82\xac", 3);
    long_title[sizeof(long_title) - 1] = '\0';
    fixture->windows[0].done = false;
    xdg_toplevel_set_title(selected.toplevel, long_title);
    CHECK(pump_until(fixture, &fixture->windows[0].done));
    CHECK(strlen(fixture->windows[0].title) == 511 &&
          strcmp(fixture->windows[0].identifier, identifier) == 0);

    selected_source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        fixture->window_manager, fixture->windows[0].handle);
    CHECK(selected_source);
    ext_image_capture_source_v1_destroy(fixture->source);
    fixture->source = selected_source;
    CHECK(start_session(fixture, &window, 0));
    CHECK(pump_until(fixture, &window.constrained) && window.width == 20 && window.height == 14 &&
          create_buffer(fixture, &window));
    CHECK(request_capture(&window) && pump_until(fixture, &window.ready) &&
          plain_frame(&window, selected_rgb));
    CHECK(
        start_session(fixture, &painted, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS));
    CHECK(pump_until(fixture, &painted.constrained) && create_buffer(fixture, &painted));
    CHECK(wlr_output_cursor_move(fixture->cursor, 8, 9));
    seaside_scene_capture_pointer(scene, 8, 9);
    CHECK(request_capture(&painted) && pump_until(fixture, &painted.ready) &&
          plain_frame(&painted, selected_rgb));
    // Once the pointer is over the selected view, only the opted-in session
    // receives the private cursor sprite. The physical cursor remains in use.
    place_window(scene, 2, 24, 19, true);
    CHECK(wlr_output_cursor_move(fixture->cursor, 9, 10));
    seaside_scene_capture_pointer(scene, 9, 10);
    CHECK(request_capture(&painted) && request_capture(&window));
    CHECK(pump_until(fixture, &painted.ready) && !painted.failed &&
          (painted.pixels[6 * painted.width + 6] & 0xffffff) == CURSOR_RGB &&
          (painted.pixels[5 * painted.width + 5] & 0xffffff) == selected_rgb);
    CHECK(pump_until(fixture, &window.ready) && plain_frame(&window, selected_rgb) &&
          fixture->output->hardware_cursor == fixture->cursor &&
          fixture->output->software_cursor_locks == 0);
    finish_session(&painted);
    CHECK(sync_client(fixture));

    // Moving and then hiding the selected scene view must not change its source.
    place_window(scene, 1, 25, 9, true);
    place_window(scene, 1, 25, 9, false);
    CHECK(request_capture(&window));
    CHECK(paint(fixture, occluding.surface, 20, 14, BACKGROUND_RGB));
    CHECK(pump_for(fixture, 45) && !window.ready);
    struct wl_callback *callback = wl_surface_frame(selected.surface);
    CHECK(callback);
    wl_callback_add_listener(callback, &sync_listener, &hidden_frame);
    wl_surface_commit(selected.surface);
    CHECK(pump_until(fixture, &hidden_frame));
    CHECK(paint(fixture, selected.surface, 20, 14, SECOND_RGB));
    CHECK(pump_until(fixture, &window.ready) && plain_frame(&window, SECOND_RGB));

    window.constrained = false;
    CHECK(paint(fixture, selected.surface, 27, 15, BACKGROUND_RGB));
    CHECK(pump_until(fixture, &window.constrained) && window.width == 27 && window.height == 15);
    release_buffer(&window);
    CHECK(create_buffer(fixture, &window) && request_capture(&window) &&
          pump_until(fixture, &window.ready) && plain_frame(&window, BACKGROUND_RGB));
    CHECK(!fixture->windows[0].closed && !fixture->windows[2].handle);

    child = wl_compositor_create_surface(fixture->compositor);
    CHECK(child);
    subsurface = wl_subcompositor_get_subsurface(fixture->subcompositor, child, selected.surface);
    CHECK(subsurface);
    wl_subsurface_set_position(subsurface, 3, 4);
    CHECK(request_capture(&window) && paint(fixture, child, 6, 5, CURSOR_RGB));
    wl_surface_commit(selected.surface);
    CHECK(pump_until(fixture, &window.ready) && !window.failed && window.full_damage &&
          (window.pixels[4 * window.width + 3] & 0xffffff) == CURSOR_RGB &&
          (window.pixels[0] & 0xffffff) == BACKGROUND_RGB);
    // The raw 4×6 texture becomes a 6×4 surface. Its right and bottom edges
    // lie beyond the explicitly declared window, so only logical 2×3 remains.
    xdg_surface_set_window_geometry(selected.xdg, 0, 0, 27, 15);
    wl_subsurface_set_position(subsurface, 25, 12);
    wl_surface_set_buffer_transform(child, WL_OUTPUT_TRANSFORM_90);
    CHECK(request_capture(&window) && paint_pattern(fixture, child, 4, 6, 0, true));
    wl_surface_commit(selected.surface);
    CHECK(pump_until(fixture, &window.ready) && !window.failed && window.full_damage &&
          (window.pixels[12 * window.width + 25] & 0xffffff) == SECOND_RGB &&
          (window.pixels[14 * window.width + 25] & 0xffffff) == 0xffff00 &&
          (window.pixels[12 * window.width + 24] & 0xffffff) == BACKGROUND_RGB);
    wl_subsurface_destroy(subsurface);
    subsurface = NULL;
    wl_surface_destroy(child);
    child = NULL;

    finish_session(&window);
    CHECK(sync_client(fixture));
    idle_callback = wl_surface_frame(selected.surface);
    CHECK(idle_callback);
    wl_callback_add_listener(idle_callback, &sync_listener, &idle_frame);
    wl_surface_commit(selected.surface);
    CHECK(pump_for(fixture, 55) && !idle_frame);
    wl_callback_destroy(idle_callback);
    idle_callback = NULL;

    CHECK(start_session(fixture, &window, 0) && pump_until(fixture, &window.constrained) &&
          create_buffer(fixture, &window));
    CHECK(request_capture(&window) && pump_until(fixture, &window.ready));
    CHECK(request_capture(&window));
    close_painted_window(&selected);
    CHECK(pump_until(fixture, &window.stopped) && window.failed && fixture->windows[0].closed &&
          !fixture->windows[1].closed);
    stale_source = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        fixture->window_manager, fixture->windows[0].handle);
    CHECK(stale_source);
    fixture->source = stale_source;
    CHECK(start_session(fixture, &stale, 0) && pump_until(fixture, &stale.stopped) &&
          !stale.constrained);
    passed = true;

done:
    if (idle_callback && !idle_frame) {
        wl_callback_destroy(idle_callback);
    }
    if (subsurface) {
        wl_subsurface_destroy(subsurface);
    }
    if (child) {
        wl_surface_destroy(child);
    }
    finish_session(&window);
    finish_session(&painted);
    finish_session(&stale);
    if (stale_source) {
        ext_image_capture_source_v1_destroy(stale_source);
    }
    if (selected_source) {
        ext_image_capture_source_v1_destroy(selected_source);
        fixture->source = NULL;
    }
    close_painted_window(&selected);
    close_painted_window(&occluding);
    seaside_scene_destroy(scene);
    return passed;
}

int main(int argc, char **argv) {
    const bool part7 = argc == 2 && strcmp(argv[1], "part7") == 0;
    int result = 1;
    struct fixture fixture = {0};
    struct capture_client hidden = {0}, embedded = {0}, resumed = {0};

    fixture.server = wl_display_create();
    CHECK(fixture.server);
    fixture.backend = wlr_headless_backend_create(wl_display_get_event_loop(fixture.server));
    CHECK(fixture.backend);
    fixture.renderer = wlr_pixman_renderer_create();
    CHECK(fixture.renderer);
    fixture.allocator = wlr_allocator_autocreate(fixture.backend, fixture.renderer);
    CHECK(fixture.allocator);
    CHECK(wlr_renderer_init_wl_shm(fixture.renderer, fixture.server));
    CHECK(wlr_backend_start(fixture.backend));
    CHECK(add_output(&fixture));
    fixture.capture = seaside_output_capture_create(fixture.server);
    CHECK(fixture.capture &&
          seaside_output_capture_add_output(fixture.capture, fixture.output, 0, 0));
    CHECK(!seaside_output_capture_client_ready(fixture.capture, getpid(), getuid()));

    int socket_fds[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socket_fds) == 0);
    struct wl_client *server_client = wl_client_create(fixture.server, socket_fds[0]);
    if (!server_client) {
        close(socket_fds[0]);
        close(socket_fds[1]);
    }
    CHECK(server_client);
    fixture.client = wl_display_connect_to_fd(socket_fds[1]);
    CHECK(fixture.client);

    fixture.registry = wl_display_get_registry(fixture.client);
    CHECK(fixture.registry);
    wl_registry_add_listener(fixture.registry, &registry_listener, &fixture);
    CHECK(sync_client(&fixture) && sync_client(&fixture));
    CHECK(fixture.shm && fixture.client_output && fixture.copy_manager && fixture.output_manager);
    CHECK(verify_layout(&fixture));
    CHECK(seaside_output_capture_client_ready(fixture.capture, getpid(), getuid()));
    CHECK(!seaside_output_capture_client_ready(fixture.capture, getpid(), getuid() + 1));

    fixture.source = ext_output_image_capture_source_manager_v1_create_source(
        fixture.output_manager, fixture.client_output);
    CHECK(fixture.source);
    CHECK(start_session(&fixture, &hidden, 0));
    CHECK(start_session(&fixture, &embedded,
                        EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS));
    CHECK(pump_until(&fixture, &hidden.constrained) && pump_until(&fixture, &embedded.constrained));
    CHECK(create_buffer(&fixture, &hidden) && create_buffer(&fixture, &embedded));
    CHECK(capture_pair(&fixture, &hidden, &embedded, 12, 21));
    CHECK(wlr_output_cursor_move(fixture.cursor, 25, 30));
    CHECK(capture_pair(&fixture, &hidden, &embedded, 25, 30));
    CHECK(!part7 || check_two_outputs(&fixture));

    remove_output(&fixture);
    CHECK(pump_until(&fixture, &hidden.stopped) && pump_until(&fixture, &embedded.stopped));
    CHECK(!seaside_output_capture_client_ready(fixture.capture, getpid(), getuid()));
    finish_session(&hidden);
    finish_session(&embedded);
    ext_image_capture_source_v1_destroy(fixture.source);
    fixture.source = NULL;

    CHECK(add_output(&fixture)); // A recovered output uses the existing client manager bindings.
    CHECK(seaside_output_capture_add_output(fixture.capture, fixture.output, 0, 0));
    CHECK(sync_client(&fixture) && sync_client(&fixture));
    CHECK(verify_layout(&fixture));
    CHECK(seaside_output_capture_client_ready(fixture.capture, getpid(), getuid()));
    fixture.source = ext_output_image_capture_source_manager_v1_create_source(
        fixture.output_manager, fixture.client_output);
    CHECK(fixture.source);
    CHECK(
        start_session(&fixture, &resumed, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS));
    CHECK(pump_until(&fixture, &resumed.constrained) && create_buffer(&fixture, &resumed));
    CHECK(request_capture(&resumed) && pump_until(&fixture, &resumed.ready));
    CHECK(check_pixels(&resumed, true, 12, 21));
    CHECK(fixture.output->hardware_cursor == fixture.cursor);
    CHECK(fixture.output->software_cursor_locks == 0);
    finish_session(&resumed);
    CHECK(sync_client(&fixture) && fixture.output->attach_render_locks == 0);
    CHECK(!part7 || check_legacy_capture(&fixture));
    CHECK(!part7 || check_selected_window(&fixture));
    puts("output capture PASS hidden=2 embedded=3 moved=yes reattached=yes layout=yes "
         "cursor-locks=0");
    if (part7) {
        puts("selected window capture PASS isolated-pixels=no-overlap hidden-updates=yes "
             "resized=yes subsurface=yes idle-callbacks=0 loss=stopped two-outputs=yes "
             "paint-cursors=verified");
    }
    result = 0;

done:
    finish_session(&hidden);
    finish_session(&embedded);
    finish_session(&resumed);
    if (fixture.source) {
        ext_image_capture_source_v1_destroy(fixture.source);
    }
    if (fixture.copy_manager) {
        ext_image_copy_capture_manager_v1_destroy(fixture.copy_manager);
    }
    if (fixture.legacy_manager) {
        zwlr_screencopy_manager_v1_destroy(fixture.legacy_manager);
    }
    if (fixture.output_manager) {
        ext_output_image_capture_source_manager_v1_destroy(fixture.output_manager);
    }
    if (fixture.layout_output) {
        zxdg_output_v1_destroy(fixture.layout_output);
    }
    if (fixture.layout_manager) {
        zxdg_output_manager_v1_destroy(fixture.layout_manager);
    }
    if (fixture.client_output) {
        wl_output_release(fixture.client_output);
    }
    if (fixture.other_output) {
        wl_output_release(fixture.other_output);
    }
    for (size_t i = 0; i < sizeof(fixture.windows) / sizeof(fixture.windows[0]); ++i) {
        if (fixture.windows[i].handle) {
            ext_foreign_toplevel_handle_v1_destroy(fixture.windows[i].handle);
        }
    }
    if (fixture.window_manager) {
        ext_foreign_toplevel_image_capture_source_manager_v1_destroy(fixture.window_manager);
    }
    if (fixture.list) {
        ext_foreign_toplevel_list_v1_destroy(fixture.list);
    }
    if (fixture.shell) {
        xdg_wm_base_destroy(fixture.shell);
    }
    if (fixture.subcompositor) {
        wl_subcompositor_destroy(fixture.subcompositor);
    }
    if (fixture.compositor) {
        wl_compositor_destroy(fixture.compositor);
    }
    if (fixture.shm) {
        wl_shm_destroy(fixture.shm);
    }
    if (fixture.registry) {
        wl_registry_destroy(fixture.registry);
    }
    if (fixture.client) {
        wl_display_disconnect(fixture.client);
    }
    if (fixture.server) {
        wl_display_destroy_clients(fixture.server);
    }
    seaside_output_capture_destroy(fixture.capture);
    remove_output(&fixture);
    if (fixture.backend) {
        wlr_backend_destroy(fixture.backend);
    }
    if (fixture.allocator) {
        wlr_allocator_destroy(fixture.allocator);
    }
    if (fixture.renderer) {
        wlr_renderer_destroy(fixture.renderer);
    }
    if (fixture.server) {
        wl_display_destroy(fixture.server);
    }
    return result;
}

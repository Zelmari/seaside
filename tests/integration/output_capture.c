#define _GNU_SOURCE

#include "output_capture.h"

#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

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
#include <wlr/types/wlr_output.h>

enum { WIDTH = 48, HEIGHT = 42, CURSOR_SIZE = 4 };
static const uint32_t BACKGROUND_RGB = 0x00ff00;
static const uint32_t CURSOR_RGB = 0xff0000;

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
    struct zxdg_output_manager_v1 *layout_manager;
    struct zxdg_output_v1 *layout_output;
    struct ext_output_image_capture_source_manager_v1 *output_manager;
    struct ext_image_copy_capture_manager_v1 *copy_manager;
    struct ext_image_capture_source_v1 *source;
    uint32_t output_name;
    int32_t layout_x, layout_y, layout_width, layout_height;
    bool layout_named, layout_done;
    bool render_failed;
};

struct capture_client {
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;
    struct wl_buffer *buffer;
    uint32_t *pixels;
    uint32_t width, height, format;
    bool constrained, stopped, ready, failed, full_damage, timestamp;
};

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "output capture check failed at %d: %s\n", __LINE__, #condition);      \
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

static void registry_add(void *data, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version) {
    (void)version;
    struct fixture *fixture = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        fixture->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        fixture->client_output = wl_registry_bind(registry, name, &wl_output_interface, 4);
        fixture->output_name = name;
    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        fixture->layout_manager =
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 2);
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        fixture->output_manager = wl_registry_bind(
            registry, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        fixture->copy_manager =
            wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface, 1);
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
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(output, &state, NULL);
    if (!pass) {
        fixture->render_failed = true;
    } else {
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {.width = WIDTH, .height = HEIGHT},
                                           .color = {.g = 1, .a = 1},
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
    if (client->width != WIDTH || client->height != HEIGHT ||
        (client->format != WL_SHM_FORMAT_XRGB8888 && client->format != WL_SHM_FORMAT_ARGB8888)) {
        return false;
    }
    const int size = WIDTH * HEIGHT * sizeof(uint32_t);
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
    struct wl_shm_pool *pool = wl_shm_create_pool(fixture->shm, fd, size);
    close(fd);
    if (!pool) {
        return false;
    }
    client->buffer =
        wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, WIDTH * sizeof(uint32_t), client->format);
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
    client->full_damage |= x == 0 && y == 0 && width == WIDTH && height == HEIGHT;
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
    ext_image_copy_capture_frame_v1_damage_buffer(client->frame, 0, 0, WIDTH, HEIGHT);
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

static void finish_session(struct capture_client *client) {
    if (client->frame) {
        ext_image_copy_capture_frame_v1_destroy(client->frame);
    }
    if (client->session) {
        ext_image_copy_capture_session_v1_destroy(client->session);
    }
    if (client->buffer) {
        wl_buffer_destroy(client->buffer);
    }
    if (client->pixels) {
        munmap(client->pixels, WIDTH * HEIGHT * sizeof(uint32_t));
    }
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

int main(void) {
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
    fixture.capture = seaside_output_capture_create(fixture.server, fixture.output);
    CHECK(fixture.capture);
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

    remove_output(&fixture);
    CHECK(pump_until(&fixture, &hidden.stopped) && pump_until(&fixture, &embedded.stopped));
    CHECK(!seaside_output_capture_client_ready(fixture.capture, getpid(), getuid()));
    finish_session(&hidden);
    finish_session(&embedded);
    ext_image_capture_source_v1_destroy(fixture.source);
    fixture.source = NULL;

    CHECK(add_output(&fixture)); // A recovered output uses the existing client manager bindings.
    CHECK(seaside_output_capture_set_output(fixture.capture, fixture.output));
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
    puts("output capture PASS hidden=2 embedded=3 moved=yes reattached=yes layout=yes "
         "cursor-locks=0");
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

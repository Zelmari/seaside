#include "server.hpp"
#include "wlr_compat.hpp"

#include <errno.h>
#include <fcntl.h>
#include <memory>
#include <pixman.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>
#include <wayland-server-core.h>
extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
}

enum { BUFFER_WIDTH = 16, BUFFER_HEIGHT = 8, SELF_TEST_ROUNDS = 3 };

struct Fixture;

struct FixtureListener {
    wl_listener listener;
    Fixture *fixture;
};

static_assert(std::is_standard_layout_v<FixtureListener>);

struct Fixture {
    std::unique_ptr<seaside::Server> server;
    char *runtime_dir;
    const char *socket;
    struct wlr_output *output;
    struct wlr_surface *surface;
    FixtureListener new_surface;
    FixtureListener surface_commit;
    FixtureListener surface_destroy;
    struct wl_event_source *terminate_signal;
    struct wl_event_source *interrupt_signal;
    struct wl_event_source *deadline;
    struct wl_event_source *client_done;
    bool runtime_created;
    bool listening;
    bool failed;
    bool client_succeeded;
    bool timed_out;
    unsigned buffered_commits;
    unsigned detached_commits;
    unsigned destroyed_surfaces;
    unsigned created_outputs;
    int done_fd = -1;
};

static void fail(Fixture *fixture, const char *message) noexcept {
    fprintf(stderr, "headless fixture: %s\n", message);
    fixture->failed = true;
    fixture->server->terminate();
}

static uint32_t pixel_value(unsigned x, unsigned y) noexcept {
    return 0xff000055u | (x << 16) | (y << 8);
}

static bool create_output(Fixture *fixture, unsigned width, unsigned height) noexcept {
    fixture->output = wlr_headless_add_output(fixture->server->backend(), width, height);
    if (!fixture->output || !wlr_output_init_render(fixture->output, fixture->server->allocator(),
                                                    fixture->server->renderer())) {
        return false;
    }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, (int)width, (int)height, 60000);
    bool committed = wlr_output_commit_state(fixture->output, &state);
    wlr_output_state_finish(&state);
    if (!committed || !fixture->output->enabled || fixture->output->width != (int)width ||
        fixture->output->height != (int)height) {
        return false;
    }
    wlr_output_create_global(fixture->output, fixture->server->display());
    fixture->created_outputs++;
    return true;
}

static bool check_texture(struct wlr_surface *surface) noexcept {
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture || !wlr_texture_is_pixman(texture) || surface->current.width != BUFFER_WIDTH ||
        surface->current.height != BUFFER_HEIGHT) {
        return false;
    }
    pixman_image_t *image = wlr_pixman_texture_get_image(texture);
    if (!image || pixman_image_get_width(image) != BUFFER_WIDTH ||
        pixman_image_get_height(image) != BUFFER_HEIGHT) {
        return false;
    }
    const uint32_t *pixels = pixman_image_get_data(image);
    size_t stride = (size_t)pixman_image_get_stride(image) / sizeof(*pixels);
    for (unsigned y = 0; y < BUFFER_HEIGHT; y++) {
        for (unsigned x = 0; x < BUFFER_WIDTH; x++) {
            if (pixels[y * stride + x] != pixel_value(x, y)) {
                return false;
            }
        }
    }
    return true;
}

static void on_surface_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    FixtureListener *source = wl_container_of(listener, source, listener);
    Fixture *fixture = source->fixture;
    if (wlr_surface_has_buffer(fixture->surface)) {
        if (fixture->buffered_commits++ || !check_texture(fixture->surface)) {
            fail(fixture, "client SHM buffer did not become the expected texture");
            return;
        }
        /* Removing the sole output must leave the existing client and its
         * surface alive. The client verifies removal before its next commit. */
        struct wlr_output *output = fixture->output;
        fixture->output = nullptr;
        wlr_output_destroy(output);
    } else {
        if (fixture->buffered_commits != 1 || fixture->output || fixture->detached_commits++ ||
            !create_output(fixture, 320, 180)) {
            fail(fixture, "output return after a zero-output commit failed");
        }
    }
}

static void on_surface_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    FixtureListener *source = wl_container_of(listener, source, listener);
    Fixture *fixture = source->fixture;
    wl_list_remove(&fixture->surface_commit.listener.link);
    wl_list_remove(&fixture->surface_destroy.listener.link);
    fixture->surface = nullptr;
    fixture->destroyed_surfaces++;
}

static void on_new_surface(struct wl_listener *listener, void *data) noexcept {
    FixtureListener *source = wl_container_of(listener, source, listener);
    Fixture *fixture = source->fixture;
    if (fixture->surface || fixture->destroyed_surfaces) {
        fail(fixture, "unexpected additional client surface");
        return;
    }
    fixture->surface = static_cast<wlr_surface *>(data);
    fixture->surface_commit.fixture = fixture;
    fixture->surface_commit.listener.notify = on_surface_commit;
    fixture->surface_destroy.fixture = fixture;
    fixture->surface_destroy.listener.notify = on_surface_destroy;
    wl_signal_add(&fixture->surface->events.commit, &fixture->surface_commit.listener);
    wl_signal_add(&fixture->surface->events.destroy, &fixture->surface_destroy.listener);
}

static int on_terminate(int signal_number, void *data) noexcept {
    (void)signal_number;
    auto *fixture = static_cast<Fixture *>(data);
    fixture->server->terminate();
    return 0;
}

static bool fixture_init(Fixture *fixture, const char *runtime_root) noexcept {
    char *absolute_root = realpath(runtime_root, nullptr);
    if (!absolute_root) {
        fprintf(stderr, "headless fixture: runtime root: %s\n", strerror(errno));
        return false;
    }
    char *runtime_dir = nullptr;
    int length = asprintf(&runtime_dir, "%s/seaside-fixture-XXXXXX", absolute_root);
    free(absolute_root);
    if (length < 0) {
        return false;
    }
    fixture->runtime_dir = runtime_dir;
    if (!mkdtemp(fixture->runtime_dir)) {
        return false;
    }
    fixture->runtime_created = true;
    if (setenv("XDG_RUNTIME_DIR", fixture->runtime_dir, 1) < 0) {
        return false;
    }
    auto server = seaside::Server::create();
    if (!server) {
        return false;
    }
    fixture->server = std::move(*server);
    struct wl_event_loop *loop = wl_display_get_event_loop(fixture->server->display());
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    if (!backend || !renderer) {
        if (backend) {
            wlr_backend_destroy(backend);
        }
        if (renderer) {
            wlr_renderer_destroy(renderer);
        }
        return false;
    }
    if (!fixture->server->start(backend, renderer)) {
        return false;
    }
    const auto socket = fixture->server->listen();
    if (!socket) {
        return false;
    }
    fixture->socket = socket->data();
    fixture->terminate_signal = wl_event_loop_add_signal(loop, SIGTERM, on_terminate, fixture);
    fixture->interrupt_signal = wl_event_loop_add_signal(loop, SIGINT, on_terminate, fixture);
    if (!fixture->terminate_signal || !fixture->interrupt_signal ||
        !create_output(fixture, 160, 90)) {
        return false;
    }
    fixture->new_surface.fixture = fixture;
    fixture->new_surface.listener.notify = on_new_surface;
    wl_signal_add(&fixture->server->compositor()->events.new_surface,
                  &fixture->new_surface.listener);
    fixture->listening = true;
    return true;
}

static bool fixture_finish(Fixture *fixture) noexcept {
    struct wl_event_source *sources[] = {fixture->terminate_signal, fixture->interrupt_signal,
                                         fixture->deadline, fixture->client_done};
    for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
        if (sources[i]) {
            wl_event_source_remove(sources[i]);
        }
    }
    if (fixture->done_fd >= 0) {
        close(fixture->done_fd);
    }
    if (fixture->listening) {
        wl_list_remove(&fixture->new_surface.listener.link);
    }
    /* Surface destroy callbacks remain live until clients are disconnected. */
    fixture->server.reset();
    bool clean = true;
    if (fixture->runtime_dir) {
        if (fixture->runtime_created && rmdir(fixture->runtime_dir) < 0) {
            fprintf(stderr, "headless fixture: runtime cleanup: %s\n", strerror(errno));
            clean = false;
        }
        free(fixture->runtime_dir);
    }
    return clean;
}

struct client_state {
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    uint32_t output_names[4];
    unsigned output_count;
    unsigned output_removals;
    bool argb8888;
};

static void on_shm_format(void *data, struct wl_shm *shm, uint32_t format) noexcept {
    (void)shm;
    auto *client = static_cast<client_state *>(data);
    if (format == WL_SHM_FORMAT_ARGB8888) {
        client->argb8888 = true;
    }
}

static const struct wl_shm_listener shm_listener = {.format = on_shm_format};

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) noexcept {
    auto *client = static_cast<client_state *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
        client->compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        wl_shm_add_listener(client->shm, &shm_listener, client);
    } else if (strcmp(interface, wl_output_interface.name) == 0 && client->output_count < 4) {
        client->output_names[client->output_count++] = name;
    }
}

static void on_global_remove(void *data, struct wl_registry *registry, uint32_t name) noexcept {
    (void)registry;
    auto *client = static_cast<client_state *>(data);
    for (unsigned i = 0; i < client->output_count; i++) {
        if (client->output_names[i] == name) {
            client->output_removals++;
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

constexpr size_t buffer_bytes = BUFFER_WIDTH * BUFFER_HEIGHT * sizeof(uint32_t);

struct ClientConnection {
    wl_display *display;
    client_state client{};
    wl_registry *registry = nullptr;
    wl_surface *surface = nullptr;
    wl_buffer *buffer = nullptr;
    void *pixels = MAP_FAILED;
    int memory_fd = -1;

    explicit ClientConnection(wl_display *display) noexcept : display(display) {}
    ClientConnection(const ClientConnection &) = delete;
    ClientConnection &operator=(const ClientConnection &) = delete;

    ~ClientConnection() {
        if (surface) {
            wl_surface_destroy(surface);
        }
        if (buffer) {
            wl_buffer_destroy(buffer);
        }
        if (client.shm) {
            wl_shm_destroy(client.shm);
        }
        if (client.compositor) {
            wl_compositor_destroy(client.compositor);
        }
        if (registry) {
            wl_registry_destroy(registry);
        }
        if (display) {
            wl_display_disconnect(display);
        }
        if (pixels != MAP_FAILED) {
            munmap(pixels, buffer_bytes);
        }
        if (memory_fd >= 0) {
            close(memory_fd);
        }
    }
};

static bool exercise_client(const char *socket_name) noexcept {
    /* libwayland gives an inherited connection FD precedence even over an
     * explicit name. This client must connect to the fixture's private socket. */
    if (unsetenv("WAYLAND_SOCKET") < 0) {
        return false;
    }
    ClientConnection connection{wl_display_connect(socket_name)};
    if (!connection.display) {
        return false;
    }
    auto *display = connection.display;
    auto &client = connection.client;
    connection.registry = wl_display_get_registry(display);
    if (!connection.registry) {
        return false;
    }
    wl_registry_add_listener(connection.registry, &registry_listener, &client);
    if (wl_display_roundtrip(display) < 0 || !client.compositor || !client.shm ||
        wl_display_roundtrip(display) < 0 || !client.argb8888 || client.output_count != 1) {
        return false;
    }
    connection.memory_fd = memfd_create("seaside-test-buffer", MFD_CLOEXEC);
    if (connection.memory_fd < 0 ||
        ftruncate(connection.memory_fd, static_cast<off_t>(buffer_bytes)) < 0) {
        return false;
    }
    connection.pixels =
        mmap(nullptr, buffer_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, connection.memory_fd, 0);
    if (connection.pixels == MAP_FAILED) {
        return false;
    }
    auto *pixels = static_cast<uint32_t *>(connection.pixels);
    for (unsigned y = 0; y < BUFFER_HEIGHT; y++) {
        for (unsigned x = 0; x < BUFFER_WIDTH; x++) {
            pixels[y * BUFFER_WIDTH + x] = pixel_value(x, y);
        }
    }
    auto *pool = wl_shm_create_pool(client.shm, connection.memory_fd, buffer_bytes);
    if (!pool) {
        return false;
    }
    connection.buffer = wl_shm_pool_create_buffer(
        pool, 0, BUFFER_WIDTH, BUFFER_HEIGHT, BUFFER_WIDTH * static_cast<int32_t>(sizeof(uint32_t)),
        WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    connection.surface = wl_compositor_create_surface(client.compositor);
    if (!connection.buffer || !connection.surface) {
        return false;
    }
    wl_surface_attach(connection.surface, connection.buffer, 0, 0);
    wl_surface_damage_buffer(connection.surface, 0, 0, BUFFER_WIDTH, BUFFER_HEIGHT);
    wl_surface_commit(connection.surface);
    if (wl_display_roundtrip(display) < 0 || client.output_removals != 1 ||
        client.output_count != 1) {
        return false;
    }
    /* This roundtrip and detach occur with no output at all. A replacement
     * output is published only after the server receives the detach. */
    if (wl_display_roundtrip(display) < 0) {
        return false;
    }
    wl_surface_attach(connection.surface, nullptr, 0, 0);
    wl_surface_commit(connection.surface);
    if (wl_display_roundtrip(display) < 0 || client.output_count != 2 ||
        client.output_removals != 1) {
        return false;
    }
    wl_surface_destroy(connection.surface);
    connection.surface = nullptr;
    return wl_display_roundtrip(display) >= 0;
}

static int on_client_done(int fd, uint32_t mask, void *data) noexcept {
    (void)mask;
    auto *fixture = static_cast<Fixture *>(data);
    unsigned char result;
    ssize_t count = read(fd, &result, sizeof(result));
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    fixture->client_succeeded = count == 1 && result == 1;
    if (!fixture->client_succeeded) {
        fail(fixture, "real Wayland client failed or disconnected unexpectedly");
    } else {
        fixture->server->terminate();
    }
    return 0;
}

static int on_deadline(void *data) noexcept {
    auto *fixture = static_cast<Fixture *>(data);
    fixture->timed_out = true;
    fail(fixture, "client lifecycle exceeded its 10 second deadline");
    return 0;
}

static bool self_test_round(const char *runtime_root) noexcept {
    Fixture fixture{};
    if (!fixture_init(&fixture, runtime_root)) {
        fprintf(stderr, "headless fixture: initialization failed\n");
        fixture_finish(&fixture);
        return false;
    }
    int completion[2];
    if (pipe2(completion, O_CLOEXEC | O_NONBLOCK) < 0) {
        fixture_finish(&fixture);
        return false;
    }
    pid_t child = fork();
    if (child == 0) {
        close(completion[0]);
        unsigned char result = exercise_client(fixture.socket) ? 1 : 0;
        ssize_t written;
        do {
            written = write(completion[1], &result, sizeof(result));
        } while (written < 0 && errno == EINTR);
        close(completion[1]);
        /* The fork inherited server descriptors; only the parent owns their
         * teardown. _exit avoids copied Wayland/libc cleanup in the child. */
        _exit(result == 1 && written == 1 ? EXIT_SUCCESS : EXIT_FAILURE);
    }
    close(completion[1]);
    fixture.done_fd = completion[0];
    if (child < 0) {
        fixture_finish(&fixture);
        return false;
    }
    struct wl_event_loop *loop = wl_display_get_event_loop(fixture.server->display());
    fixture.client_done =
        wl_event_loop_add_fd(loop, fixture.done_fd, WL_EVENT_READABLE, on_client_done, &fixture);
    fixture.deadline = wl_event_loop_add_timer(loop, on_deadline, &fixture);
    if (!fixture.client_done || !fixture.deadline ||
        wl_event_source_timer_update(fixture.deadline, 10000) < 0) {
        fixture.failed = true;
    } else {
        fixture.failed |= !fixture.server->run();
    }
    if (!fixture.client_succeeded || fixture.failed || fixture.timed_out) {
        kill(child, SIGKILL);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    bool passed = !fixture.failed && fixture.client_succeeded && waited == child &&
                  WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS &&
                  fixture.buffered_commits == 1 && fixture.detached_commits == 1 &&
                  fixture.destroyed_surfaces == 1 && !fixture.surface &&
                  fixture.created_outputs == 2;
    return fixture_finish(&fixture) && passed;
}

static void print_json_string(const char *value) noexcept {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 0x20) {
            printf("\\u%04x", *p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

int main(int argc, char **argv) {
    bool serve = argc == 2 && strcmp(argv[1], "--serve") == 0;
    if (argc != 1 && !serve) {
        fprintf(stderr, "usage: seaside-headless-fixture [--serve]\n");
        return EXIT_FAILURE;
    }
    const char *configured_runtime = getenv("XDG_RUNTIME_DIR");
    char *runtime_root = strdup(configured_runtime ? configured_runtime : "/tmp");
    if (!runtime_root) {
        return EXIT_FAILURE;
    }
    bool passed = true;
    if (serve) {
        Fixture fixture{};
        passed = fixture_init(&fixture, runtime_root);
        if (passed) {
            fputs("{\"event\":\"ready\",\"socket\":", stdout);
            print_json_string(fixture.socket);
            fputs(",\"runtime_dir\":", stdout);
            print_json_string(fixture.runtime_dir);
            fputs("}\n", stdout);
            fflush(stdout);
            passed = fixture.server->run().has_value() && !fixture.failed;
        }
        passed = fixture_finish(&fixture) && passed;
    } else {
        unsigned completed_rounds = 0;
        for (unsigned i = 0; i < SELF_TEST_ROUNDS; i++) {
            if (!self_test_round(runtime_root)) {
                passed = false;
                break;
            }
            completed_rounds++;
        }
        printf("{\"event\":\"self-test\",\"status\":\"%s\",\"rounds\":%u}\n",
               passed ? "pass" : "fail", completed_rounds);
    }
    free(runtime_root);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

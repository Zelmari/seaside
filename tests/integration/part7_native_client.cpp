#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <chrono>
#include <errno.h>
#include <new>
#include <optional>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>
#include <wayland-client.h>

using Clock = std::chrono::steady_clock;

struct client {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct wl_surface *surface;
    struct wl_callback *frame;
    struct wl_list buffers;
    int32_t width;
    int32_t height;
    int32_t pending_width;
    int32_t pending_height;
    uint32_t pixel;
    unsigned resize_delay_ms;
    std::optional<Clock::time_point> resize_deadline;
    bool dirty;
    bool fixed_size;
    bool configured;
    bool stop;
    bool failed;
};

struct pending_buffer {
    struct wl_buffer *proxy;
    struct wl_list link;
};

static_assert(std::is_standard_layout_v<pending_buffer>);

static void draw(struct client *client) noexcept;

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) noexcept {
    (void)time;
    auto *client = static_cast<struct client *>(data);
    wl_callback_destroy(callback);
    client->frame = NULL;
    draw(client);
    puts("frame-callback");
}

static void buffer_released(void *data, struct wl_buffer *buffer) noexcept {
    auto *pending = static_cast<pending_buffer *>(data);
    wl_buffer_destroy(buffer);
    wl_list_remove(&pending->link);
    delete pending;
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};
static const struct wl_buffer_listener buffer_listener = {.release = buffer_released};

static void draw(struct client *client) noexcept {
    if (!client->dirty || client->resize_deadline) {
        return;
    }
    const int32_t stride = client->width * 4;
    const int32_t length = stride * client->height;
    const int fd = memfd_create("seaside-part7-pixels", MFD_CLOEXEC);
    if (fd < 0) {
        client->failed = true;
        return;
    }
    if (ftruncate(fd, length) < 0) {
        close(fd);
        client->failed = true;
        return;
    }
    auto *pixels = static_cast<uint32_t *>(
        mmap(NULL, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (pixels == MAP_FAILED) {
        close(fd);
        client->failed = true;
        return;
    }
    for (int32_t index = 0; index < length / 4; ++index) {
        pixels[index] = client->pixel;
    }
    munmap(pixels, (size_t)length);
    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, length);
    close(fd);
    if (!pool) {
        client->failed = true;
        return;
    }
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, client->width, client->height,
                                                         stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    if (!buffer) {
        client->failed = true;
        return;
    }
    auto *pending = new (std::nothrow) pending_buffer{};
    if (!pending) {
        wl_buffer_destroy(buffer);
        client->failed = true;
        return;
    }
    pending->proxy = buffer;
    wl_list_insert(&client->buffers, &pending->link);
    wl_buffer_add_listener(buffer, &buffer_listener, pending);
    if (!client->frame) {
        client->frame = wl_surface_frame(client->surface);
        if (!client->frame) {
            buffer_released(pending, buffer);
            client->failed = true;
            return;
        }
        wl_callback_add_listener(client->frame, &frame_listener, client);
    }
    wl_surface_attach(client->surface, buffer, 0, 0);
    wl_surface_damage(client->surface, 0, 0, client->width, client->height);
    wl_surface_commit(client->surface);
    client->dirty = false;
    printf("color %06x %d %d\n", client->pixel & 0xffffffU, client->width, client->height);
}

static void surface_configured(void *data, struct xdg_surface *surface, uint32_t serial) noexcept {
    auto *client = static_cast<struct client *>(data);
    xdg_surface_ack_configure(surface, serial);
    const bool resized = client->pending_width > 0 && (client->pending_width != client->width ||
                                                       client->pending_height != client->height);
    if (resized) {
        client->width = client->pending_width;
        client->height = client->pending_height;
        client->dirty = true;
    }
    client->pending_width = 0;
    client->pending_height = 0;
    if (client->configured && resized && client->resize_delay_ms) {
        // Acknowledge each configure while waiting to commit the latest size.
        if (wl_display_flush(client->display) < 0 && errno != EAGAIN) {
            client->failed = true;
            return;
        }
        client->resize_deadline = Clock::now() + std::chrono::milliseconds(client->resize_delay_ms);
    }
    client->configured = true;
    draw(client);
}

static void toplevel_configured(void *data, struct xdg_toplevel *toplevel, int32_t width,
                                int32_t height, struct wl_array *states) noexcept {
    (void)toplevel;
    (void)states;
    auto *client = static_cast<struct client *>(data);
    if (!client->fixed_size && width > 0 && height > 0 && width <= 4096 && height <= 4096) {
        client->pending_width = width;
        client->pending_height = height;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) noexcept {
    (void)toplevel;
    ((struct client *)data)->stop = true;
}

static void shell_ping(void *data, struct xdg_wm_base *shell, uint32_t serial) noexcept {
    (void)data;
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener shell_listener = {.ping = shell_ping};
static const struct xdg_surface_listener surface_listener = {.configure = surface_configured};
static const struct xdg_toplevel_listener toplevel_listener = {.configure = toplevel_configured,
                                                               .close = toplevel_close,
                                                               .configure_bounds = nullptr,
                                                               .wm_capabilities = nullptr};

static void registry_add(void *data, struct wl_registry *registry, uint32_t name,
                         const char *interface, uint32_t version) noexcept {
    (void)version;
    auto *client = static_cast<struct client *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        client->compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        client->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->shell =
            static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(client->shell, &shell_listener, NULL);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name) noexcept {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {.global = registry_add,
                                                              .global_remove = registry_remove};

static bool parse_pixel(const char *word, uint32_t *pixel) noexcept {
    char *end = NULL;
    const unsigned long value = strtoul(word, &end, 16);
    if (word == end || (*end && *end != '\n') || value > 0xffffffUL) {
        return false;
    }
    *pixel = (uint32_t)value | 0xff000000U;
    return true;
}

static bool parse_option(const char *word, struct client *client) noexcept {
    if (strcmp(word, "--fixed-size") == 0) {
        client->fixed_size = true;
        return true;
    }
    const char prefix[] = "--delay-resize=";
    if (strncmp(word, prefix, sizeof(prefix) - 1) == 0) {
        const char *start = word + sizeof(prefix) - 1;
        char *end = NULL;
        unsigned long delay = strtoul(start, &end, 10);
        if (end != start && *end == '\0' && delay > 0 && delay <= 3000) {
            client->resize_delay_ms = (unsigned)delay;
            return true;
        }
    }
    return false;
}

static void read_command(struct client *client) noexcept {
    char line[24];
    if (!fgets(line, sizeof(line), stdin) || strcmp(line, "close\n") == 0) {
        client->stop = true;
    } else if (parse_pixel(line, &client->pixel)) {
        client->dirty = true;
        if (client->configured && !client->frame) {
            draw(client);
        }
    } else {
        fputs("Invalid fixture color\n", stderr);
        client->failed = true;
    }
}

static void run(struct client *client) noexcept {
    struct pollfd fds[] = {
        {.fd = wl_display_get_fd(client->display), .events = POLLIN, .revents = 0},
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0}};
    while (!client->stop && !client->failed) {
        if (wl_display_dispatch_pending(client->display) < 0) {
            client->failed = true;
            break;
        }
        const int flushed = wl_display_flush(client->display);
        if (flushed < 0 && errno != EAGAIN) {
            client->failed = true;
            break;
        }
        fds[0].events = flushed < 0 ? POLLIN | POLLOUT : POLLIN;
        int timeout = -1;
        if (client->resize_deadline) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                *client->resize_deadline - Clock::now());
            timeout =
                static_cast<int>(std::max(remaining, std::chrono::milliseconds::zero()).count());
        }
        if (poll(fds, 2, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            client->failed = true;
            break;
        }
        // Dispatch a ready configure before committing a size whose deadline expired.
        if (((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
             wl_display_dispatch(client->display) < 0) ||
            (fds[0].revents & POLLNVAL)) {
            client->failed = true;
        } else if (fds[1].revents) {
            read_command(client);
        }
        if (!client->stop && !client->failed && client->resize_deadline &&
            Clock::now() >= *client->resize_deadline) {
            client->resize_deadline.reset();
            draw(client);
        }
    }
}

static void destroy_client(struct client *client, struct wl_registry *registry) noexcept {
    if (client->frame) {
        wl_callback_destroy(client->frame);
    }
    struct pending_buffer *buffer, *next;
    wl_list_for_each_safe(buffer, next, &client->buffers, link) {
        buffer_released(buffer, buffer->proxy);
    }
    if (client->surface) {
        wl_surface_destroy(client->surface);
    }
    if (client->shell) {
        xdg_wm_base_destroy(client->shell);
    }
    if (client->shm) {
        wl_shm_destroy(client->shm);
    }
    if (client->compositor) {
        wl_compositor_destroy(client->compositor);
    }
    if (registry) {
        wl_registry_destroy(registry);
    }
    wl_display_flush(client->display);
    wl_display_disconnect(client->display);
}

int main(int argc, char **argv) {
    struct client client{};
    client.width = 264;
    client.height = 176;
    client.dirty = true;
    if (argc < 3 || argc > 4 || (argc == 4 && !parse_option(argv[3], &client)) ||
        !parse_pixel(argv[2], &client.pixel)) {
        fputs("Usage: seaside-part7-native-client TITLE RRGGBB [--fixed-size|--delay-resize=MS]\n",
              stderr);
        return EXIT_FAILURE;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin, NULL, _IONBF, 0);
    wl_list_init(&client.buffers);
    client.display = wl_display_connect(NULL);
    if (!client.display) {
        return EXIT_FAILURE;
    }
    struct wl_registry *registry = wl_display_get_registry(client.display);
    if (!registry || wl_registry_add_listener(registry, &registry_listener, &client) < 0 ||
        wl_display_roundtrip(client.display) < 0 || !client.compositor || !client.shm ||
        !client.shell) {
        destroy_client(&client, registry);
        return EXIT_FAILURE;
    }
    client.surface = wl_compositor_create_surface(client.compositor);
    struct xdg_surface *xdg =
        client.surface ? xdg_wm_base_get_xdg_surface(client.shell, client.surface) : NULL;
    struct xdg_toplevel *toplevel = xdg ? xdg_surface_get_toplevel(xdg) : NULL;
    if (toplevel) {
        xdg_surface_add_listener(xdg, &surface_listener, &client);
        xdg_toplevel_add_listener(toplevel, &toplevel_listener, &client);
        xdg_toplevel_set_title(toplevel, argv[1]);
        xdg_toplevel_set_app_id(toplevel, "org.seaside.Part7.NativeFixture");
        wl_surface_commit(client.surface);
        run(&client);
        xdg_toplevel_destroy(toplevel);
    } else {
        client.failed = true;
    }
    if (xdg) {
        xdg_surface_destroy(xdg);
    }
    destroy_client(&client, registry);
    return client.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

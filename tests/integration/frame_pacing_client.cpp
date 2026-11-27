#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {
constexpr int width = 512;
constexpr int height = 320;
constexpr int stride = width * sizeof(std::uint32_t);
constexpr int bytes = stride * height;

struct Buffer {
    wl_buffer *proxy = nullptr;
    std::uint32_t *pixels = nullptr;
    bool busy = false;
};

struct Client;
struct Feedback {
    Client *client;
    struct wp_presentation_feedback *proxy;
    std::uint64_t frame;
};

struct Client {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    wp_presentation *presentation = nullptr;
    wl_surface *surface = nullptr;
    xdg_surface *role = nullptr;
    xdg_toplevel *toplevel = nullptr;
    wl_callback *callback = nullptr;
    std::array<Buffer, 3> buffers{};
    std::list<Feedback> feedbacks;
    std::uint32_t *mapping = nullptr;
    clockid_t clock = -1;
    std::uint64_t frame = 0;
    bool configured = false;
    bool close = false;

    ~Client();
    void draw();
};

void forget_feedback(Feedback &feedback) {
    wp_presentation_feedback_destroy(feedback.proxy);
    auto &feedbacks = feedback.client->feedbacks;
    feedbacks.remove_if([&](const Feedback &entry) { return &entry == &feedback; });
}

constexpr wp_presentation_feedback_listener feedback_listener{
    .sync_output = [](void *, struct wp_presentation_feedback *, wl_output *) {},
    .presented =
        [](void *data, struct wp_presentation_feedback *, std::uint32_t seconds_hi,
           std::uint32_t seconds_lo, std::uint32_t ns, std::uint32_t period, std::uint32_t seq_hi,
           std::uint32_t seq_lo, std::uint32_t flags) {
            auto &feedback = *static_cast<Feedback *>(data);
            const auto seconds = (std::uint64_t{seconds_hi} << 32) | seconds_lo;
            const auto sequence = (std::uint64_t{seq_hi} << 32) | seq_lo;
            std::printf("present %llu %llu %u %llu %u\n",
                        static_cast<unsigned long long>(feedback.frame),
                        static_cast<unsigned long long>(seconds * 1'000'000'000 + ns), period,
                        static_cast<unsigned long long>(sequence), flags);
            forget_feedback(feedback);
        },
    .discarded =
        [](void *data, struct wp_presentation_feedback *) {
            auto &feedback = *static_cast<Feedback *>(data);
            std::printf("discard %llu\n", static_cast<unsigned long long>(feedback.frame));
            forget_feedback(feedback);
        },
};

constexpr wl_callback_listener frame_listener{
    .done =
        [](void *data, wl_callback *callback, std::uint32_t milliseconds) {
            auto &client = *static_cast<Client *>(data);
            std::printf("callback %llu %u\n", static_cast<unsigned long long>(client.frame),
                        milliseconds);
            wl_callback_destroy(callback);
            client.callback = nullptr;
            client.draw();
        },
};

void Client::draw() {
    if (!configured || callback || close) {
        return;
    }
    const auto buffer = std::ranges::find(buffers, false, &Buffer::busy);
    if (buffer == buffers.end()) {
        return;
    }
    auto *stripe = buffer->pixels + (frame % height) * width;
    std::fill_n(stripe, width, (frame % 2 == 0) ? 0x00245473 : 0x006a351f);
    buffer->busy = true;
    auto &feedback =
        feedbacks.emplace_back(this, wp_presentation_feedback(presentation, surface), ++frame);
    wp_presentation_feedback_add_listener(feedback.proxy, &feedback_listener, &feedback);
    callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &frame_listener, this);
    wl_surface_attach(surface, buffer->proxy, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    wl_surface_commit(surface);
    timespec now{};
    if (clock_gettime(clock, &now) == 0) {
        std::printf("commit %llu %llu\n", static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(now.tv_sec) * 1'000'000'000 + now.tv_nsec);
    }
}

constexpr wl_buffer_listener buffer_listener{
    .release = [](void *data, wl_buffer *) { static_cast<Buffer *>(data)->busy = false; },
};
constexpr xdg_surface_listener surface_listener{
    .configure =
        [](void *data, xdg_surface *surface, std::uint32_t serial) {
            auto &client = *static_cast<Client *>(data);
            xdg_surface_ack_configure(surface, serial);
            client.configured = true;
            client.draw();
        },
};
constexpr xdg_toplevel_listener toplevel_listener{
    .configure = [](void *, xdg_toplevel *, std::int32_t, std::int32_t, wl_array *) {},
    .close = [](void *data, xdg_toplevel *) { static_cast<Client *>(data)->close = true; },
    .configure_bounds = nullptr,
    .wm_capabilities = nullptr,
};
constexpr xdg_wm_base_listener shell_listener{
    .ping = [](void *, xdg_wm_base *shell,
               std::uint32_t serial) { xdg_wm_base_pong(shell, serial); },
};
constexpr wp_presentation_listener presentation_listener{
    .clock_id =
        [](void *data, wp_presentation *, std::uint32_t clock) {
            static_cast<Client *>(data)->clock = static_cast<clockid_t>(clock);
            std::printf("clock %u\n", clock);
        },
};
constexpr wl_registry_listener registry_listener{
    .global =
        [](void *data, wl_registry *registry, std::uint32_t name, const char *interface,
           std::uint32_t version) {
            auto &client = *static_cast<Client *>(data);
            if (std::strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
                client.compositor = static_cast<wl_compositor *>(
                    wl_registry_bind(registry, name, &wl_compositor_interface, 4));
            } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
                client.shm =
                    static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
            } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
                client.shell = static_cast<xdg_wm_base *>(
                    wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
                xdg_wm_base_add_listener(client.shell, &shell_listener, nullptr);
            } else if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
                client.presentation = static_cast<wp_presentation *>(
                    wl_registry_bind(registry, name, &wp_presentation_interface, 1));
                wp_presentation_add_listener(client.presentation, &presentation_listener, data);
            }
        },
    .global_remove = [](void *, wl_registry *, std::uint32_t) {},
};

Client::~Client() {
    for (const auto &feedback : feedbacks) {
        wp_presentation_feedback_destroy(feedback.proxy);
    }
    if (callback) {
        wl_callback_destroy(callback);
    }
    for (const auto &buffer : buffers) {
        if (buffer.proxy) {
            wl_buffer_destroy(buffer.proxy);
        }
    }
    if (mapping) {
        munmap(mapping, bytes * buffers.size());
    }
    if (toplevel) {
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(role);
        wl_surface_destroy(surface);
    }
    if (presentation) {
        wp_presentation_destroy(presentation);
    }
    if (shell) {
        xdg_wm_base_destroy(shell);
    }
    if (compositor) {
        wl_compositor_destroy(compositor);
    }
    if (shm) {
        wl_shm_destroy(shm);
    }
    if (registry) {
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fputs("Usage: seaside-frame-pacing-client APP_ID\n", stderr);
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Client client;
    client.display = wl_display_connect(nullptr);
    if (!client.display) {
        std::perror("Wayland display");
        return 1;
    }
    client.registry = wl_display_get_registry(client.display);
    wl_registry_add_listener(client.registry, &registry_listener, &client);
    if (wl_display_roundtrip(client.display) < 0 || !client.compositor || !client.shm ||
        !client.shell || !client.presentation || wl_display_roundtrip(client.display) < 0 ||
        client.clock < 0) {
        std::fputs("Required Wayland globals or presentation clock unavailable\n", stderr);
        return 1;
    }
    const auto length = bytes * client.buffers.size();
    const int fd = memfd_create("seaside-frame-pacing", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(length)) < 0) {
        std::perror("Frame buffer storage");
        if (fd >= 0) {
            close(fd);
        }
        return 1;
    }
    void *mapping = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        std::perror("Frame buffer mapping");
        close(fd);
        return 1;
    }
    client.mapping = static_cast<std::uint32_t *>(mapping);
    std::fill_n(client.mapping, length / sizeof(std::uint32_t), 0x00202020U);
    auto *pool = wl_shm_create_pool(client.shm, fd, static_cast<int>(length));
    close(fd);
    int offset = 0;
    for (auto &buffer : client.buffers) {
        buffer.pixels = client.mapping + offset / sizeof(std::uint32_t);
        buffer.proxy =
            wl_shm_pool_create_buffer(pool, offset, width, height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_buffer_add_listener(buffer.proxy, &buffer_listener, &buffer);
        offset += bytes;
    }
    wl_shm_pool_destroy(pool);

    client.surface = wl_compositor_create_surface(client.compositor);
    client.role = xdg_wm_base_get_xdg_surface(client.shell, client.surface);
    client.toplevel = xdg_surface_get_toplevel(client.role);
    xdg_toplevel_set_app_id(client.toplevel, argv[1]);
    xdg_toplevel_set_title(client.toplevel, argv[1]);
    xdg_toplevel_set_min_size(client.toplevel, width, height);
    xdg_toplevel_set_max_size(client.toplevel, width, height);
    xdg_surface_add_listener(client.role, &surface_listener, &client);
    xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
    wl_surface_commit(client.surface);
    while (!client.close) {
        if (wl_display_dispatch(client.display) < 0) {
            std::perror("Wayland dispatch");
            return 1;
        }
        // A buffer release can arrive after the frame callback.
        client.draw();
    }
    return 0;
}

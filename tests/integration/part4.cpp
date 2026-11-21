#include "desktop.hpp"
#include "part3_bridge.h"
#include "server.hpp"
#include "wlroots_bridge.h"
#include "xdg-shell-client-protocol.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

namespace {

using seaside::Action;
using seaside::ActionKind;
using seaside::WindowId;
using seaside::WindowRect;

constexpr WindowRect output_bounds{0, 0, 160, 90};
constexpr int ordinary_width = 48;
constexpr int ordinary_height = 32;

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "Part 4 fixture failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                              \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

struct Configure {
    int width = 0;
    int height = 0;
    bool activated = false;
    bool maximized = false;
    bool fullscreen = false;
};

struct Client {
    ~Client() {
        destroy_window();
        if (shell) {
            xdg_wm_base_destroy(shell);
        }
        if (shm) {
            wl_shm_destroy(shm);
        }
        if (compositor) {
            wl_compositor_destroy(compositor);
        }
        if (registry) {
            wl_registry_destroy(registry);
        }
        if (display) {
            wl_display_disconnect(display);
        }
    }

    void destroy_window() {
        if (toplevel) {
            xdg_toplevel_destroy(toplevel);
            toplevel = nullptr;
        }
        if (xdg) {
            xdg_surface_destroy(xdg);
            xdg = nullptr;
        }
        if (surface) {
            wl_surface_destroy(surface);
            surface = nullptr;
        }
    }

    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    wl_surface *surface = nullptr;
    xdg_surface *xdg = nullptr;
    xdg_toplevel *toplevel = nullptr;
    Configure pending;
    Configure configured;
    std::uint32_t serial = 0;
    unsigned configures = 0;
    unsigned frames = 0;
    bool close_requested = false;
};

struct Popup {
    ~Popup() {
        if (role) {
            xdg_popup_destroy(role);
        }
        if (xdg) {
            xdg_surface_destroy(xdg);
        }
        if (surface) {
            wl_surface_destroy(surface);
        }
    }

    wl_surface *surface = nullptr;
    xdg_surface *xdg = nullptr;
    xdg_popup *role = nullptr;
    WindowRect geometry;
    std::uint32_t serial = 0;
    bool dismissed = false;
};

void ping(void *, xdg_wm_base *shell, std::uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
const xdg_wm_base_listener shell_listener{.ping = ping};

void global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
            std::uint32_t version) {
    auto &client = *static_cast<Client *>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0 && version >= 4) {
        client.compositor = static_cast<wl_compositor *>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        client.shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0 && version >= 6) {
        client.shell =
            static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 6));
        xdg_wm_base_add_listener(client.shell, &shell_listener, nullptr);
    }
}

void global_remove(void *, wl_registry *, std::uint32_t) {}
const wl_registry_listener registry_listener{.global = global, .global_remove = global_remove};

void surface_configure(void *data, xdg_surface *, std::uint32_t serial) {
    auto &client = *static_cast<Client *>(data);
    client.configured = client.pending;
    client.serial = serial;
    ++client.configures;
}
const xdg_surface_listener surface_listener{.configure = surface_configure};

void toplevel_configure(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height,
                        wl_array *states) {
    auto &client = *static_cast<Client *>(data);
    client.pending = Configure{.width = width, .height = height};
    auto *state = static_cast<const std::uint32_t *>(states->data);
    for (std::size_t i = 0; i < states->size / sizeof(*state); ++i) {
        client.pending.activated |= state[i] == XDG_TOPLEVEL_STATE_ACTIVATED;
        client.pending.maximized |= state[i] == XDG_TOPLEVEL_STATE_MAXIMIZED;
        client.pending.fullscreen |= state[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
}
void toplevel_close(void *data, xdg_toplevel *) {
    static_cast<Client *>(data)->close_requested = true;
}
void toplevel_bounds(void *, xdg_toplevel *, std::int32_t, std::int32_t) {}
void toplevel_capabilities(void *, xdg_toplevel *, wl_array *) {}
const xdg_toplevel_listener toplevel_listener{
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

void popup_surface_configure(void *data, xdg_surface *, std::uint32_t serial) {
    static_cast<Popup *>(data)->serial = serial;
}
const xdg_surface_listener popup_surface_listener{.configure = popup_surface_configure};

void popup_configure(void *data, xdg_popup *, std::int32_t x, std::int32_t y, std::int32_t width,
                     std::int32_t height) {
    static_cast<Popup *>(data)->geometry = WindowRect{x, y, width, height};
}
void popup_done(void *data, xdg_popup *) {
    static_cast<Popup *>(data)->dismissed = true;
}
void popup_repositioned(void *, xdg_popup *, std::uint32_t) {}
const xdg_popup_listener popup_listener{
    .configure = popup_configure, .popup_done = popup_done, .repositioned = popup_repositioned};

void sync_done(void *data, wl_callback *, std::uint32_t) {
    *static_cast<bool *>(data) = true;
}
const wl_callback_listener sync_listener{.done = sync_done};

void frame_done(void *data, wl_callback *callback, std::uint32_t) {
    ++static_cast<Client *>(data)->frames;
    wl_callback_destroy(callback);
}
const wl_callback_listener frame_listener{.done = frame_done};

class Connection {
  public:
    explicit Connection(wl_display *server) : server_(server) {}

    bool connect(Client &client) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) < 0) {
            std::perror("Part 4 socketpair");
            return false;
        }
        if (!wl_client_create(server_, sockets[0])) {
            std::perror("Part 4 wl_client_create");
            close(sockets[0]);
            close(sockets[1]);
            return false;
        }
        client.display = wl_display_connect_to_fd(sockets[1]);
        if (!client.display) {
            std::perror("Part 4 wl_display_connect_to_fd");
            return false;
        }
        client.registry = wl_display_get_registry(client.display);
        wl_registry_add_listener(client.registry, &registry_listener, &client);
        return roundtrip(client) && client.compositor && client.shm && client.shell;
    }

    // Drive both ends of real Wayland connections from one thread. The test
    // has no sleeps or races with a separate owner of the compositor state.
    bool pump(Client &client) {
        if (wl_display_dispatch_pending(client.display) < 0 ||
            (wl_display_flush(client.display) < 0 && errno != EAGAIN) ||
            wl_event_loop_dispatch(wl_display_get_event_loop(server_), 5) < 0) {
            return false;
        }
        wl_display_flush_clients(server_);
        if (wl_display_prepare_read(client.display) != 0) {
            return wl_display_dispatch_pending(client.display) >= 0;
        }
        pollfd descriptor{.fd = wl_display_get_fd(client.display), .events = POLLIN, .revents = 0};
        const int status = poll(&descriptor, 1, 0);
        if (status <= 0 || !(descriptor.revents & POLLIN)) {
            wl_display_cancel_read(client.display);
            return status == 0 || (status < 0 && errno == EINTR);
        }
        return wl_display_read_events(client.display) >= 0 &&
               wl_display_dispatch_pending(client.display) >= 0;
    }

    template <class Predicate> bool wait(Client &client, Predicate ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            if (!pump(client)) {
                return false;
            }
        }
        return ready();
    }

    bool roundtrip(Client &client) {
        bool done = false;
        auto *callback = wl_display_sync(client.display);
        wl_callback_add_listener(callback, &sync_listener, &done);
        const bool complete = wait(client, [&] { return done; });
        wl_callback_destroy(callback);
        return complete;
    }

    bool open(Client &client) {
        client.surface = wl_compositor_create_surface(client.compositor);
        client.xdg = xdg_wm_base_get_xdg_surface(client.shell, client.surface);
        xdg_surface_add_listener(client.xdg, &surface_listener, &client);
        client.toplevel = xdg_surface_get_toplevel(client.xdg);
        xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
        wl_surface_commit(client.surface);
        return wait(client, [&] { return client.configures > 0; });
    }

    bool attach(Client &client, int width, int height, bool ack = true) {
        return attach(client, client.surface, client.xdg, ack ? client.serial : 0, width, height);
    }

    bool open_popup(Client &client, Popup &popup) {
        popup.surface = wl_compositor_create_surface(client.compositor);
        popup.xdg = xdg_wm_base_get_xdg_surface(client.shell, popup.surface);
        xdg_surface_add_listener(popup.xdg, &popup_surface_listener, &popup);
        auto *positioner = xdg_wm_base_create_positioner(client.shell);
        xdg_positioner_set_size(positioner, 12, 8);
        xdg_positioner_set_anchor_rect(positioner, 0, 0, 10, 10);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        popup.role = xdg_surface_get_popup(popup.xdg, client.xdg, positioner);
        xdg_positioner_destroy(positioner);
        xdg_popup_add_listener(popup.role, &popup_listener, &popup);
        wl_surface_commit(popup.surface);
        return wait(client, [&] { return popup.serial != 0; }) &&
               attach(client, popup.surface, popup.xdg, popup.serial, popup.geometry.width,
                      popup.geometry.height);
    }

  private:
    bool attach(Client &client, wl_surface *surface, xdg_surface *xdg, std::uint32_t serial,
                int width, int height) {
        const int stride = width * 4;
        const int length = stride * height;
        const int fd = memfd_create("seaside-part4-client", MFD_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        if (ftruncate(fd, length) < 0) {
            close(fd);
            return false;
        }
        auto *pool = wl_shm_create_pool(client.shm, fd, length);
        auto *buffer =
            wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);
        if (serial) {
            xdg_surface_ack_configure(xdg, serial);
        }
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, width, height);
        wl_surface_commit(surface);
        wl_buffer_destroy(buffer);
        return roundtrip(client);
    }

    wl_display *server_;
};

bool hit(seaside::Scene &scene, WindowRect geometry, WindowId id) {
    seaside_scene_target target{};
    return scene.window_at(geometry.x + 3, geometry.y + 3, target) && target.id == id &&
           target.surface == scene.window_surface(id) && target.sx == 3.0 && target.sy == 3.0;
}

Action action(ActionKind kind, std::int64_t argument = 0) {
    Action result{};
    result.kind = kind;
    result.argument = argument;
    return result;
}

bool run() {
    auto server = seaside::Server::create();
    REQUIRE(server);
    auto *display = (*server)->display();
    auto scene = seaside::Scene::create(display);
    REQUIRE(scene);
    auto *backend = part3_headless_backend_create(display);
    auto *renderer = part3_pixman_renderer_create();
    if (!backend || !renderer) {
        if (backend) {
            seaside_wlr_backend_destroy(backend);
        }
        if (renderer) {
            seaside_wlr_renderer_destroy(renderer);
        }
        return false;
    }
    REQUIRE((*server)->start(backend, renderer));
    seaside::Config config;
    config.window.tiling = false;
    seaside::Desktop desktop(**scene, config);
    const auto create_output = [&] {
        return part3_output_create(display, backend, (*server)->allocator(), renderer);
    };
    std::unique_ptr<part3_output, decltype(&part3_output_destroy)> output(create_output(),
                                                                          part3_output_destroy);
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "stable-headless",
                               output_bounds));
    const auto &windows = desktop.windows();
    const auto original_workspace = windows.focused_workspace();
    REQUIRE(original_workspace);

    Connection connection(display);
    Client first;
    REQUIRE(connection.connect(first));
    REQUIRE(connection.open(first));
    REQUIRE(first.configured.width == 0 && first.configured.height == 0 &&
            !first.configured.activated);
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    REQUIRE(connection.wait(first, [&] { return first.configured.activated; }));
    const auto a = windows.focused_window();
    REQUIRE(a);
    const auto first_geometry = windows.view(*a).geometry;
    REQUIRE(hit(**scene, first_geometry, *a));

    Client second;
    REQUIRE(connection.connect(second));
    REQUIRE(connection.open(second));
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    REQUIRE(connection.wait(second, [&] { return second.configured.activated; }));
    REQUIRE(connection.wait(first, [&] { return !first.configured.activated; }));
    const auto b = windows.focused_window();
    REQUIRE(b && b != a);
    const auto ordinary = windows.view(*b).geometry;
    REQUIRE(ordinary.width == ordinary_width && ordinary.height == ordinary_height);
    REQUIRE(hit(**scene, ordinary, *b));

    xdg_toplevel_set_maximized(second.toplevel);
    REQUIRE(connection.wait(second, [&] { return second.configured.maximized; }));
    REQUIRE(second.configured.width == output_bounds.width &&
            second.configured.height == output_bounds.height);
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    REQUIRE(windows.view(*b).geometry == output_bounds);
    xdg_toplevel_set_fullscreen(second.toplevel, nullptr);
    REQUIRE(connection.wait(second, [&] { return second.configured.fullscreen; }));
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    xdg_toplevel_unset_fullscreen(second.toplevel);
    REQUIRE(connection.wait(
        second, [&] { return !second.configured.fullscreen && second.configured.maximized; }));
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    xdg_toplevel_unset_maximized(second.toplevel);
    // Submit one last old-size buffer before the new configure is acked.
    REQUIRE(connection.attach(second, output_bounds.width, output_bounds.height, false));
    REQUIRE(connection.wait(second, [&] { return !second.configured.maximized; }));
    REQUIRE(second.configured.width == ordinary.width &&
            second.configured.height == ordinary.height);
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    REQUIRE(windows.view(*b).geometry == ordinary);

    REQUIRE(desktop.action(action(ActionKind::move_to_workspace, 42)));
    REQUIRE(windows.focused_window() == b && windows.focused_workspace() == 42);
    REQUIRE(windows.state().window(*b)->home_workspace == 42 && !windows.view(*a).visible);
    REQUIRE(desktop.action(action(ActionKind::workspace, *original_workspace)));
    REQUIRE(connection.wait(first, [&] { return first.configured.activated; }));
    REQUIRE(connection.wait(second, [&] { return !second.configured.activated; }));
    REQUIRE(windows.focused_window() == a && !windows.view(*b).visible);
    REQUIRE(hit(**scene, first_geometry, *a));

    {
        Popup popup;
        REQUIRE(connection.open_popup(first, popup));
        REQUIRE(popup.geometry == (WindowRect{10, 10, 12, 8}));
        seaside_scene_target target{};
        const double x = first_geometry.x + popup.geometry.x + 2;
        const double y = first_geometry.y + popup.geometry.y + 2;
        REQUIRE((*scene)->window_at(x, y, target));
        REQUIRE(target.id == a && target.surface != (*scene)->window_surface(*a));
        REQUIRE(target.sx == 2.0 && target.sy == 2.0);
        REQUIRE(desktop.action(action(ActionKind::workspace, 42)));
        REQUIRE(connection.wait(first, [&] { return popup.dismissed; }));
        REQUIRE(!windows.view(*a).visible);
        REQUIRE(desktop.action(action(ActionKind::workspace, *original_workspace)));
    }

    REQUIRE(desktop.action(action(ActionKind::toggle_window_pin)));
    REQUIRE(desktop.action(action(ActionKind::workspace, 42)));
    REQUIRE(windows.view(*a).visible && windows.view(*a).pinned);
    REQUIRE(windows.state().window(*a)->home_workspace == *original_workspace);
    REQUIRE(windows.focused_window() == b);
    desktop.remove_output(part3_output_handle(output.get()));
    output.reset();
    REQUIRE(!windows.focused_window() && !windows.focused_output());
    REQUIRE(!windows.view(*a).visible && !windows.view(*b).visible);
    REQUIRE(windows.state().window(*a)->mapped && windows.state().window(*b)->mapped);
    REQUIRE(connection.roundtrip(first) && connection.roundtrip(second));

    output.reset(create_output());
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "stable-headless",
                               output_bounds));
    REQUIRE(windows.focused_workspace() == 42 && windows.focused_window() == b);
    REQUIRE(windows.view(*a).visible && windows.view(*b).visible);
    REQUIRE(hit(**scene, ordinary, *b));
    auto *frame = wl_surface_frame(second.surface);
    wl_callback_add_listener(frame, &frame_listener, &second);
    wl_surface_damage_buffer(second.surface, 0, 0, ordinary.width, ordinary.height);
    wl_surface_commit(second.surface);
    REQUIRE(connection.wait(second, [&] { return second.frames == 1; }));

    const auto mapping = windows.state().window(*b)->initial_mapping_seq;
    wl_surface_attach(second.surface, nullptr, 0, 0);
    wl_surface_commit(second.surface);
    REQUIRE(connection.roundtrip(second));
    REQUIRE(!windows.state().window(*b)->mapped && !windows.view(*b).visible);
    const auto count = second.configures;
    wl_surface_commit(second.surface);
    REQUIRE(connection.wait(second, [&] { return second.configures > count; }));
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    REQUIRE(windows.state().window(*b)->mapped && windows.focused_window() == b);
    REQUIRE(windows.state().window(*b)->initial_mapping_seq == mapping);

    REQUIRE(desktop.action(action(ActionKind::close_window)));
    REQUIRE(connection.wait(second, [&] { return second.close_requested; }));
    second.destroy_window();
    REQUIRE(connection.roundtrip(second));
    REQUIRE(!windows.state().window(*b));
    REQUIRE(desktop.action(action(ActionKind::workspace, *original_workspace)));
    REQUIRE(windows.focused_window() == a);
    first.destroy_window();
    REQUIRE(connection.roundtrip(first));
    REQUIRE(windows.state().window_count() == 0);
    REQUIRE(!windows.focused_window() && windows.state().valid());
    REQUIRE(!desktop.failed() && !(*scene)->snapshot().failed);
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

} // namespace

int main() {
    return run() ? EXIT_SUCCESS : EXIT_FAILURE;
}

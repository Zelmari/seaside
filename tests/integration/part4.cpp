#include "desktop.hpp"
#include "live_config.hpp"
#include "part3_bridge.h"
#include "server.hpp"
#include "test_directory.hpp"
#include "tty_input.h"
#include "wlroots_bridge.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// The generated C function names an argument namespace.
#define namespace layer_namespace
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <linux/input-event-codes.h>
#include <memory>
#include <poll.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

using seaside::Action;
using seaside::ActionKind;
using seaside::Direction;
using seaside::WindowId;
using seaside::WindowRect;

constexpr WindowRect output_bounds{0, 0, 160, 90};
constexpr int ordinary_width = 48;
constexpr int ordinary_height = 32;
constexpr std::uint8_t super = 1;
constexpr unsigned all_tiled_edges = 0xf;
constexpr std::uint32_t red = 0xa03030;
constexpr std::uint32_t green = 0x30a030;
constexpr std::uint32_t blue = 0x3030a0;

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "Desktop fixture failed at %s:%d: %s\n", __FILE__, __LINE__,      \
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
    bool resizing = false;
    unsigned tiled_edges = 0;
};

struct Client {
    ~Client() {
        destroy_window();
        if (pointer) {
            wl_pointer_release(pointer);
        }
        if (keyboard) {
            wl_keyboard_release(keyboard);
        }
        if (seat) {
            wl_seat_release(seat);
        }
        for (auto *output : outputs) {
            if (output) {
                wl_output_destroy(output);
            }
        }
        if (layers) {
            zwlr_layer_shell_v1_destroy(layers);
        }
        if (data_devices) {
            wl_data_device_manager_destroy(data_devices);
        }
        if (shell) {
            xdg_wm_base_destroy(shell);
        }
        if (activation) {
            xdg_activation_v1_destroy(activation);
        }
        if (shm) {
            wl_shm_destroy(shm);
        }
        if (subcompositor) {
            wl_subcompositor_destroy(subcompositor);
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
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    xdg_activation_v1 *activation = nullptr;
    zwlr_layer_shell_v1 *layers = nullptr;
    wl_data_device_manager *data_devices = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_pointer *pointer = nullptr;
    std::array<wl_output *, 2> outputs{};
    wl_surface *keyboard_surface = nullptr;
    wl_surface *pointer_surface = nullptr;
    unsigned keys = 0;
    std::uint32_t button_serial = 0;
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

struct Subsurface {
    ~Subsurface() {
        if (role) {
            wl_subsurface_destroy(role);
        }
        if (surface) {
            wl_surface_destroy(surface);
        }
    }

    wl_surface *surface = nullptr;
    wl_subsurface *role = nullptr;
};

struct Layer {
    ~Layer() { destroy(); }
    void destroy() {
        if (role) {
            zwlr_layer_surface_v1_destroy(role);
            role = nullptr;
        }
        if (surface) {
            wl_surface_destroy(surface);
            surface = nullptr;
        }
    }
    void create(Client &client, wl_output *output, std::uint32_t level, std::uint32_t anchor,
                std::uint32_t width, std::uint32_t height, int zone) {
        surface = wl_compositor_create_surface(client.compositor);
        role = zwlr_layer_shell_v1_get_layer_surface(client.layers, surface, output, level,
                                                     "seaside-test");
        zwlr_layer_surface_v1_set_anchor(role, anchor);
        zwlr_layer_surface_v1_set_size(role, width, height);
        zwlr_layer_surface_v1_set_exclusive_zone(role, zone);
    }
    zwlr_layer_surface_v1 *role = nullptr;
    wl_surface *surface = nullptr;
    std::uint32_t serial = 0;
    int width = 0;
    int height = 0;
    bool closed = false;
};

void layer_configure(void *data, zwlr_layer_surface_v1 *, std::uint32_t serial, std::uint32_t width,
                     std::uint32_t height) {
    auto &layer = *static_cast<Layer *>(data);
    layer.serial = serial;
    layer.width = static_cast<int>(width);
    layer.height = static_cast<int>(height);
}

void layer_closed(void *data, zwlr_layer_surface_v1 *) {
    static_cast<Layer *>(data)->closed = true;
}

const zwlr_layer_surface_v1_listener layer_listener{.configure = layer_configure,
                                                    .closed = layer_closed};

void keyboard_keymap(void *, wl_keyboard *, std::uint32_t, int fd, std::uint32_t) {
    close(fd);
}
void keyboard_enter(void *data, wl_keyboard *, std::uint32_t, wl_surface *surface, wl_array *) {
    static_cast<Client *>(data)->keyboard_surface = surface;
}
void keyboard_leave(void *data, wl_keyboard *, std::uint32_t, wl_surface *) {
    static_cast<Client *>(data)->keyboard_surface = nullptr;
}
void keyboard_key(void *data, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
                  std::uint32_t state) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        ++static_cast<Client *>(data)->keys;
    }
}
void keyboard_modifiers(void *, wl_keyboard *, std::uint32_t, std::uint32_t, std::uint32_t,
                        std::uint32_t, std::uint32_t) {}
void keyboard_repeat(void *, wl_keyboard *, std::int32_t, std::int32_t) {}
const wl_keyboard_listener keyboard_listener{.keymap = keyboard_keymap,
                                             .enter = keyboard_enter,
                                             .leave = keyboard_leave,
                                             .key = keyboard_key,
                                             .modifiers = keyboard_modifiers,
                                             .repeat_info = keyboard_repeat};

void pointer_enter(void *data, wl_pointer *, std::uint32_t, wl_surface *surface, wl_fixed_t,
                   wl_fixed_t) {
    static_cast<Client *>(data)->pointer_surface = surface;
}
void pointer_leave(void *data, wl_pointer *, std::uint32_t, wl_surface *) {
    static_cast<Client *>(data)->pointer_surface = nullptr;
}
void pointer_motion(void *, wl_pointer *, std::uint32_t, wl_fixed_t, wl_fixed_t) {}
void pointer_button(void *data, wl_pointer *, std::uint32_t serial, std::uint32_t, std::uint32_t,
                    std::uint32_t state) {
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        static_cast<Client *>(data)->button_serial = serial;
    }
}
void pointer_axis(void *, wl_pointer *, std::uint32_t, std::uint32_t, wl_fixed_t) {}
void pointer_frame(void *, wl_pointer *) {}
void pointer_source(void *, wl_pointer *, std::uint32_t) {}
void pointer_stop(void *, wl_pointer *, std::uint32_t, std::uint32_t) {}
void pointer_discrete(void *, wl_pointer *, std::uint32_t, std::int32_t) {}
const auto pointer_listener = [] {
    wl_pointer_listener listener{};
    listener.enter = pointer_enter;
    listener.leave = pointer_leave;
    listener.motion = pointer_motion;
    listener.button = pointer_button;
    listener.axis = pointer_axis;
    listener.frame = pointer_frame;
    listener.axis_source = pointer_source;
    listener.axis_stop = pointer_stop;
    listener.axis_discrete = pointer_discrete;
    return listener;
}();

void seat_capabilities(void *data, wl_seat *seat, std::uint32_t capabilities) {
    auto &client = *static_cast<Client *>(data);
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD && !client.keyboard) {
        client.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client.keyboard, &keyboard_listener, &client);
    }
    if (capabilities & WL_SEAT_CAPABILITY_POINTER && !client.pointer) {
        client.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client.pointer, &pointer_listener, &client);
    }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener seat_listener{.capabilities = seat_capabilities, .name = seat_name};

void output_geometry(void *, wl_output *, std::int32_t, std::int32_t, std::int32_t, std::int32_t,
                     std::int32_t, const char *, const char *, std::int32_t) {}
void output_mode(void *, wl_output *, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
const wl_output_listener output_listener{.geometry = output_geometry,
                                         .mode = output_mode,
                                         .done = nullptr,
                                         .scale = nullptr,
                                         .name = nullptr,
                                         .description = nullptr};

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
    } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
        client.subcompositor = static_cast<wl_subcompositor *>(
            wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        client.shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0 && version >= 6) {
        client.shell =
            static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 6));
        xdg_wm_base_add_listener(client.shell, &shell_listener, nullptr);
    } else if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0) {
        client.activation = static_cast<xdg_activation_v1 *>(
            wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        client.layers = static_cast<zwlr_layer_shell_v1 *>(wl_registry_bind(
            registry, name, &zwlr_layer_shell_v1_interface, std::min(version, 5u)));
    } else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0 && version >= 3) {
        client.data_devices = static_cast<wl_data_device_manager *>(
            wl_registry_bind(registry, name, &wl_data_device_manager_interface, 3));
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && version >= 5) {
        client.seat =
            static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 5));
        wl_seat_add_listener(client.seat, &seat_listener, &client);
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        const auto found = std::ranges::find(client.outputs, nullptr);
        if (found != client.outputs.end()) {
            *found =
                static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, 1));
            wl_output_add_listener(*found, &output_listener, nullptr);
        }
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
        client.pending.resizing |= state[i] == XDG_TOPLEVEL_STATE_RESIZING;
        if (state[i] >= XDG_TOPLEVEL_STATE_TILED_LEFT &&
            state[i] <= XDG_TOPLEVEL_STATE_TILED_BOTTOM) {
            client.pending.tiled_edges |= 1u << (state[i] - XDG_TOPLEVEL_STATE_TILED_LEFT);
        }
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
            std::perror("desktop socketpair");
            return false;
        }
        if (!wl_client_create(server_, sockets[0])) {
            std::perror("desktop wl_client_create");
            close(sockets[0]);
            close(sockets[1]);
            return false;
        }
        client.display = wl_display_connect_to_fd(sockets[1]);
        if (!client.display) {
            std::perror("desktop wl_display_connect_to_fd");
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

    bool open(Client &client, int minimum_width = 0) {
        client.surface = wl_compositor_create_surface(client.compositor);
        client.xdg = xdg_wm_base_get_xdg_surface(client.shell, client.surface);
        xdg_surface_add_listener(client.xdg, &surface_listener, &client);
        client.toplevel = xdg_surface_get_toplevel(client.xdg);
        xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
        if (minimum_width) {
            xdg_toplevel_set_min_size(client.toplevel, minimum_width, 0);
        }
        wl_surface_commit(client.surface);
        return wait(client, [&] { return client.configures > 0; });
    }

    bool attach(Client &client, int width, int height, bool ack = true, std::uint32_t rgb = 0) {
        return attach(client, client.surface, client.xdg, ack ? client.serial : 0, width, height,
                      rgb);
    }

    bool attach_configured(Client &client, std::uint32_t rgb = 0) {
        return attach(client, client.configured.width, client.configured.height, true, rgb);
    }

    bool attach_subsurface(Client &client, Subsurface &subsurface, int width, int height,
                           std::uint32_t rgb) {
        return attach(client, subsurface.surface, nullptr, 0, width, height, rgb);
    }

    bool attach_layer(Client &client, Layer &layer, std::uint32_t rgb) {
        zwlr_layer_surface_v1_ack_configure(layer.role, layer.serial);
        return attach(client, layer.surface, nullptr, 0, layer.width, layer.height, rgb);
    }

    bool open_popup(Client &client, Popup &popup, Layer *layer = nullptr, std::uint32_t grab = 0) {
        popup.surface = wl_compositor_create_surface(client.compositor);
        popup.xdg = xdg_wm_base_get_xdg_surface(client.shell, popup.surface);
        xdg_surface_add_listener(popup.xdg, &popup_surface_listener, &popup);
        auto *positioner = xdg_wm_base_create_positioner(client.shell);
        xdg_positioner_set_size(positioner, 12, 8);
        xdg_positioner_set_anchor_rect(positioner, 0, 0, 10, 10);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        popup.role = xdg_surface_get_popup(popup.xdg, layer ? nullptr : client.xdg, positioner);
        xdg_positioner_destroy(positioner);
        xdg_popup_add_listener(popup.role, &popup_listener, &popup);
        if (layer) {
            zwlr_layer_surface_v1_get_popup(layer->role, popup.role);
        }
        if (grab) {
            xdg_popup_grab(popup.role, client.seat, grab);
        }
        wl_surface_commit(popup.surface);
        return wait(client, [&] { return popup.serial != 0; }) &&
               attach(client, popup.surface, popup.xdg, popup.serial, popup.geometry.width,
                      popup.geometry.height);
    }

  private:
    bool attach(Client &client, wl_surface *surface, xdg_surface *xdg, std::uint32_t serial,
                int width, int height, std::uint32_t rgb = 0) {
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            return false;
        }
        const int stride = width * 4;
        const int length = stride * height;
        const int fd = memfd_create("seaside-desktop-client", MFD_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        if (ftruncate(fd, length) < 0) {
            close(fd);
            return false;
        }
        if (rgb) {
            void *data =
                mmap(nullptr, static_cast<std::size_t>(length), PROT_WRITE, MAP_SHARED, fd, 0);
            if (data == MAP_FAILED) {
                close(fd);
                return false;
            }
            std::fill_n(static_cast<std::uint32_t *>(data), width * height, rgb);
            munmap(data, static_cast<std::size_t>(length));
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

Action directional_action(ActionKind kind, Direction direction, int distance = 0) {
    auto result = action(kind, distance);
    result.direction = direction;
    return result;
}

using Output = std::unique_ptr<part3_output, decltype(&part3_output_destroy)>;

struct Fixture {
    wl_display *display;
    seaside::Scene &scene;
    seaside::Desktop &desktop;
    wlr_backend *backend;
    wlr_allocator *allocator;
    wlr_renderer *renderer;

    Output output() const {
        return Output(part3_output_create(display, backend, allocator, renderer),
                      part3_output_destroy);
    }
};

bool run_fixture(const seaside::Config &config, bool (*scenario)(const Fixture &)) {
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
    seaside::Desktop desktop(**scene, config);
    return scenario({display, **scene, desktop, backend, (*server)->allocator(), renderer});
}

bool run_part4(const Fixture &fixture) {
    auto &scene = fixture.scene;
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "stable-headless",
                               output_bounds));
    const auto &windows = desktop.windows();
    const auto original_workspace = windows.focused_workspace();
    REQUIRE(original_workspace);

    Connection connection(fixture.display);
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
    REQUIRE(hit(scene, first_geometry, *a));

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
    REQUIRE(hit(scene, ordinary, *b));

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
    REQUIRE(hit(scene, first_geometry, *a));

    {
        Popup popup;
        REQUIRE(connection.open_popup(first, popup));
        REQUIRE(popup.geometry == (WindowRect{10, 10, 12, 8}));
        seaside_scene_target target{};
        const double x = first_geometry.x + popup.geometry.x + 2;
        const double y = first_geometry.y + popup.geometry.y + 2;
        REQUIRE(scene.window_at(x, y, target));
        REQUIRE(target.id == a && target.surface != scene.window_surface(*a));
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

    output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "stable-headless",
                               output_bounds));
    REQUIRE(windows.focused_workspace() == 42 && windows.focused_window() == b);
    REQUIRE(windows.view(*a).visible && windows.view(*b).visible);
    REQUIRE(hit(scene, ordinary, *b));
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
    REQUIRE(!desktop.failed() && !scene.snapshot().failed);
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

bool run_workspace_reassignment(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    const WindowRect right_bounds{output_bounds.width, 0, output_bounds.width,
                                  output_bounds.height};
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", "", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", "", right_bounds));
    const auto migrated = windows.state().output("right")->shown;
    REQUIRE(migrated);

    desktop.remove_output(part3_output_handle(right.get()));
    right.reset();
    REQUIRE(windows.workspace_output(*migrated) == "left");
    REQUIRE(windows.workspace_owner(*migrated) == "right");
    auto transfer = action(ActionKind::transfer_workspace);
    transfer.text = "connector:left";
    const auto changed = desktop.dispatch(transfer, 0, {}, migrated);
    REQUIRE(changed && *changed);
    REQUIRE(windows.workspace_owner(*migrated) == "left");
    REQUIRE(windows.focused_workspace() == migrated);
    const auto current = desktop.dispatch(transfer, 0, {}, migrated);
    REQUIRE(current && !*current);

    right = fixture.output();
    REQUIRE(right);
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", "", right_bounds));
    REQUIRE(windows.workspace_output(*migrated) == "left");
    REQUIRE(windows.state().output("left")->shown == migrated);
    REQUIRE(windows.focused_workspace() == migrated);
    REQUIRE(windows.state().valid() && !desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(part3_output_handle(right.get()));
    desktop.remove_output(part3_output_handle(left.get()));
    return true;
}

bool wait_tiled(Connection &connection, Client &client, int width, int height) {
    return connection.wait(client, [&] {
        return client.configured.tiled_edges == all_tiled_edges &&
               client.configured.width == width && client.configured.height == height;
    });
}

bool map_tiled(const Fixture &fixture, Connection &connection, Client &client, WindowId &id,
               int width, int height, int minimum_width = 0, std::uint32_t rgb = 0) {
    REQUIRE(connection.connect(client));
    REQUIRE(connection.open(client, minimum_width));
    REQUIRE(client.configured.width == width && client.configured.height == height);
    REQUIRE(client.configured.tiled_edges == all_tiled_edges && !client.configured.activated);
    REQUIRE(connection.attach_configured(client, rgb));
    id = fixture.desktop.windows().focused_window().value_or(0);
    REQUIRE(id);
    REQUIRE(connection.wait(client, [&] { return client.configured.activated; }));
    REQUIRE(wait_tiled(connection, client, width, height));
    REQUIRE(connection.attach_configured(client, rgb));
    return true;
}

bool begin_drag(const Fixture &fixture, WindowId id, std::uint32_t button, double x, double y) {
    seaside_scene_target target{};
    REQUIRE(fixture.scene.window_at(x, y, target) && target.id == id);
    return fixture.desktop.button(target.id, button, true, super, x, y);
}

bool wait_pixel(Connection &connection, Client &client, part3_output *output, unsigned x,
                unsigned y, std::uint32_t expected) {
    REQUIRE(part3_output_sample_pixel(output, x, y));
    const bool ready = connection.wait(client, [&] {
        const auto pixel = part3_output_get_pixel(output);
        return pixel.frames && pixel.rgb == expected;
    });
    if (!ready) {
        const auto pixel = part3_output_get_pixel(output);
        std::fprintf(stderr, "Pixel at (%u, %u) expected %06x, got %06x after %u frames\n", x, y,
                     expected, pixel.rgb, pixel.frames);
    }
    return ready;
}

bool run_opening_transaction(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", "main", output_bounds));
    Connection connection(fixture.display);
    Client first, second;
    WindowId left{};
    REQUIRE(map_tiled(fixture, connection, first, left, 160, 90, 0, red));
    REQUIRE(connection.connect(second) && connection.open(second));
    REQUIRE(second.configured.width == 80 && second.configured.height == 90 &&
            second.configured.tiled_edges == all_tiled_edges);
    REQUIRE(connection.attach_configured(second, green));
    REQUIRE(wait_tiled(connection, first, 80, 90));
    REQUIRE(!second.configured.activated);
    // Keep the previous layout until the existing client submits its
    // response. The new client already has a buffer at the right size.
    REQUIRE(wait_pixel(connection, first, output.get(), 110, 55, red));
    const auto right = desktop.windows().focused_window();
    REQUIRE(right && right != left);
    REQUIRE(!desktop.button(left, BTN_LEFT, true, 0, 110, 45));
    REQUIRE(!desktop.button(left, BTN_LEFT, false, 0, 110, 45));
    seaside_scene_target shown{};
    REQUIRE(fixture.scene.window_at(110, 45, shown) && shown.id == left);
    REQUIRE(desktop.windows().focused_window() == left);
    REQUIRE(connection.attach_configured(first, red));
    REQUIRE(wait_pixel(connection, second, output.get(), 110, 55, green));
    REQUIRE(wait_pixel(connection, first, output.get(), 40, 55, red));

    desktop.enable_frame_paced_resize();
    REQUIRE(begin_drag(fixture, *right, BTN_RIGHT, 110, 45));
    REQUIRE(connection.wait(second, [&] { return second.configured.activated; }));
    const auto pump_frames = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(35);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!connection.pump(first)) {
                return false;
            }
        }
        return true;
    };
    REQUIRE(pump_frames());
    unsigned commits = 0;
    fixture.scene.set_frame_observer(
        [](void *data, wlr_output *, const seaside_scene_frame_observation *frame) {
            if (std::strcmp(frame->event, "commit") == 0 && frame->succeeded) {
                ++*static_cast<unsigned *>(data);
            }
        },
        &commits);
    desktop.motion(110, 45);
    const bool stationary_pumped = pump_frames();
    const auto stationary_commits = commits;
    desktop.motion(110.2, 45);
    const bool fractional_pumped = pump_frames();
    const auto fractional_commits = commits;
    desktop.motion(110.2, 45);
    const bool repeated_pumped = pump_frames();
    fixture.scene.set_frame_observer(nullptr, nullptr);
    REQUIRE(stationary_pumped && fractional_pumped && repeated_pumped);
    REQUIRE(stationary_commits == 0 && commits == fractional_commits);

    // The fractional part remains available to the next sampled position.
    REQUIRE(desktop.windows().view(left).geometry.width == 80);
    desktop.motion(111.1, 45);
    REQUIRE(
        connection.wait(first, [&] { return desktop.windows().view(left).geometry.width == 81; }));
    REQUIRE(connection.wait(
        second, [&] { return second.configured.resizing && second.configured.width == 79; }));
    desktop.motion(115, 45);
    desktop.motion(130, 45);
    REQUIRE(desktop.windows().view(left).geometry.width == 81);
    REQUIRE(
        connection.wait(first, [&] { return desktop.windows().view(left).geometry.width == 100; }));
    REQUIRE(wait_pixel(connection, first, output.get(), 98, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), 102, 45, green));

    desktop.motion(135, 45);
    desktop.button(0, BTN_RIGHT, false, super, 135, 45);
    REQUIRE(desktop.windows().view(left).geometry.width == 105);
    REQUIRE(connection.wait(
        second, [&] { return !second.configured.resizing && second.configured.width == 55; }));
    REQUIRE(wait_pixel(connection, first, output.get(), 103, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), 107, 45, green));

    Client third;
    REQUIRE(connection.connect(third) && connection.open(third));
    REQUIRE(third.configured.tiled_edges == all_tiled_edges);
    REQUIRE(connection.attach_configured(third, blue));
    REQUIRE(desktop.action(action(ActionKind::toggle_window_tiling)));
    REQUIRE(connection.wait(third, [&] {
        return !third.configured.tiled_edges && !third.configured.width && !third.configured.height;
    }));
    REQUIRE(connection.attach(third, ordinary_width, ordinary_height, true, blue));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    return true;
}

bool run_floating_borders(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "", output_bounds));
    Connection connection(fixture.display);
    Client first, second;
    REQUIRE(connection.connect(first) && connection.open(first));
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    const auto a = desktop.windows().focused_window();
    REQUIRE(a);
    const auto geometry = desktop.windows().view(*a).geometry;
    const auto x = static_cast<unsigned>(geometry.x - 1);
    const auto y = static_cast<unsigned>(geometry.y + 4);
    constexpr std::uint32_t active_border = 0xbfbfbf;
    constexpr std::uint32_t inactive_border = 0x595959;
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, active_border));
    seaside_scene_target border{};
    REQUIRE(fixture.scene.window_at(x, y, border) && border.id == a && !border.surface);

    REQUIRE(connection.connect(second) && connection.open(second));
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    const auto b = desktop.windows().focused_window();
    REQUIRE(b && b != a);
    REQUIRE(desktop.action(directional_action(ActionKind::move_window, Direction::right, 24)));
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, inactive_border));

    auto config = seaside::default_config();
    config.window.tiling = false;
    config.window.border_width = 0;
    desktop.apply_config(config);
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, 0));
    REQUIRE(hit(fixture.scene, geometry, *a));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

struct LayerSeat {
    explicit LayerSeat(const Fixture &display) : fixture(display) {}
    ~LayerSeat() { fixture.desktop.set_input(nullptr); }

    bool start() {
        const seaside_tty_input_callbacks callbacks{
            .motion = nullptr,
            .exit_requested = nullptr,
            .launch_requested = nullptr,
            .shortcut_pressed =
                [](void *data, std::uint8_t mods, std::uint32_t keysym) {
                    auto &seat = *static_cast<LayerSeat *>(data);
                    if (keysym != XKB_KEY_F8) {
                        return false;
                    }
                    ++seat.shortcuts;
                    return seat.fixture.desktop.action(action(ActionKind::toggle_window_fullscreen),
                                                       mods);
                },
            .modifiers_changed = nullptr,
            .pointer_target =
                [](void *data, seaside_tty_pointer_target *target) {
                    const auto &seat = *static_cast<LayerSeat *>(data);
                    seaside_scene_target hit{.surface = target->surface, .id = 0, .sx = 0, .sy = 0};
                    if (!seat.fixture.scene.window_at(seat.x, seat.y, hit)) {
                        return false;
                    }
                    *target = {hit.surface, hit.sx, hit.sy, hit.id};
                    return true;
                },
            .pointer_button =
                [](void *data, std::uint64_t window, std::uint32_t button, bool pressed,
                   std::uint8_t mods) {
                    auto &seat = *static_cast<LayerSeat *>(data);
                    return seat.fixture.desktop.button(window, button, pressed, mods, seat.x,
                                                       seat.y);
                },
            .pointer_absolute_delta = nullptr,
            .pointer_locked = nullptr,
            .cursor_shape = nullptr,
            .cursor_surface = nullptr,
            .pointer_cancelled = nullptr,
            .drag_icon = nullptr,
            .activity = nullptr,
        };
        input.reset(seaside_tty_input_create(fixture.backend, &callbacks, this));
        if (!input || !seaside_tty_input_enable_keyboard(input.get(), fixture.display)) {
            return false;
        }
        devices.reset(part3_input_create(fixture.backend));
        fixture.desktop.set_input(input.get());
        return devices && !seaside_tty_input_get_snapshot(input.get()).failed;
    }

    void key(std::uint32_t code) const {
        part3_input_key(devices.get(), code, true);
        part3_input_key(devices.get(), code, false);
    }

    void click(double px, double py) {
        x = px;
        y = py;
        part3_input_button(devices.get(), BTN_LEFT, true);
        part3_input_button(devices.get(), BTN_LEFT, false);
    }

    const Fixture &fixture;
    std::unique_ptr<seaside_tty_input, decltype(&seaside_tty_input_destroy)> input{
        nullptr, seaside_tty_input_destroy};
    std::unique_ptr<part3_input, decltype(&part3_input_destroy)> devices{nullptr,
                                                                         part3_input_destroy};
    double x = 1, y = 1;
    unsigned shortcuts = 0;
};

bool run_pending_opening_focus(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto right_bounds = output_bounds;
    right_bounds.x = output_bounds.width;
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", "", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", "", right_bounds));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());

    Connection connection(fixture.display);
    Client first, second, remote, opening;
    WindowId remote_id{}, first_id{};
    seat.click(210, 45);
    REQUIRE(map_tiled(fixture, connection, remote, remote_id, 160, 90, 0, blue));
    seat.click(30, 45);
    REQUIRE(map_tiled(fixture, connection, first, first_id, 160, 90, 0, red));
    REQUIRE(connection.wait(first, [&] { return first.pointer_surface == first.surface; }));

    REQUIRE(connection.connect(second) && connection.open(second));
    REQUIRE(connection.attach_configured(second, blue));
    REQUIRE(wait_tiled(connection, first, 80, 90));
    REQUIRE(connection.attach_configured(first, red));
    REQUIRE(connection.wait(second, [&] { return second.keyboard_surface == second.surface; }));
    const auto second_id = desktop.windows().focused_window();
    REQUIRE(second_id && second_id != first_id);

    REQUIRE(connection.connect(opening) && connection.open(opening));
    REQUIRE(connection.attach_configured(opening, green));
    REQUIRE(wait_tiled(connection, second, 80, 45));

    // Both keys must go to the visible client just clicked, before dispatch
    // can process a resize buffer or the opening deadline.
    seat.click(30, 45);
    seat.key(KEY_A);
    seat.click(210, 45);
    seat.key(KEY_B);
    seaside_scene_target shown{};
    REQUIRE(fixture.scene.window_at(110, 70, shown) && shown.id == second_id);
    REQUIRE(connection.roundtrip(first) && connection.roundtrip(second) &&
            connection.roundtrip(remote));
    REQUIRE(first.keys == 1 && second.keys == 0 && remote.keys == 1);
    REQUIRE(remote.keyboard_surface == remote.surface);

    REQUIRE(connection.attach_configured(second, blue));
    REQUIRE(wait_pixel(connection, opening, left.get(), 110, 70, green));
    REQUIRE(desktop.windows().focused_window() == remote_id);
    desktop.remove_output(part3_output_handle(right.get()));
    desktop.remove_output(part3_output_handle(left.get()));
    return true;
}

bool run_client_gestures(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", "main", output_bounds));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client));
    REQUIRE(connection.open(client));
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height));
    const auto &windows = desktop.windows();
    const auto id = windows.focused_window();
    REQUIRE(id && !windows.view(*id).tiled);

    const auto initial = windows.view(*id).geometry;
    seat.x = initial.x + 10;
    seat.y = initial.y + 10;
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    REQUIRE(connection.wait(client, [&] { return client.button_serial != 0; }));
    const auto move_serial = client.button_serial;
    xdg_toplevel_move(client.toplevel, client.seat, move_serial);
    REQUIRE(connection.wait(client, [&] { return client.pointer_surface == nullptr; }));
    seat.x += 12;
    seat.y += 7;
    desktop.motion(seat.x, seat.y);
    const auto moved = windows.view(*id).geometry;
    REQUIRE(moved == (WindowRect{initial.x + 12, initial.y + 7, initial.width, initial.height}));
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    REQUIRE(connection.wait(client, [&] { return client.pointer_surface == client.surface; }));

    // A serial from a completed press cannot start another grab.
    xdg_toplevel_move(client.toplevel, client.seat, move_serial);
    REQUIRE(connection.roundtrip(client));
    desktop.motion(seat.x + 5, seat.y + 5);
    REQUIRE(windows.view(*id).geometry == moved);

    xdg_toplevel_set_min_size(client.toplevel, 40, 26);
    wl_surface_commit(client.surface);
    REQUIRE(connection.roundtrip(client));
    seat.x = moved.x + 6;
    seat.y = moved.y + 6;
    client.button_serial = 0;
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    REQUIRE(connection.wait(client, [&] { return client.button_serial != 0; }));
    xdg_toplevel_resize(client.toplevel, client.seat, move_serial,
                        XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
    REQUIRE(connection.roundtrip(client) && !client.configured.resizing &&
            client.pointer_surface == client.surface);
    xdg_toplevel_resize(client.toplevel, client.seat, client.button_serial,
                        XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
    REQUIRE(connection.wait(client, [&] { return client.configured.resizing; }));
    seat.x -= 6;
    seat.y -= 4;
    desktop.motion(seat.x, seat.y);
    const auto expanded = windows.view(*id).geometry;
    REQUIRE(expanded == (WindowRect{moved.x - 6, moved.y - 4, moved.width + 6, moved.height + 4}));
    REQUIRE(connection.wait(client, [&] {
        return client.configured.width == expanded.width &&
               client.configured.height == expanded.height;
    }));
    REQUIRE(connection.attach_configured(client));
    seat.x += 32;
    seat.y += 18;
    desktop.motion(seat.x, seat.y);
    const auto clamped = windows.view(*id).geometry;
    REQUIRE(clamped ==
            (WindowRect{moved.x + moved.width - 40, moved.y + moved.height - 26, 40, 26}));
    REQUIRE(connection.wait(
        client, [&] { return client.configured.width == 40 && client.configured.height == 26; }));
    REQUIRE(connection.attach_configured(client));
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    REQUIRE(connection.wait(client, [&] { return !client.configured.resizing; }));
    REQUIRE(windows.view(*id).geometry == clamped);

    seat.x = clamped.x + 5;
    seat.y = clamped.y + 5;
    client.button_serial = 0;
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    REQUIRE(connection.wait(client, [&] { return client.button_serial != 0; }));
    xdg_toplevel_resize(client.toplevel, client.seat, client.button_serial,
                        XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    REQUIRE(connection.wait(client, [&] { return client.configured.resizing; }));
    desktop.apply_config(seaside::default_config());
    REQUIRE(connection.wait(client, [&] {
        return !client.configured.resizing && client.configured.tiled_edges == all_tiled_edges;
    }));
    const auto tiled = windows.view(*id).geometry;
    desktop.motion(seat.x + 8, seat.y + 8);
    REQUIRE(windows.view(*id).geometry == tiled);
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    return true;
}

bool map_layer(Connection &connection, Client &client, Layer &layer, int width, int height,
               std::uint32_t color) {
    zwlr_layer_surface_v1_add_listener(layer.role, &layer_listener, &layer);
    wl_surface_commit(layer.surface);
    return connection.wait(client, [&] { return layer.serial != 0; }) && layer.width == width &&
           layer.height == height && connection.attach_layer(client, layer, color);
}

bool run_layer_shell(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto &scene = fixture.scene;
    auto primary = fixture.output();
    auto other = fixture.output();
    auto *first = part3_output_handle(primary.get());
    auto *second = part3_output_handle(other.get());
    REQUIRE(first && second && desktop.add_output(first, "HEADLESS-1", "", output_bounds) &&
            desktop.add_output(second, "HEADLESS-2", "", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && client.layers && client.outputs[0] && client.outputs[1]);
    REQUIRE(connection.wait(client, [&] { return client.keyboard && client.pointer; }));

    constexpr auto top = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
    constexpr auto bottom = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    constexpr auto left = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
    constexpr auto right = ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;

    seat.click(190, 80);
    Layer wallpaper;
    wallpaper.create(client, nullptr, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
                     top | bottom | left | right, 0, 0, -1);
    REQUIRE(map_layer(connection, client, wallpaper, 160, 90, green));
    REQUIRE(wait_pixel(connection, client, other.get(), 3, 3, green));
    seat.click(10, 80);

    Layer underlay;
    underlay.create(client, client.outputs[0], ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, 0, 40, 30, 0);
    REQUIRE(map_layer(connection, client, underlay, 40, 30, blue));
    REQUIRE(wait_pixel(connection, client, primary.get(), 80, 45, blue));

    Layer panel;
    panel.create(client, client.outputs[0], ZWLR_LAYER_SHELL_V1_LAYER_TOP, top | left | right, 0,
                 12, 12);
    zwlr_layer_surface_v1_set_margin(panel.role, 4, 5, 0, 3);
    REQUIRE(map_layer(connection, client, panel, 152, 12, green));
    seaside_scene_target panel_hit{};
    REQUIRE(scene.window_at(4, 5, panel_hit) && !panel_hit.id && panel_hit.sx == 1 &&
            panel_hit.sy == 1 && scene.visible_output(panel_hit.surface) == first);
    REQUIRE(wait_pixel(connection, client, primary.get(), 4, 5, green));

    REQUIRE(connection.open(client) && connection.attach(client, 50, 40, true, red));
    const auto id = desktop.windows().focused_window().value_or(0);
    const WindowRect reserved{0, 16, 160, 74};
    REQUIRE(id && desktop.windows().view(id).geometry == reserved);
    REQUIRE(connection.wait(client, [&] { return client.configured.height == reserved.height; }));
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(wait_pixel(connection, client, primary.get(), 80, 45, red));
    REQUIRE(client.keyboard_surface == client.surface);

    seat.click(4, 5);
    REQUIRE(connection.roundtrip(client) && client.pointer_surface == panel.surface &&
            client.keyboard_surface == client.surface && client.button_serial);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        panel.role, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    wl_surface_commit(panel.surface);
    REQUIRE(connection.roundtrip(client));
    seat.click(4, 5);
    REQUIRE(connection.wait(client, [&] { return client.keyboard_surface == panel.surface; }));
    seat.key(KEY_A);
    REQUIRE(connection.wait(client, [&] { return client.keys == 1; }));

    Popup popup;
    REQUIRE(connection.open_popup(client, popup, &panel, client.button_serial));
    seaside_scene_target popup_hit{};
    REQUIRE(scene.window_at(16, 17, popup_hit) && popup_hit.id == 0 && popup_hit.sx == 3 &&
            popup_hit.sy == 3 && scene.visible_output(popup_hit.surface) == first);
    seat.key(KEY_F8);
    REQUIRE(connection.roundtrip(client) && seat.shortcuts == 1 && client.keys == 1 &&
            !client.configured.fullscreen && !popup.dismissed);

    const auto fullscreen = desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, id);
    REQUIRE(fullscreen && *fullscreen);
    REQUIRE(
        connection.wait(client, [&] { return popup.dismissed && client.configured.fullscreen; }));
    REQUIRE(seat.shortcuts == 1 && client.keys == 1 && client.keyboard_surface == client.surface &&
            desktop.windows().view(id).geometry == output_bounds);
    REQUIRE(!scene.visible_output(panel_hit.surface));
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(wait_pixel(connection, client, primary.get(), 4, 5, red));
    REQUIRE(wait_pixel(connection, client, other.get(), 3, 3, green));

    Layer overlay;
    overlay.create(client, client.outputs[0], ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, top, 40, 10, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        overlay.role, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    REQUIRE(map_layer(connection, client, overlay, 40, 10, blue));
    REQUIRE(connection.wait(client, [&] { return client.keyboard_surface == overlay.surface; }));
    REQUIRE(wait_pixel(connection, client, primary.get(), 80, 5, blue));

    const auto close = action(ActionKind::close_window);
    const auto implicit = desktop.dispatch(close);
    REQUIRE(!implicit && implicit.error() == seaside::DesktopActionError::no_target);
    REQUIRE(connection.roundtrip(client) && !client.close_requested);
    const auto explicit_target = desktop.dispatch(close, 0, id);
    REQUIRE(explicit_target && !*explicit_target);
    REQUIRE(connection.wait(client, [&] { return client.close_requested; }));
    REQUIRE(client.keyboard_surface == overlay.surface);

    overlay.destroy();
    REQUIRE(connection.wait(client, [&] { return client.keyboard_surface == client.surface; }));

    seat.key(KEY_F8);
    REQUIRE(connection.wait(client, [&] { return !client.configured.fullscreen; }));
    REQUIRE(seat.shortcuts == 2 && client.keys == 1 &&
            desktop.windows().view(id).geometry == reserved);
    REQUIRE(wait_pixel(connection, client, primary.get(), 4, 5, green));
    REQUIRE(client.data_devices);
    const std::unique_ptr<wl_data_device, decltype(&wl_data_device_release)> data_device{
        wl_data_device_manager_get_data_device(client.data_devices, client.seat),
        wl_data_device_release};
    client.button_serial = 0;
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    REQUIRE(connection.wait(client, [&] { return client.button_serial != 0; }));
    wl_data_device_start_drag(data_device.get(), nullptr, panel.surface, nullptr,
                              client.button_serial);
    const auto *server_seat = seaside_tty_input_get_seat(seat.input.get());
    REQUIRE(connection.wait(client, [&] { return part3_seat_has_drag(server_seat); }));
    const auto fullscreen_during_drag =
        desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, id);
    REQUIRE(fullscreen_during_drag && *fullscreen_during_drag);
    REQUIRE(connection.wait(client, [&] { return client.configured.fullscreen; }));
    REQUIRE(!part3_seat_has_drag(server_seat));
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    seat.key(KEY_F8);
    REQUIRE(connection.wait(client, [&] { return !client.configured.fullscreen; }));

    zwlr_layer_surface_v1_set_exclusive_zone(panel.role, 20);
    wl_surface_commit(panel.surface);
    REQUIRE(connection.wait(client, [&] { return desktop.windows().view(id).geometry.y == 24; }));
    xdg_toplevel_set_maximized(client.toplevel);
    REQUIRE(connection.wait(client, [&] { return client.configured.maximized; }));
    REQUIRE(client.configured.width == 160 && client.configured.height == 66);

    const auto old_serial = panel.serial;
    REQUIRE(part3_output_set_scale(primary.get(), 1.25F) &&
            part3_output_set_transform(primary.get(), WL_OUTPUT_TRANSFORM_90) &&
            desktop.update_output(first, {0, 0, 72, 128}));
    REQUIRE(
        connection.wait(client, [&] { return panel.serial != old_serial && panel.width == 64; }));
    REQUIRE(panel.height == 12 && desktop.windows().view(id).geometry.height == 104);
    wl_surface_attach(panel.surface, nullptr, 0, 0);
    wl_surface_commit(panel.surface);
    REQUIRE(
        connection.wait(client, [&] { return desktop.windows().view(id).geometry.height == 128; }));
    REQUIRE(!scene.visible_output(panel_hit.surface));

    desktop.remove_output(second);
    REQUIRE(connection.wait(client, [&] { return wallpaper.closed; }));
    REQUIRE(!desktop.failed() && !scene.snapshot().failed);
    return true;
}

bool run_fullscreen_aspect(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto &scene = fixture.scene;
    auto output = fixture.output();
    auto *handle = part3_output_handle(output.get());
    REQUIRE(handle && desktop.add_output(handle, "HEADLESS-1", "", output_bounds));

    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && client.subcompositor && connection.open(client));
    REQUIRE(connection.attach(client, 60, 60, true, red));
    const auto id = desktop.windows().focused_window().value_or(0);
    REQUIRE(id);
    const auto ordinary = desktop.windows().view(id).geometry;
    REQUIRE(ordinary.width == 60 && ordinary.height == 60);

    Subsurface child;
    child.surface = wl_compositor_create_surface(client.compositor);
    child.role =
        wl_subcompositor_get_subsurface(client.subcompositor, child.surface, client.surface);
    wl_subsurface_set_desync(child.role);
    wl_subsurface_set_position(child.role, 30, 2);
    REQUIRE(connection.attach_subsurface(client, child, 8, 8, blue));
    xdg_toplevel_set_fullscreen(client.toplevel, nullptr);
    REQUIRE(connection.wait(client, [&] { return client.configured.fullscreen; }));
    REQUIRE(client.configured.width == 160 && client.configured.height == 90);
    REQUIRE(connection.attach(client, 60, 60, true, green));
    REQUIRE(desktop.windows().view(id).geometry == output_bounds);
    REQUIRE(wait_pixel(connection, client, output.get(), 34, 45, 0));
    REQUIRE(wait_pixel(connection, client, output.get(), 35, 45, green));
    REQUIRE(wait_pixel(connection, client, output.get(), 124, 45, green));
    REQUIRE(wait_pixel(connection, client, output.get(), 125, 45, 0));
    REQUIRE(wait_pixel(connection, client, output.get(), 83, 6, blue));

    seaside_scene_target target{};
    REQUIRE(!scene.window_at(34, 45, target));
    REQUIRE(scene.window_at(65, 45, target) && target.id == id &&
            target.surface == scene.window_surface(id) && target.sx == 20 && target.sy == 30);
    REQUIRE(scene.window_at(32, 45, target) && target.sx == -2 && target.sy == 30);
    auto *input = wl_compositor_create_region(client.compositor);
    wl_region_add(input, 10, 20, 25, 25);
    wl_surface_set_input_region(client.surface, input);
    wl_region_destroy(input);
    wl_surface_commit(client.surface);
    REQUIRE(connection.roundtrip(client));
    target = {};
    REQUIRE(!scene.window_at(40, 45, target));
    REQUIRE(scene.window_at(65, 45, target) && target.sx == 20 && target.sy == 30);
    wl_surface_set_input_region(client.surface, nullptr);
    wl_surface_commit(client.surface);
    REQUIRE(connection.roundtrip(client));
    target = {};
    REQUIRE(scene.window_at(83, 6, target) && target.id == id &&
            target.surface != scene.window_surface(id) && target.sx == 2 && target.sy == 2);

    Popup popup;
    REQUIRE(connection.open_popup(client, popup));
    REQUIRE(wait_pixel(connection, client, output.get(), 53, 18, 0));
    target = {};
    REQUIRE(scene.window_at(53, 18, target) && target.id == id && target.sx == 2 && target.sy == 2);
    REQUIRE(scene.window_at(47, 18, target) && target.sx == -2 && target.sy == 2);

    auto stretch = seaside::default_config();
    stretch.window.tiling = false;
    stretch.render.fullscreen_aspect = seaside::FullscreenAspect::stretch;
    desktop.apply_config(stretch);
    REQUIRE(wait_pixel(connection, client, output.get(), 1, 45, green));
    REQUIRE(wait_pixel(connection, client, output.get(), 158, 45, green));
    REQUIRE(wait_pixel(connection, client, output.get(), 30, 18, 0));
    REQUIRE(wait_pixel(connection, client, output.get(), 83, 11, blue));
    REQUIRE(connection.attach_subsurface(client, child, 12, 4, red));
    REQUIRE(wait_pixel(connection, client, output.get(), 105, 6, red));
    REQUIRE(wait_pixel(connection, client, output.get(), 105, 11, green));

    stretch.render.fullscreen_aspect = seaside::FullscreenAspect::preserve;
    desktop.apply_config(stretch);
    REQUIRE(connection.attach(client, 160, 90, false, red));
    REQUIRE(wait_pixel(connection, client, output.get(), 1, 45, red));
    REQUIRE(wait_pixel(connection, client, output.get(), 158, 45, red));
    target = {};
    REQUIRE(scene.window_at(100, 60, target) && target.sx == 100 && target.sy == 60);

    xdg_toplevel_unset_fullscreen(client.toplevel);
    REQUIRE(connection.wait(client, [&] { return !client.configured.fullscreen; }));
    REQUIRE(desktop.windows().view(id).geometry == ordinary);
    REQUIRE(connection.attach_configured(client, green));
    REQUIRE(hit(scene, ordinary, id));
    REQUIRE(!desktop.failed() && !scene.snapshot().failed);
    return true;
}

bool run_dwindle(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", "main", output_bounds));
    const auto &windows = desktop.windows();
    Connection connection(fixture.display);
    Client first, second;
    WindowId a{}, b{};
    constexpr WindowRect full{6, 6, 148, 78};
    REQUIRE(map_tiled(fixture, connection, first, a, full.width, full.height, 0, red));
    REQUIRE(windows.view(a).geometry == full);
    REQUIRE(map_tiled(fixture, connection, second, b, 69, 78, 0, green));
    REQUIRE(b != a);
    REQUIRE(windows.view(a).geometry == (WindowRect{6, 6, 69, 78}));
    REQUIRE(windows.view(b).geometry == (WindowRect{85, 6, 69, 78}));

    REQUIRE(wait_tiled(connection, first, 69, 78));
    // The previous acknowledged size remains legal until the new configure is
    // acknowledged. A late commit cannot resize the allocated tile.
    REQUIRE(connection.attach(first, full.width, full.height, false, red));
    REQUIRE(windows.view(a).geometry == (WindowRect{6, 6, 69, 78}));
    REQUIRE(wait_pixel(connection, first, output.get(), 74, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), 80, 45, 0));
    REQUIRE(wait_pixel(connection, first, output.get(), 100, 45, green));
    seaside_scene_target border{};
    REQUIRE(fixture.scene.window_at(75, 45, border) && border.id == a && !border.surface);
    REQUIRE(connection.attach_configured(first, red));
    constexpr std::uint32_t inactive_border = 0x595959;
    constexpr std::uint32_t active_border = 0xbfbfbf;
    REQUIRE(wait_pixel(connection, first, output.get(), 5, 45, inactive_border));
    REQUIRE(wait_pixel(connection, first, output.get(), 84, 45, active_border));

    xdg_toplevel_set_maximized(second.toplevel);
    REQUIRE(connection.wait(second, [&] { return second.configured.maximized; }));
    REQUIRE(connection.attach_configured(second, green));
    REQUIRE(wait_pixel(connection, first, output.get(), 20, 45, green));
    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(windows.focused_window() == a && windows.view(b).maximized);
    REQUIRE(wait_pixel(connection, first, output.get(), 20, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), 5, 45, active_border));
    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(windows.focused_window() == b && windows.view(b).maximized);
    REQUIRE(wait_pixel(connection, first, output.get(), 20, 45, green));
    xdg_toplevel_unset_maximized(second.toplevel);
    REQUIRE(wait_tiled(connection, second, 69, 78));
    REQUIRE(connection.attach_configured(second, green));

    REQUIRE(begin_drag(fixture, b, BTN_RIGHT, 119, 45));
    desktop.motion(131, 45);
    const auto resized_a = windows.view(a).geometry;
    const auto resized_b = windows.view(b).geometry;
    REQUIRE(resized_a.width != 69 && resized_a.width + resized_b.width == 138);
    REQUIRE(wait_tiled(connection, first, resized_a.width, 78));
    REQUIRE(wait_tiled(connection, second, resized_b.width, 78));
    REQUIRE(second.configured.resizing);
    // Both clients still have their earlier sized buffers. The new split
    // must already be fully covered up to its configured gap.
    const auto left_edge = static_cast<unsigned>(resized_a.x + resized_a.width - 1);
    const auto right_edge = static_cast<unsigned>(resized_b.x);
    REQUIRE(wait_pixel(connection, first, output.get(), left_edge, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), right_edge, 45, green));
    REQUIRE(connection.attach_configured(first) && connection.attach_configured(second));
    desktop.button(0, BTN_RIGHT, false, super, 131, 45);
    REQUIRE(connection.wait(second, [&] { return !second.configured.resizing; }));

    REQUIRE(begin_drag(fixture, b, BTN_LEFT, resized_b.x + 3, 45));
    REQUIRE(windows.view(b).tile_layout);
    desktop.button(0, BTN_LEFT, false, super, resized_b.x + 3, 45);
    REQUIRE(windows.view(a).geometry == resized_a && windows.view(b).geometry == resized_b);

    REQUIRE(begin_drag(fixture, b, BTN_LEFT, resized_b.x + 3, 45));
    desktop.motion(14, 45);
    REQUIRE(windows.view(a).geometry == full);
    desktop.button(0, BTN_LEFT, false, super, 14, 45);
    const auto reinserted_a = windows.view(a).geometry;
    const auto reinserted_b = windows.view(b).geometry;
    REQUIRE(reinserted_b.x < reinserted_a.x);
    REQUIRE(wait_tiled(connection, first, reinserted_a.width, 78));
    REQUIRE(wait_tiled(connection, second, reinserted_b.width, 78));
    REQUIRE(connection.attach_configured(first) && connection.attach_configured(second));

    REQUIRE(begin_drag(fixture, b, BTN_LEFT, reinserted_b.x + 3, 45));
    desktop.motion(200, 110);
    REQUIRE(connection.wait(second, [&] { return !second.configured.tiled_edges; }));
    desktop.button(0, BTN_LEFT, false, super, 200, 110);
    REQUIRE(windows.view(a).geometry == reinserted_a);
    REQUIRE(windows.view(b).geometry == reinserted_b);
    REQUIRE(wait_tiled(connection, second, reinserted_b.width, 78));
    REQUIRE(connection.attach_configured(second));

    REQUIRE(begin_drag(fixture, b, BTN_LEFT, reinserted_b.x + 3, 45));
    desktop.motion(reinserted_b.x + 6, 45);
    second.destroy_window();
    REQUIRE(connection.roundtrip(second));
    desktop.button(0, BTN_LEFT, false, super, 14, 45);
    REQUIRE(!windows.state().window(b) && windows.focused_window() == a);
    REQUIRE(windows.view(a).geometry == full);
    REQUIRE(wait_tiled(connection, first, full.width, full.height));
    REQUIRE(connection.attach_configured(first));

    REQUIRE(desktop.action(action(ActionKind::toggle_window_tiling)));
    REQUIRE(connection.wait(first, [&] { return !first.configured.tiled_edges; }));
    REQUIRE(first.configured.width == 0 && first.configured.height == 0);
    REQUIRE(connection.attach(first, full.width, full.height, false));
    REQUIRE(windows.view(a).geometry.width == 0 && windows.view(a).geometry.height == 0);
    REQUIRE(hit(fixture.scene, full, a));
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    REQUIRE(hit(fixture.scene, windows.view(a).geometry, a));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    return true;
}

bool run_scrolling(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto &scene = fixture.scene;
    auto primary = fixture.output();
    auto neighbor = fixture.output();
    REQUIRE(primary && neighbor);
    REQUIRE(desktop.add_output(part3_output_handle(primary.get()), "main", "main", output_bounds));
    constexpr WindowRect adjacent{160, 0, 160, 90};
    REQUIRE(
        desktop.add_output(part3_output_handle(neighbor.get()), "adjacent", "adjacent", adjacent));
    const auto &windows = desktop.windows();
    const auto workspace = windows.focused_workspace();
    REQUIRE(workspace);
    Connection connection(fixture.display);
    Client first, second, third;
    WindowId a{}, b{}, c{};
    REQUIRE(map_tiled(fixture, connection, first, a, 80, 90, 0, red));
    REQUIRE(windows.view(a).geometry == (WindowRect{0, 0, 80, 90}));
    REQUIRE(map_tiled(fixture, connection, second, b, 100, 90, 100, green));
    REQUIRE(windows.view(a).geometry.x == -20);
    REQUIRE(windows.view(b).geometry == (WindowRect{60, 0, 100, 90}));
    seaside_scene_target clipped{};
    REQUIRE(scene.window_at(5, 45, clipped) && clipped.id == a && clipped.sx == 25.0);
    seaside_scene_target grabbed{};
    grabbed.surface = clipped.surface;
    REQUIRE(scene.window_at(170, 45, grabbed) && grabbed.id == a && grabbed.sx == 190.0);
    REQUIRE(map_tiled(fixture, connection, third, c, 80, 90, 0, blue));
    REQUIRE(a != b && b != c && c != a);
    REQUIRE(windows.view(c).geometry == (WindowRect{80, 0, 80, 90}));

    REQUIRE(desktop.action(directional_action(ActionKind::focus, Direction::right)));
    REQUIRE(windows.focused_window() == c);
    REQUIRE(windows.view(c).geometry == (WindowRect{80, 0, 80, 90}));
    REQUIRE(desktop.action(directional_action(ActionKind::focus, Direction::left)));
    REQUIRE(windows.focused_window() == b);
    REQUIRE(desktop.action(directional_action(ActionKind::focus, Direction::left)));
    REQUIRE(windows.focused_window() == a);
    REQUIRE(desktop.action(directional_action(ActionKind::focus, Direction::left)));
    REQUIRE(windows.focused_window() == a);
    REQUIRE(windows.view(a).geometry == (WindowRect{0, 0, 80, 90}));
    REQUIRE(windows.view(b).geometry == (WindowRect{80, 0, 100, 90}));
    REQUIRE(windows.view(c).geometry == (WindowRect{180, 0, 80, 90}));
    seaside_scene_target inside{};
    seaside_scene_target across{};
    REQUIRE(scene.window_at(150, 45, inside) && inside.id == b && inside.sx == 70.0);
    REQUIRE(!scene.window_at(165, 45, across));
    REQUIRE(wait_pixel(connection, first, primary.get(), 150, 45, green));
    REQUIRE(wait_pixel(connection, first, neighbor.get(), 5, 45, 0));

    REQUIRE(desktop.action(directional_action(ActionKind::focus, Direction::right)));
    REQUIRE(windows.focused_window() == b);
    REQUIRE(windows.view(b).geometry.x == 60);
    REQUIRE(desktop.action(directional_action(ActionKind::resize_window, Direction::right, 20)));
    REQUIRE(wait_tiled(connection, second, 120, 90));
    REQUIRE(connection.attach_configured(second, green));
    const auto width_a = windows.view(a).geometry.width;
    const auto width_c = windows.view(c).geometry.width;
    const auto custom_column = windows.view(b).geometry;
    REQUIRE(width_a == 80 && width_c == 80 && custom_column.width == 120);

    REQUIRE(desktop.action(action(ActionKind::workspace, 42)));
    REQUIRE(!windows.view(a).visible && !windows.view(b).visible && !windows.view(c).visible);
    REQUIRE(desktop.action(action(ActionKind::workspace, *workspace)));
    REQUIRE(windows.focused_window() == b && windows.view(b).geometry == custom_column);
    second.destroy_window();
    REQUIRE(connection.roundtrip(second));
    REQUIRE(!windows.state().window(b) && windows.focused_window() == c);
    REQUIRE(windows.view(a).geometry.width == width_a && windows.view(c).geometry.width == width_c);
    REQUIRE(windows.view(a).geometry.x + width_a == windows.view(c).geometry.x);
    REQUIRE(!desktop.failed() && !scene.snapshot().failed);
    return true;
}

void token_created(void *data, xdg_activation_token_v1 *, const char *token) {
    *static_cast<std::string *>(data) = token;
}

bool run_attention(const Fixture &fixture) {
    auto &scene = fixture.scene;
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", "stable-headless",
                               output_bounds));
    const auto &windows = desktop.windows();
    const auto original_workspace = windows.focused_workspace();
    REQUIRE(original_workspace);

    Connection connection(fixture.display);
    Client first, second;
    REQUIRE(connection.connect(first) && first.activation && connection.open(first));
    xdg_toplevel_set_app_id(first.toplevel, "org.example.Attention");
    xdg_toplevel_set_title(first.toplevel, "Attention café");
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    const auto first_id = windows.focused_window();
    REQUIRE(first_id);
    const auto metadata = scene.window_metadata(*first_id);
    REQUIRE(metadata.app_id && std::string_view(metadata.app_id) == "org.example.Attention");
    REQUIRE(metadata.title && std::string_view(metadata.title) == "Attention café");
    REQUIRE(connection.connect(second) && connection.open(second));
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    const auto second_id = windows.focused_window();
    REQUIRE(second_id && second_id != first_id);
    REQUIRE(desktop.action(action(ActionKind::move_to_workspace, 42)));
    REQUIRE(!windows.view(*first_id).visible);

    struct Attention {
        WindowId target = 0;
        seaside::WorkspaceId workspace = 0;
        unsigned count = 0;
    };
    const auto attention = std::make_shared<Attention>();
    desktop.on_attention([attention](WindowId target, seaside::WorkspaceId workspace) {
        *attention = {target, workspace, attention->count + 1};
    });
    const xdg_activation_token_v1_listener listener{.done = token_created};
    std::string token;
    auto *request = xdg_activation_v1_get_activation_token(first.activation);
    xdg_activation_token_v1_add_listener(request, &listener, &token);
    xdg_activation_token_v1_set_surface(request, first.surface);
    xdg_activation_token_v1_commit(request);
    const bool issued = connection.wait(first, [&] { return !token.empty(); });
    xdg_activation_token_v1_destroy(request);
    REQUIRE(issued);
    xdg_activation_v1_activate(first.activation, token.c_str(), first.surface);
    REQUIRE(connection.roundtrip(first));
    REQUIRE(attention->count == 1 && attention->target == *first_id &&
            attention->workspace == *original_workspace);
    REQUIRE(windows.focused_window() == second_id && windows.focused_workspace() == 42);
    xdg_activation_v1_activate(first.activation, token.c_str(), first.surface);
    REQUIRE(connection.roundtrip(first) && attention->count == 1);
    desktop.on_attention({});

    xdg_toplevel_set_fullscreen(second.toplevel, nullptr);
    REQUIRE(connection.wait(second, [&] { return second.configured.fullscreen; }));
    REQUIRE(connection.attach_configured(second, blue));
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, blue));
    const seaside::Diagnostic error{.path = "/tmp/café.toml",
                                    .line = 4,
                                    .column = 1,
                                    .field = "window.layout",
                                    .message = "Invalid value"};
    scene.set_config_error(error);
    const auto banner = seaside::render_config_banner("color sample", output_bounds.width, 1);
    REQUIRE(banner);
    const auto &pixel = banner->pixels;
    const auto color = static_cast<std::uint32_t>(pixel[0] | pixel[1] << 8 | pixel[2] << 16);
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, color));
    seaside_scene_target target{};
    REQUIRE(scene.window_at(1, 1, target) && target.id == *second_id);
    scene.set_config_error(std::nullopt);
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, blue));
    return true;
}

bool run_binding_reload(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "reload", {}, output_bounds));
    Connection connection(fixture.display);
    Client first, second;
    WindowId a{}, b{};
    REQUIRE(map_tiled(fixture, connection, first, a, 148, 78, 0, red));
    REQUIRE(map_tiled(fixture, connection, second, b, 69, 78, 0, blue));
    REQUIRE(wait_tiled(connection, first, 69, 78));
    REQUIRE(connection.attach_configured(first));
    const auto left = windows.view(a).geometry;
    const auto right = windows.view(b).geometry;

    const seaside::test::Directory directory;
    const seaside::ConfigPaths paths{directory.path() / "config.toml",
                                     directory.path() / "state/last-good.toml"};
    const seaside::ConfigStore store;
    const std::string initial = "[window]\ngaps_outer=4\ngaps_inner=6\n";
    REQUIRE(store.publish(paths.user, initial, seaside::PublishMode::create_user_file));
    seaside::LiveConfig *controller = nullptr;
    auto reload = seaside::LiveConfig::create(
        wl_display_get_event_loop(fixture.display), store, paths,
        seaside::load_startup_config(store, paths), {}, {},
        [&](seaside::LiveConfig::Change change, const auto &) {
            if (change == seaside::LiveConfig::Change::committed) {
                // The production session projects the committed binding map here.
                desktop.apply_config(controller->transaction().active());
            }
        });
    REQUIRE(reload);
    controller = reload->get();

    REQUIRE(begin_drag(fixture, b, BTN_LEFT, right.x + 6, 45));
    desktop.motion(20, 45);
    REQUIRE(!windows.view(b).tile_layout && windows.view(a).geometry.width == 148);
    const auto replacement = directory.path() / "editor.tmp";
    REQUIRE(static_cast<bool>(std::ofstream(replacement) << initial << "[keybinds]\n"));
    std::filesystem::rename(replacement, paths.user);
    REQUIRE(connection.wait(second,
                            [&] { return (*reload)->transaction().active().keybinds.empty(); }));
    REQUIRE(windows.view(a).geometry == left && windows.view(b).geometry == right);
    REQUIRE(wait_tiled(connection, second, right.width, right.height));

    desktop.button(0, BTN_LEFT, false, super, 20, 45);
    desktop.motion(27, 45);
    REQUIRE(windows.view(a).geometry == left && windows.view(b).geometry == right);
    REQUIRE(!desktop.button(b, BTN_LEFT, true, super, right.x + 6, 45));
    REQUIRE(!desktop.failed());
    return true;
}

} // namespace

int main(int argc, char **argv) {
    auto config = seaside::default_config();
    if (argc == 1) {
        config.window.tiling = false;
        return run_fixture(config, run_part4) && run_fixture(config, run_workspace_reassignment) &&
                       run_fixture(config, run_floating_borders) &&
                       run_fixture(config, run_client_gestures)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    const std::string_view scenario = argv[1];
    if (argc == 2 && scenario == "layer-shell") {
        config.window.gaps_outer = 0;
        config.window.gaps_inner = 0;
        config.window.border_width = 0;
        return run_fixture(config, run_layer_shell) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "fullscreen-aspect") {
        config.window.tiling = false;
        return run_fixture(config, run_fullscreen_aspect) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "attention") {
        config.window.tiling = false;
        return run_fixture(config, run_attention) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "binding-reload") {
        config.window.gaps_outer = 4;
        config.window.gaps_inner = 6;
        return run_fixture(config, run_binding_reload) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "dwindle") {
        auto transaction = config;
        transaction.window.gaps_outer = 0;
        transaction.window.gaps_inner = 0;
        transaction.window.border_width = 0;
        config.window.gaps_outer = 4;
        config.window.gaps_inner = 6;
        return run_fixture(config, run_dwindle) &&
                       run_fixture(transaction, run_opening_transaction) &&
                       run_fixture(transaction, run_pending_opening_focus)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "scrolling") {
        config.window.layout = seaside::Layout::scrolling;
        config.window.gaps_outer = 0;
        config.window.gaps_inner = 0;
        config.window.border_width = 0;
        return run_fixture(config, run_scrolling) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    std::fprintf(stderr, "Unknown desktop fixture scenario: %s\n", argv[1]);
    return EXIT_FAILURE;
}

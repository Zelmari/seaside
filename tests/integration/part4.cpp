#include "desktop.hpp"
#include "live_config.hpp"
#include "part3_bridge.hpp"
#include "pointer-constraints-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "server.hpp"
#include "test_directory.hpp"
#include "tty_input.hpp"
#include "wlr_layer_shell_client_compat.hpp"
#include "wlroots_bridge.hpp"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/input-event-codes.h>
#include <memory>
#include <poll.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

extern "C" wl_event_source *__real_wl_event_loop_add_timer(wl_event_loop *loop,
                                                           wl_event_loop_timer_func_t callback,
                                                           void *data);
extern "C" int __real_wl_event_source_timer_update(wl_event_source *source, int delay);

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

// Hold the Desktop deadline while the test prepares and examines pending windows.
// Timeout checks can then arm the same Wayland timer with its original delay.
struct OpeningDeadline {
    explicit OpeningDeadline(const seaside::Desktop &owner) noexcept : desktop(&owner) {
        active = this;
    }
    ~OpeningDeadline() { active = nullptr; }
    OpeningDeadline(const OpeningDeadline &) = delete;
    OpeningDeadline &operator=(const OpeningDeadline &) = delete;

    bool start() const noexcept {
        return timer && delay > 0 && __real_wl_event_source_timer_update(timer, delay) == 0;
    }

    static inline OpeningDeadline *active = nullptr;
    const seaside::Desktop *desktop;
    wl_event_source *timer = nullptr;
    int delay = 0;
    unsigned arms = 0;
};

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
        if (constraints) {
            zwp_pointer_constraints_v1_destroy(constraints);
        }
        if (relative_manager) {
            zwp_relative_pointer_manager_v1_destroy(relative_manager);
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
        if (decorations) {
            zxdg_decoration_manager_v1_destroy(decorations);
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
        if (display && owns_display) {
            wl_display_disconnect(display);
        }
    }

    void destroy_window() {
        if (decoration) {
            zxdg_toplevel_decoration_v1_destroy(decoration);
            decoration = nullptr;
        }
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
    bool owns_display = true;
    wl_registry *registry = nullptr;
    wl_compositor *compositor = nullptr;
    wl_subcompositor *subcompositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    xdg_activation_v1 *activation = nullptr;
    zxdg_decoration_manager_v1 *decorations = nullptr;
    zxdg_toplevel_decoration_v1 *decoration = nullptr;
    std::uint32_t decoration_mode = 0;
    zwlr_layer_shell_v1 *layers = nullptr;
    wl_data_device_manager *data_devices = nullptr;
    wl_seat *seat = nullptr;
    wl_keyboard *keyboard = nullptr;
    wl_pointer *pointer = nullptr;
    zwp_pointer_constraints_v1 *constraints = nullptr;
    zwp_relative_pointer_manager_v1 *relative_manager = nullptr;
    std::array<wl_output *, 2> outputs{};
    wl_surface *keyboard_surface = nullptr;
    wl_surface *pointer_surface = nullptr;
    unsigned keys = 0;
    unsigned pointer_motions = 0;
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
void pointer_motion(void *data, wl_pointer *, std::uint32_t, wl_fixed_t, wl_fixed_t) {
    ++static_cast<Client *>(data)->pointer_motions;
}
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
    } else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        client.decorations = static_cast<zxdg_decoration_manager_v1 *>(
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
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
    } else if (std::strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        client.constraints = static_cast<zwp_pointer_constraints_v1 *>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        client.relative_manager = static_cast<zwp_relative_pointer_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
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
        return bind_globals(client);
    }

    bool connect_sibling(Client &client, const Client &owner) {
        client.display = owner.display;
        client.owns_display = false;
        return bind_globals(client);
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

    bool open(Client &client, int minimum_width = 0, bool negotiate_decoration = false) {
        client.surface = wl_compositor_create_surface(client.compositor);
        client.xdg = xdg_wm_base_get_xdg_surface(client.shell, client.surface);
        xdg_surface_add_listener(client.xdg, &surface_listener, &client);
        client.toplevel = xdg_surface_get_toplevel(client.xdg);
        xdg_toplevel_add_listener(client.toplevel, &toplevel_listener, &client);
        if (negotiate_decoration) {
            if (!client.decorations) {
                return false;
            }
            client.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
                client.decorations, client.toplevel);
            static const zxdg_toplevel_decoration_v1_listener listener{
                .configure =
                    [](void *data, zxdg_toplevel_decoration_v1 *, std::uint32_t mode) {
                        static_cast<Client *>(data)->decoration_mode = mode;
                    },
            };
            zxdg_toplevel_decoration_v1_add_listener(client.decoration, &listener, &client);
            zxdg_toplevel_decoration_v1_set_mode(client.decoration,
                                                 ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
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

    bool open_popup(Client &client, Popup &popup, Layer *layer = nullptr, std::uint32_t grab = 0,
                    int anchor_x = 0, Popup *parent = nullptr) {
        popup.surface = wl_compositor_create_surface(client.compositor);
        popup.xdg = xdg_wm_base_get_xdg_surface(client.shell, popup.surface);
        xdg_surface_add_listener(popup.xdg, &popup_surface_listener, &popup);
        auto *positioner = xdg_wm_base_create_positioner(client.shell);
        xdg_positioner_set_size(positioner, 12, 8);
        xdg_positioner_set_anchor_rect(positioner, anchor_x, 0, 10, 10);
        xdg_positioner_set_anchor(positioner, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
        xdg_positioner_set_gravity(positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
        popup.role = xdg_surface_get_popup(popup.xdg,
                                           layer    ? nullptr
                                           : parent ? parent->xdg
                                                    : client.xdg,
                                           positioner);
        xdg_positioner_destroy(positioner);
        xdg_popup_add_listener(popup.role, &popup_listener, &popup);
        if (layer) {
            zwlr_layer_surface_v1_get_popup(layer->role, popup.role);
        }
        if (grab) {
            xdg_popup_grab(popup.role, client.seat, grab);
        }
        wl_surface_commit(popup.surface);
        return wait(client, [&] { return popup.serial || popup.dismissed; }) && !popup.dismissed &&
               attach(client, popup.surface, popup.xdg, popup.serial, popup.geometry.width,
                      popup.geometry.height);
    }

  private:
    bool bind_globals(Client &client) {
        client.registry = wl_display_get_registry(client.display);
        wl_registry_add_listener(client.registry, &registry_listener, &client);
        return roundtrip(client) && client.compositor && client.shm && client.shell;
    }

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

template <typename Scenario> bool run_fixture(const seaside::Config &config, Scenario scenario) {
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

bool run_titlebars(const Fixture &fixture) {
    constexpr auto client_side = ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    constexpr auto server_side = ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    Connection connection(fixture.display);
    Client first;
    REQUIRE(connection.connect(first) && connection.open(first, 0, true));
    REQUIRE(first.decoration_mode == client_side);
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));

    auto config = seaside::default_config();
    config.window.titlebars = false;
    fixture.desktop.apply_config(config);
    REQUIRE(connection.wait(first, [&] { return first.decoration_mode == server_side; }));

    // Reverse the policy while the client still owes an acknowledgement.
    config.window.titlebars = true;
    fixture.desktop.apply_config(config);
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    REQUIRE(connection.wait(first, [&] { return first.decoration_mode == client_side; }));
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));

    config.window.titlebars = false;
    fixture.desktop.apply_config(config);
    Client second;
    REQUIRE(connection.connect(second) && connection.open(second, 0, true));
    REQUIRE(second.decoration_mode == server_side);
    zxdg_toplevel_decoration_v1_set_mode(second.decoration, client_side);
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    REQUIRE(second.decoration_mode == server_side);
    REQUIRE(connection.wait(first, [&] { return first.decoration_mode == server_side; }));
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));

    config.window.titlebars = true;
    fixture.desktop.apply_config(config);
    REQUIRE(connection.wait(first, [&] { return first.decoration_mode == client_side; }));
    REQUIRE(connection.wait(second, [&] { return second.decoration_mode == client_side; }));
    return !fixture.desktop.failed();
}

bool run_sparse_view_ids(const Fixture &fixture) {
    // Exercise two concurrent live IDs with identical low bits and a large gap.
    constexpr WindowId low = 17, high = (WindowId{1} << 48) + low;
    WindowId next = low;
    fixture.scene.set_window_handler(
        [](void *data, const seaside_scene_window_event *event) -> WindowId {
            return event->kind == SEASIDE_WINDOW_CREATED ? *static_cast<WindowId *>(data) : 0;
        },
        &next);
    Connection connection(fixture.display);
    Client first, second;
    REQUIRE(connection.connect(first) && connection.open(first));
    next = high;
    REQUIRE(connection.connect(second) && connection.open(second));
    xdg_toplevel_set_app_id(first.toplevel, "low");
    xdg_toplevel_set_app_id(second.toplevel, "high");
    REQUIRE(connection.attach(first, 20, 10) && connection.attach(second, 30, 15));
    const auto app_id = [&](WindowId id) {
        const char *value = fixture.scene.window_metadata(id).app_id;
        return std::string_view(value ? value : "");
    };
    REQUIRE(app_id(low) == "low" && app_id(high) == "high");
    first.destroy_window();
    REQUIRE(connection.roundtrip(first));
    REQUIRE(!fixture.scene.window_metadata(low).app_id);
    auto *surface = fixture.scene.window_surface(high);
    REQUIRE(surface && !fixture.scene.window_surface(low));
    wl_surface_attach(second.surface, nullptr, 0, 0);
    wl_surface_commit(second.surface);
    REQUIRE(connection.roundtrip(second) && !fixture.scene.window_surface(high));
    xdg_toplevel_set_app_id(second.toplevel, "remapped");
    const auto before = second.configures;
    wl_surface_commit(second.surface);
    REQUIRE(connection.wait(second, [&] { return second.configures > before; }));
    REQUIRE(connection.attach(second, 24, 12) && fixture.scene.window_surface(high) == surface);
    REQUIRE(app_id(high) == "remapped");
    second.destroy_window();
    REQUIRE(connection.roundtrip(second) && !fixture.scene.window_metadata(high).app_id);
    fixture.scene.set_window_handler(nullptr, nullptr);
    return true;
}

bool run_part4(const Fixture &fixture) {
    auto &scene = fixture.scene;
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", output_bounds));
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
    const WindowRect maximized{12, 12, 136, 66};
    REQUIRE(second.configured.width == maximized.width &&
            second.configured.height == maximized.height);
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    REQUIRE(windows.view(*b).geometry == maximized);
    xdg_toplevel_set_fullscreen(second.toplevel, nullptr);
    REQUIRE(connection.wait(second, [&] { return second.configured.fullscreen; }));
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    xdg_toplevel_unset_fullscreen(second.toplevel);
    REQUIRE(connection.wait(
        second, [&] { return !second.configured.fullscreen && second.configured.maximized; }));
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    xdg_toplevel_unset_maximized(second.toplevel);
    // Submit one last old-size buffer before the new configure is acked.
    REQUIRE(connection.attach(second, maximized.width, maximized.height, false));
    REQUIRE(connection.wait(second, [&] { return !second.configured.maximized; }));
    REQUIRE(second.configured.width == ordinary.width &&
            second.configured.height == ordinary.height);
    REQUIRE(connection.attach(second, second.configured.width, second.configured.height));
    REQUIRE(windows.view(*b).geometry == ordinary);

    REQUIRE(desktop.action(action(ActionKind::move_to_workspace, 4)));
    REQUIRE(windows.focused_window() == b && windows.focused_workspace() == 4);
    REQUIRE(windows.state().window(*b)->home_workspace == 4 && !windows.view(*a).visible);
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
        REQUIRE(desktop.action(action(ActionKind::workspace, 4)));
        REQUIRE(connection.wait(first, [&] { return popup.dismissed; }));
        REQUIRE(!windows.view(*a).visible);
        REQUIRE(desktop.action(action(ActionKind::workspace, *original_workspace)));
    }

    REQUIRE(desktop.action(action(ActionKind::toggle_window_pin)));
    REQUIRE(desktop.action(action(ActionKind::workspace, 4)));
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
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", output_bounds));
    REQUIRE(windows.focused_workspace() == 4 && windows.focused_window() == b);
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

bool run_workspace_topology(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    const WindowRect right_bounds{output_bounds.width, 0, output_bounds.width,
                                  output_bounds.height};
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", right_bounds));
    REQUIRE(windows.state().workspaces().size() == seaside::workspace_count);
    REQUIRE(windows.state().output("left")->shown == 1);
    REQUIRE(windows.state().output("right")->shown == 6);
    REQUIRE(desktop.dispatch(action(ActionKind::workspace, 8)).value_or(false));
    REQUIRE(windows.focused_output() == "right");
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && connection.open(client));
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height));
    const auto window = desktop.keyboard_window();
    REQUIRE(window);
    REQUIRE(windows.view(*window).geometry.x >= right_bounds.x);

    desktop.remove_output(part3_output_handle(right.get()));
    right.reset();
    REQUIRE(windows.workspace_output(8) == "left");
    REQUIRE(windows.workspace_owner(8) == "left");
    REQUIRE(windows.focused_workspace() == 8);
    REQUIRE(windows.focused_output() == "left");
    REQUIRE(desktop.keyboard_window() == window);

    right = fixture.output();
    REQUIRE(right);
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", right_bounds));
    REQUIRE(windows.workspace_output(8) == "right");
    REQUIRE(windows.state().output("left")->shown == 1);
    REQUIRE(windows.focused_workspace() == 8);
    REQUIRE(windows.focused_output() == "right");
    REQUIRE(windows.view(*window).geometry.x >= right_bounds.x);
    desktop.set_primary_output("right");
    REQUIRE(windows.state().output("right")->shown == 1);
    REQUIRE(windows.state().output("left")->shown == 8);
    REQUIRE(windows.focused_output() == "left");
    REQUIRE(windows.focused_workspace() == 8);
    REQUIRE(desktop.keyboard_window() == window);
    REQUIRE(windows.view(*window).geometry.x < right_bounds.x);
    REQUIRE(hit(fixture.scene, windows.view(*window).geometry, *window));
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

bool run_edge_drag(const Fixture &fixture) {
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    constexpr WindowRect right_bounds{160, 0, 160, 90};
    auto &desktop = fixture.desktop;
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", right_bounds));
    const auto &windows = desktop.windows();
    Connection connection(fixture.display);
    Client client;
    WindowId tiled{};
    REQUIRE(map_tiled(fixture, connection, client, tiled, 160, 90, 0, red));

    REQUIRE(begin_drag(fixture, tiled, BTN_LEFT, 100, 45));
    desktop.motion(185, 45);
    REQUIRE(windows.state().window(tiled)->home_workspace == 1);
    REQUIRE(windows.view(tiled).unclipped_move);
    REQUIRE(wait_pixel(connection, client, right.get(), 30, 45, red));
    seaside_scene_target preview{};
    REQUIRE(fixture.scene.window_at(190, 45, preview) && preview.id == tiled);

    desktop.button(0, BTN_LEFT, false, super, 185, 45);
    REQUIRE(windows.state().window(tiled)->home_workspace == 6);
    REQUIRE(windows.focused_window() == tiled && windows.focused_output() == "right");
    REQUIRE(windows.view(tiled).tile_layout && windows.view(tiled).geometry == right_bounds);
    REQUIRE(wait_tiled(connection, client, 160, 90));
    REQUIRE(connection.attach_configured(client, red));

    REQUIRE(begin_drag(fixture, tiled, BTN_LEFT, 220, 45));
    desktop.motion(145, 45);
    desktop.cancel_drag();
    desktop.button(0, BTN_LEFT, false, super, 145, 45);
    REQUIRE(windows.state().window(tiled)->home_workspace == 6);
    REQUIRE(windows.view(tiled).tile_layout && windows.view(tiled).geometry == right_bounds);
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(part3_output_handle(right.get()));
    desktop.remove_output(part3_output_handle(left.get()));
    return true;
}

bool pump_frames(Connection &connection, Client &client) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(35);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!connection.pump(client)) {
            return false;
        }
    }
    return true;
}

bool check_idle_resize(const Fixture &fixture, Connection &connection, Client &client,
                       std::initializer_list<std::pair<double, double>> positions) {
    REQUIRE(pump_frames(connection, client));
    unsigned commits = 0;
    fixture.scene.set_frame_observer(
        [](void *data, wlr_output *, const seaside_scene_frame_observation *frame) {
            if (std::strcmp(frame->event, "commit") == 0 && frame->succeeded) {
                ++*static_cast<unsigned *>(data);
            }
        },
        &commits);
    bool pumped = true;
    for (const auto &[x, y] : positions) {
        fixture.desktop.motion(x, y);
        pumped = pump_frames(connection, client) && pumped;
    }
    fixture.scene.set_frame_observer(nullptr, nullptr);
    REQUIRE(pumped && commits == 0);
    return true;
}

bool run_opening_transaction(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
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
    const auto workspace = desktop.windows().focused_workspace();
    REQUIRE(workspace);
    const auto unchanged = desktop.dispatch(action(ActionKind::workspace, *workspace));
    REQUIRE(unchanged && !*unchanged);
    seaside_scene_target pending{};
    REQUIRE(fixture.scene.window_at(110, 45, pending) && pending.id == left);
    REQUIRE(wait_pixel(connection, first, output.get(), 110, 55, red));
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
    REQUIRE(check_idle_resize(fixture, connection, first,
                              {{110, 45}, {110.2, 45.1}, {109.5, 44.2}, {110.6, 45}, {110.6, 45}}));
    desktop.motion(115, 45);
    desktop.motion(109.8, 45);
    REQUIRE(pump_frames(connection, first));
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
    REQUIRE(begin_drag(fixture, *right, BTN_RIGHT, 115, 45));
    desktop.motion(135, 45);
    desktop.cancel_drag();
    REQUIRE(pump_frames(connection, first));
    REQUIRE(desktop.windows().view(left).geometry.width == 105);

    Client third;
    REQUIRE(connection.connect(third) && connection.open(third));
    REQUIRE(third.configured.tiled_edges == all_tiled_edges);
    REQUIRE(connection.attach_configured(third, blue));
    const auto opened = desktop.windows().focused_window();
    REQUIRE(opened && desktop.dispatch(action(ActionKind::toggle_window_tiling), 0, opened));
    REQUIRE(connection.wait(third, [&] {
        return !third.configured.tiled_edges && !third.configured.width && !third.configured.height;
    }));
    REQUIRE(connection.attach(third, ordinary_width, ordinary_height, true, blue));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    return true;
}

bool run_workspace_during_opening(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto output = fixture.output();
    REQUIRE(output && desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
    Connection connection(fixture.display);
    Client first, second, third;
    WindowId left{};
    REQUIRE(map_tiled(fixture, connection, first, left, 160, 90, 0, red));
    const auto workspace = desktop.windows().focused_workspace();
    REQUIRE(workspace);
    REQUIRE(connection.connect(second) && connection.open(second) &&
            connection.attach_configured(second, green));
    const auto right = desktop.windows().focused_window();
    REQUIRE(right && right != left && desktop.keyboard_window() == left);
    // Reselect throughout the stall. The original opening timer must still fire
    // while the neighbor withholds its resized buffer.
    REQUIRE(deadline.arms == 1 && deadline.start());
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (desktop.keyboard_window() == left && std::chrono::steady_clock::now() < limit) {
        const auto unchanged = desktop.dispatch(action(ActionKind::workspace, *workspace));
        REQUIRE(unchanged && !*unchanged && desktop.keyboard_window() == left);
        REQUIRE(connection.pump(first));
    }
    REQUIRE(deadline.arms == 1 && desktop.keyboard_window() == right);
    REQUIRE(wait_pixel(connection, second, output.get(), 110, 55, green));
    REQUIRE(connection.connect(third) && connection.open(third) &&
            connection.attach_configured(third, blue));
    REQUIRE(desktop.keyboard_window() == right);
    const auto switched = desktop.dispatch(action(ActionKind::workspace, 4));
    REQUIRE(switched && *switched && !desktop.keyboard_window());
    seaside_scene_target empty{};
    REQUIRE(!fixture.scene.window_at(110, 55, empty));
    REQUIRE(wait_pixel(connection, third, output.get(), 110, 55, 0));
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

bool run_floating_delayed_commits(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output && desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && connection.open(client));
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height, true, red));
    const auto id = desktop.keyboard_window().value();
    const auto rect = desktop.windows().view(id).geometry;
    const double x = rect.x + rect.width - 4, y = rect.y + rect.height - 4;
    REQUIRE(begin_drag(fixture, id, BTN_RIGHT, x, y));
    desktop.motion(x + 6, y);
    REQUIRE(connection.wait(client, [&] { return client.configured.width == rect.width + 6; }));
    desktop.motion(x + 12, y);
    REQUIRE(desktop.windows().view(id).geometry.width == rect.width + 12);
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(desktop.windows().view(id).geometry.width == rect.width + 12);
    REQUIRE(connection.wait(client, [&] { return client.configured.width == rect.width + 12; }));
    desktop.motion(x + 18, y);
    desktop.button(0, BTN_RIGHT, false, super, x + 18, y);
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(desktop.windows().view(id).geometry.width == rect.width + 18);
    REQUIRE(connection.wait(client, [&] {
        return !client.configured.resizing && client.configured.width == rect.width + 18;
    }));
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(desktop.windows().view(id).geometry.width == rect.width + 18);
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height, false, green));
    REQUIRE(desktop.windows().view(id).geometry.width == ordinary_width);
    return true;
}

bool run_floating_resize_edges(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output && desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && connection.open(client));
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height, true, red));
    const auto id = desktop.keyboard_window().value_or(0);
    REQUIRE(id);
    const auto rect = desktop.windows().view(id).geometry;
    for (int horizontal : {-1, 0, 1}) {
        for (int vertical : {-1, 0, 1}) {
            if (!horizontal && !vertical) {
                continue;
            }
            const double x = rect.x + (horizontal < 0   ? 2
                                       : horizontal > 0 ? rect.width - 2
                                                        : rect.width / 2);
            const double y = rect.y + (vertical < 0   ? 2
                                       : vertical > 0 ? rect.height - 2
                                                      : rect.height / 2);
            REQUIRE(begin_drag(fixture, id, BTN_RIGHT, x, y));
            desktop.motion(x + (horizontal ? horizontal * 5 : 7),
                           y + (vertical ? vertical * 3 : 9));
            const WindowRect expected{
                rect.x - (horizontal < 0 ? 5 : 0), rect.y - (vertical < 0 ? 3 : 0),
                rect.width + (horizontal ? 5 : 0), rect.height + (vertical ? 3 : 0)};
            REQUIRE(desktop.windows().view(id).geometry == expected);
            desktop.motion(x, y);
            REQUIRE(desktop.windows().view(id).geometry == rect);
            desktop.button(0, BTN_RIGHT, false, super, x, y);
        }
    }
    const double x = rect.x + rect.width / 2, y = rect.y + rect.height / 2;
    REQUIRE(begin_drag(fixture, id, BTN_RIGHT, x, y));
    desktop.motion(x + 8, y + 1);
    REQUIRE(desktop.windows().view(id).geometry ==
            (WindowRect{rect.x, rect.y, rect.width + 8, rect.height}));
    desktop.motion(x - 4, y - 6);
    REQUIRE(desktop.windows().view(id).geometry ==
            (WindowRect{rect.x, rect.y, rect.width - 4, rect.height}));
    desktop.button(0, BTN_RIGHT, false, super, x - 4, y - 6);
    REQUIRE(!desktop.failed());
    return true;
}

bool run_floating_resize_limits(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output && desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && connection.open(client));
    xdg_toplevel_set_min_size(client.toplevel, 40, 26);
    xdg_toplevel_set_max_size(client.toplevel, ordinary_width, ordinary_height);
    REQUIRE(connection.attach(client, ordinary_width, ordinary_height, true, red));
    const auto id = desktop.keyboard_window();
    REQUIRE(id);
    const auto rect = desktop.windows().view(*id).geometry;
    const double x = rect.x + rect.width - 4, y = rect.y + rect.height - 4;
    REQUIRE(begin_drag(fixture, *id, BTN_RIGHT, x, y));
    const auto projections = std::make_shared<unsigned>(0);
    desktop.on_projection([projections] { ++*projections; });
    for (int step = 1; step <= 5; ++step) {
        desktop.motion(x + step, y + step);
    }
    REQUIRE(*projections == 0 && desktop.windows().view(*id).geometry == rect);
    desktop.motion(x - 15, y - 15);
    REQUIRE(connection.wait(client, [&] {
        return client.configured.resizing && client.configured.width == 40 &&
               client.configured.height == 26;
    }));
    REQUIRE(connection.attach_configured(client, red));
    *projections = 0;
    desktop.motion(x - 20, y - 20);
    REQUIRE(*projections == 0);
    const auto shrink = directional_action(ActionKind::resize_window, Direction::left, 1);
    const auto finish_drag = desktop.dispatch(shrink);
    REQUIRE(finish_drag && !*finish_drag);
    REQUIRE(connection.wait(client, [&] { return !client.configured.resizing; }));
    *projections = 0;
    const auto minimum = desktop.dispatch(shrink);
    REQUIRE(minimum && !*minimum && *projections == 0);
    desktop.button(0, BTN_RIGHT, false, super, x - 20, y - 20);

    xdg_toplevel_set_max_size(client.toplevel, 0, 0);
    wl_surface_commit(client.surface);
    REQUIRE(connection.roundtrip(client));
    const auto grow = directional_action(ActionKind::resize_window, Direction::right, 200);
    REQUIRE(desktop.dispatch(grow).value_or(false));
    REQUIRE(desktop.dispatch(directional_action(ActionKind::resize_window, Direction::down, 200))
                .value_or(false));
    REQUIRE(desktop.windows().view(*id).geometry == output_bounds);
    REQUIRE(connection.wait(
        client, [&] { return client.configured.width == 160 && client.configured.height == 90; }));
    REQUIRE(connection.attach_configured(client, red));
    *projections = 0;
    const auto maximum = desktop.dispatch(grow, 0, id);
    REQUIRE(maximum && !*maximum && *projections == 0);
    REQUIRE(begin_drag(fixture, *id, BTN_RIGHT, 140, 70));
    *projections = 0;
    desktop.motion(159, 89);
    REQUIRE(*projections == 0 && desktop.windows().view(*id).geometry == output_bounds);
    desktop.button(0, BTN_RIGHT, false, super, 159, 89);
    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_maximize)).value_or(false));
    const auto invalid = desktop.dispatch(grow);
    REQUIRE(!invalid && invalid.error() == seaside::DesktopActionError::invalid_state);
    desktop.on_projection({});
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

bool run_floating_resize_dialogs(const Fixture &fixture) {
    auto output = fixture.output();
    auto &desktop = fixture.desktop;
    REQUIRE(output &&
            desktop.add_output(part3_output_handle(output.get()), "headless", output_bounds));
    Connection connection(fixture.display);
    Client parent, child, nested;
    REQUIRE(connection.connect(parent) && connection.open(parent));
    REQUIRE(connection.attach(parent, 40, 32, true, red));
    const auto parent_id = desktop.windows().focused_window();
    REQUIRE(parent_id);
    // Fix the leading edge so the center of the resized parent moves.
    REQUIRE(
        desktop.dispatch(directional_action(ActionKind::move_window, Direction::left), 0, parent_id)
            .value_or(false));
    REQUIRE(connection.connect_sibling(child, parent) && connection.open(child));
    xdg_toplevel_set_parent(child.toplevel, parent.toplevel);
    REQUIRE(connection.attach(child, 22, 18, true, green));
    const auto child_id = desktop.windows().focused_window();
    REQUIRE(child_id && child_id != parent_id);
    REQUIRE(connection.connect_sibling(nested, parent) && connection.open(nested));
    xdg_toplevel_set_parent(nested.toplevel, child.toplevel);
    REQUIRE(connection.attach(nested, 8, 6, true, blue));
    const auto nested_id = desktop.windows().focused_window();
    REQUIRE(nested_id && nested_id != child_id);
    const auto before = desktop.windows().view(*nested_id).geometry;
    REQUIRE(wait_pixel(connection, nested, output.get(), before.x + 2, before.y + 2, blue));

    REQUIRE(desktop
                .dispatch(directional_action(ActionKind::resize_window, Direction::right, 24), 0,
                          parent_id)
                .value_or(false));
    const auto after = desktop.windows().view(*nested_id).geometry;
    const auto middle = desktop.windows().view(*child_id).geometry;
    REQUIRE(after.x == before.x + 12 && hit(fixture.scene, after, *nested_id));
    REQUIRE(desktop.keyboard_window() == nested_id);
    REQUIRE(wait_pixel(connection, nested, output.get(), middle.x + 1, middle.y + 1, green));
    REQUIRE(wait_pixel(connection, nested, output.get(), after.x + 2, after.y + 2, blue));
    REQUIRE(connection.wait(parent, [&] { return parent.configured.width == 64; }));
    REQUIRE(connection.attach_configured(parent, red));
    REQUIRE(hit(fixture.scene, after, *nested_id));
    return true;
}

bool run_floating_borders(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", output_bounds));
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
    config.window.active_border_color = 0xe28437;
    config.window.inactive_border_color = 0x3254a8;
    desktop.apply_config(config);
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, 0x3254a8));
    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, 0xe28437));
    config.window.border_width = 0;
    desktop.apply_config(config);
    REQUIRE(wait_pixel(connection, first, output.get(), x, y, 0));
    REQUIRE(hit(fixture.scene, geometry, *a));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

struct LayerSeat : seaside::TtyInputObserver {
    explicit LayerSeat(const Fixture &display) : fixture(display) {}
    ~LayerSeat() { fixture.desktop.set_input(nullptr); }

    void motion(seaside_pointer_motion kind, double dx, double dy) noexcept override {
        if (kind == SEASIDE_POINTER_ABSOLUTE) {
            pointer_absolute_delta(dx, dy, dx, dy);
        }
        x += dx;
        y += dy;
        fixture.desktop.motion(x, y);
    }

    bool pointer_absolute_delta(double nx, double ny, double &dx, double &dy) noexcept override {
        dx = nx * (2 * output_bounds.width) - x;
        dy = ny * output_bounds.height - y;
        return true;
    }

    bool shortcut_pressed(std::uint8_t mods, std::uint32_t keysym) noexcept override {
        if (keysym == XKB_KEY_F8) {
            ++shortcuts;
            return fixture.desktop.action(action(ActionKind::toggle_window_fullscreen), mods);
        }
        const auto binding = bindings.find({mods, seaside::ChordKind::keyboard, keysym});
        return binding != bindings.end() && fixture.desktop.action(binding->second, mods);
    }

    void modifiers_changed(std::uint8_t modifiers) noexcept override {
        fixture.desktop.modifiers_changed(modifiers);
    }

    bool pointer_target(seaside::TtyPointerTarget &target) noexcept override {
        return pointer_target_at(x, y, target);
    }

    bool pointer_target_at(double x, double y,
                           seaside::TtyPointerTarget &target) noexcept override {
        seaside_scene_target hit{.surface = target.surface, .id = 0, .sx = 0, .sy = 0};
        if (!fixture.scene.window_at(x, y, hit)) {
            return false;
        }
        target = {hit.surface, hit.sx, hit.sy, hit.id, x, y};
        return true;
    }

    bool pointer_content(wlr_surface *surface, seaside_pointer_content &content) noexcept override {
        return fixture.scene.pointer_content(surface, content);
    }

    bool place_pointer(double px, double py) noexcept override {
        x = px;
        y = py;
        ++placements;
        fixture.desktop.motion(x, y);
        return true;
    }

    void pointer_locked(bool value) noexcept override { locked = value; }

    bool pointer_button(std::uint64_t window, std::uint32_t button, bool pressed,
                        std::uint8_t mods) noexcept override {
        return fixture.desktop.button(window, button, pressed, mods, x, y);
    }

    void input_popup(wlr_input_popup_surface_v2 *popup, wlr_surface *surface,
                     const wlr_box *cursor) noexcept override {
        fixture.scene.set_input_popup(popup, surface, cursor);
    }

    bool start() {
        auto created = seaside::TtyInput::create(fixture.backend, *this, {.pointer_targets = true});
        if (!created || !(*created)->enable_keyboard(fixture.display)) {
            return false;
        }
        input = std::move(*created);
        devices.reset(part3_input_create(fixture.backend));
        fixture.desktop.set_input(input.get());
        return devices && !input->snapshot().failed;
    }

    void key_state(std::uint32_t code, bool pressed) const {
        part3_input_key(devices.get(), code, pressed);
    }

    void key(std::uint32_t code) const {
        key_state(code, true);
        key_state(code, false);
    }

    void click(double px, double py) {
        x = px;
        y = py;
        part3_input_button(devices.get(), BTN_LEFT, true);
        part3_input_button(devices.get(), BTN_LEFT, false);
    }

    const Fixture &fixture;
    std::unique_ptr<seaside::TtyInput> input;
    std::unique_ptr<part3_input, decltype(&part3_input_destroy)> devices{nullptr,
                                                                         part3_input_destroy};
    const seaside::BindingMap bindings = seaside::default_bindings();
    double x = 1, y = 1;
    unsigned shortcuts = 0;
    unsigned placements = 0;
    bool locked = false;
};

bool run_cross_output_cycle(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto *left_output = part3_output_handle(left.get());
    auto *right_output = part3_output_handle(right.get());
    REQUIRE(desktop.add_output(left_output, "left", output_bounds));
    REQUIRE(desktop.add_output(right_output, "right", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client first, second, remote;
    REQUIRE(connection.connect(first) && connection.open(first));
    REQUIRE(connection.attach(first, ordinary_width, ordinary_height));
    const auto a = desktop.keyboard_window();
    REQUIRE(a);
    REQUIRE(connection.connect(second) && connection.open(second));
    REQUIRE(connection.attach(second, ordinary_width, ordinary_height));
    const auto b = desktop.keyboard_window();
    REQUIRE(b && b != a);

    seat.click(200, 45);
    REQUIRE(!desktop.keyboard_window() && desktop.focused_output_handle() == right_output);
    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(desktop.keyboard_window() == b && desktop.focused_output_handle() == left_output);
    seat.click(200, 45);
    REQUIRE(connection.connect(remote) && connection.open(remote));
    REQUIRE(connection.attach(remote, ordinary_width, ordinary_height));
    const auto c = desktop.keyboard_window();
    REQUIRE(c && c != a && c != b);
    const auto box = windows.view(*a).geometry;
    seat.click(box.x + 1, box.y + 1);
    REQUIRE(connection.wait(first, [&] { return first.pointer_surface == first.surface; }));
    REQUIRE(desktop.keyboard_window() == a);
    Popup menu;
    REQUIRE(connection.open_popup(first, menu, nullptr, first.button_serial, 0));
    REQUIRE(first.keyboard_surface == menu.surface);

    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == c && desktop.focused_output_handle() == right_output);
    REQUIRE(connection.wait(first, [&] { return menu.dismissed; }));
    REQUIRE(connection.wait(remote, [&] { return remote.keyboard_surface == remote.surface; }));
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == b && desktop.focused_output_handle() == left_output);
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == a);
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == c);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(connection.roundtrip(first) && connection.roundtrip(remote));
    const auto first_keys = first.keys;
    const auto remote_keys = remote.keys;
    seat.key(KEY_C);
    REQUIRE(connection.roundtrip(first) && connection.roundtrip(remote));
    REQUIRE(remote.keys == remote_keys + 1 && first.keys == first_keys);
    REQUIRE(first.pointer_surface == first.surface && !remote.pointer_surface);
    REQUIRE(windows.state().output("left")->shown == 1);
    REQUIRE(windows.state().output("right")->shown == 6);

    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == a && desktop.focused_output_handle() == left_output);
    desktop.remove_output(right_output);
    right.reset();
    seat.key(KEY_TAB);
    REQUIRE(desktop.keyboard_window() == b);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(connection.roundtrip(second));
    const auto second_keys = second.keys;
    seat.key(KEY_B);
    REQUIRE(connection.roundtrip(second) && second.keys == second_keys + 1);
    REQUIRE(windows.state().output("left")->shown == 1 && !windows.view(*c).visible);
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(left_output);
    return true;
}

bool run_scratchpad(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto *left_output = part3_output_handle(left.get());
    auto *right_output = part3_output_handle(right.get());
    REQUIRE(desktop.add_output(left_output, "left", output_bounds));
    REQUIRE(desktop.add_output(right_output, "right", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client normal, secondary, scratch;
    REQUIRE(connection.connect(normal) && connection.open(normal));
    REQUIRE(connection.attach(normal, ordinary_width, ordinary_height));
    const auto a = desktop.keyboard_window();
    REQUIRE(a);
    seat.click(200, 45);
    REQUIRE(connection.connect(secondary) && connection.open(secondary));
    REQUIRE(connection.attach(secondary, ordinary_width, ordinary_height));
    const auto b = desktop.keyboard_window();
    REQUIRE(b && b != a);
    const auto toggle = [&] {
        seat.key_state(KEY_LEFTMETA, true);
        seat.key(KEY_GRAVE);
        seat.key_state(KEY_LEFTMETA, false);
    };
    toggle();
    REQUIRE(desktop.focused_output_handle() == left_output && !desktop.keyboard_window());
    REQUIRE(!windows.view(*a).visible);
    REQUIRE(connection.connect(scratch) && connection.open(scratch));
    REQUIRE(connection.attach(scratch, ordinary_width, ordinary_height));
    const auto s = desktop.keyboard_window();
    REQUIRE(s && s != a && s != b);
    REQUIRE(windows.state().window(*s)->home_workspace == seaside::scratchpad_workspace);
    const auto scratch_box = windows.view(*s).geometry;
    REQUIRE(hit(fixture.scene, scratch_box, *s));
    seat.click(scratch_box.x + 1, scratch_box.y + 1);
    REQUIRE(connection.wait(scratch, [&] { return scratch.pointer_surface == scratch.surface; }));
    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(desktop.keyboard_window() == b && desktop.focused_output_handle() == right_output);
    toggle();
    REQUIRE(desktop.keyboard_window() == b && desktop.focused_output_handle() == right_output);
    REQUIRE(windows.state().output("left")->shown == 1 && !windows.view(*s).visible);
    REQUIRE(connection.roundtrip(scratch) && connection.roundtrip(secondary));
    const auto scratch_keys = scratch.keys;
    const auto secondary_keys = secondary.keys;
    seat.key(KEY_A);
    REQUIRE(connection.roundtrip(scratch) && connection.roundtrip(secondary));
    REQUIRE(scratch.keys == scratch_keys && secondary.keys == secondary_keys + 1);
    REQUIRE(!scratch.keyboard_surface && !scratch.pointer_surface);
    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(desktop.keyboard_window() == a);

    seat.click(200, 45);
    toggle();
    REQUIRE(desktop.keyboard_window() == s && desktop.focused_output_handle() == left_output);
    REQUIRE(windows.view(*s).geometry == scratch_box);
    REQUIRE(desktop.dispatch(action(ActionKind::move_to_workspace, 3)).value_or(false));
    REQUIRE(windows.state().output("left")->shown == 3);
    REQUIRE(desktop.keyboard_window() == s);
    REQUIRE(desktop.dispatch(action(ActionKind::move_to_scratchpad)).value_or(false));
    REQUIRE(!desktop.keyboard_window() && !windows.view(*s).visible);
    toggle();
    REQUIRE(desktop.keyboard_window() == s);
    desktop.set_primary_output("right");
    REQUIRE(!windows.view(*s).visible);
    toggle();
    REQUIRE(desktop.keyboard_window() == s && desktop.focused_output_handle() == right_output);
    desktop.remove_output(right_output);
    right.reset();
    REQUIRE(!windows.view(*s).visible);
    toggle();
    REQUIRE(desktop.keyboard_window() == s && desktop.focused_output_handle() == left_output);
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    desktop.remove_output(left_output);
    return true;
}

bool run_wayland_input(const Fixture &fixture, const char *executable) {
    auto output = fixture.output();
    REQUIRE(output);
    auto *handle = part3_output_handle(output.get());
    auto &desktop = fixture.desktop;
    REQUIRE(desktop.add_output(handle, "main", output_bounds));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());

    int sockets[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    auto *connection = wl_client_create(fixture.display, sockets[0]);
    REQUIRE(connection);
    int transcript[2];
    REQUIRE(pipe2(transcript, O_CLOEXEC | O_NONBLOCK) == 0);
    const pid_t pid = fork();
    if (pid == 0) {
        char fd[16];
        std::snprintf(fd, sizeof(fd), "%d", sockets[1]);
        if (fcntl(sockets[1], F_SETFD, 0) < 0 || dup2(transcript[1], STDOUT_FILENO) < 0 ||
            setenv("WAYLAND_SOCKET", fd, 1) < 0) {
            _exit(1);
        }
        execl(executable, executable, "--verify-focus", nullptr);
        _exit(1);
    }
    close(sockets[1]);
    close(transcript[1]);
    struct Child {
        pid_t pid;
        int transcript;
        ~Child() {
            close(transcript);
            if (pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
        }
    } child{pid, transcript[0]};
    REQUIRE(pid > 0);
    std::string log;
    const auto pump = [&](std::string_view expected) {
        auto *loop = wl_display_get_event_loop(fixture.display);
        for (int i = 0; i < 200 && log.find(expected) == std::string::npos; ++i) {
            wl_display_flush_clients(fixture.display);
            if (wl_event_loop_dispatch(loop, 20) < 0) {
                return false;
            }
            char bytes[256];
            const auto size = read(child.transcript, bytes, sizeof(bytes));
            if (size > 0) {
                log.append(bytes, size);
            } else if (size == 0) {
                break;
            }
        }
        return log.find(expected) != std::string::npos;
    };
    REQUIRE(pump("Wayland input composition, popup, privacy, and key forwarding passed"));
    const auto window = desktop.keyboard_window();
    REQUIRE(window);
    const auto icon = fixture.scene.window_metadata(*window);
    REQUIRE(icon.icon_name && std::string_view(icon.icon_name) == "seaside-fixture");
    REQUIRE(icon.icon_png_base64 && std::string_view(icon.icon_png_base64).starts_with("iVBOR"));
    seat.key(KEY_A);
    REQUIRE(pump("physical grab key 30 state 0"));
    // Drain the forwarded key before removing focus.
    wl_display_flush_clients(fixture.display);
    REQUIRE(wl_event_loop_dispatch(wl_display_get_event_loop(fixture.display), 20) >= 0);
    REQUIRE(desktop.action(action(ActionKind::workspace, 2)));
    REQUIRE(pump("text input focus left"));
    REQUIRE(desktop.action(action(ActionKind::workspace, 1)));
    bool exited = false;
    for (int i = 0; i < 200 && !exited; ++i) {
        wl_display_flush_clients(fixture.display);
        REQUIRE(wl_event_loop_dispatch(wl_display_get_event_loop(fixture.display), 20) >= 0);
        int status;
        const auto finished = waitpid(child.pid, &status, WNOHANG);
        REQUIRE(finished >= 0);
        if (finished) {
            child.pid = -1;
            REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
            exited = true;
        }
    }
    REQUIRE(exited);
    desktop.remove_output(handle);
    return true;
}

template <bool outside_click> bool run_popup_shutdown(const Fixture &fixture) {
    auto output = fixture.output();
    REQUIRE(output);
    auto *handle = part3_output_handle(output.get());
    REQUIRE(fixture.desktop.add_output(handle, "main", output_bounds));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client client;
    REQUIRE(connection.connect(client) && connection.open(client) &&
            connection.attach(client, ordinary_width, ordinary_height));
    const auto focused = fixture.desktop.keyboard_window();
    REQUIRE(focused);
    const auto box = fixture.desktop.windows().view(*focused).geometry;
    seat.click(box.x + 4, box.y + 4);
    REQUIRE(connection.wait(client, [&] { return client.button_serial; }));
    Popup menu, submenu;
    REQUIRE(connection.open_popup(client, menu, nullptr, client.button_serial));
    REQUIRE(connection.open_popup(client, submenu, nullptr, client.button_serial, 0, &menu));
    REQUIRE(client.keyboard_surface == submenu.surface);
    if constexpr (outside_click) {
        // Real clients can still own both popup objects after popup_done.
        seat.click(0, 0);
        REQUIRE(connection.roundtrip(client) && menu.dismissed && submenu.dismissed);
    }
    // Follow the ordinary session exit path while the client is connected.
    fixture.desktop.set_input(nullptr);
    seat.input.reset();
    REQUIRE(connection.roundtrip(client) && menu.dismissed && submenu.dismissed);
    fixture.desktop.remove_output(handle);
    return true;
}

bool run_tile_popup(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output && desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client left, right;
    WindowId a{}, b{};
    REQUIRE(map_tiled(fixture, connection, left, a, 160, 90, 0, red));
    REQUIRE(map_tiled(fixture, connection, right, b, 80, 90, 0, green));
    REQUIRE(wait_tiled(connection, left, 80, 90) && connection.attach_configured(left, red));
    seat.click(50, 10);
    REQUIRE(connection.wait(
        left, [&] { return left.keyboard_surface == left.surface && left.button_serial; }));
    Popup menu;
    REQUIRE(connection.open_popup(left, menu, nullptr, left.button_serial, 66));
    seaside_scene_target target{};
    REQUIRE(fixture.scene.window_at(83, 12, target) && target.id == a &&
            target.surface != fixture.scene.window_surface(a) && target.sx == 7 && target.sy == 2);
    REQUIRE(wait_pixel(connection, left, output.get(), 83, 12, 0));
    REQUIRE(left.keyboard_surface == menu.surface);
    auto *handle = part3_output_handle(output.get());
    auto *menu_surface = target.surface;
    REQUIRE(left.subcompositor);
    Subsurface child;
    child.surface = wl_compositor_create_surface(left.compositor);
    child.role = wl_subcompositor_get_subsurface(left.subcompositor, child.surface, menu.surface);
    wl_subsurface_set_position(child.role, 4, 4);
    REQUIRE(connection.attach_subsurface(left, child, 3, 3, blue));
    wl_surface_commit(menu.surface);
    REQUIRE(connection.roundtrip(left) && wait_pixel(connection, left, output.get(), 81, 15, blue));
    target = {};
    REQUIRE(fixture.scene.window_at(81, 15, target) && target.id == a &&
            target.surface != menu_surface && target.sx == 1 && target.sy == 1);
    auto *child_surface = target.surface;
    REQUIRE(fixture.scene.visible_output(fixture.scene.window_surface(a)) == handle);
    REQUIRE(fixture.scene.visible_output(menu_surface) == handle);
    REQUIRE(fixture.scene.visible_output(child_surface) == handle);
    {
        Popup submenu;
        REQUIRE(connection.open_popup(left, submenu, nullptr, left.button_serial, 0, &menu));
        REQUIRE(left.keyboard_surface == submenu.surface);
        target = {};
        REQUIRE(fixture.scene.window_at(88, 22, target) && target.id == a && target.sx == 2 &&
                target.sy == 2);
        REQUIRE(fixture.scene.visible_output(target.surface) == handle);
    }
    REQUIRE(connection.roundtrip(left) && left.keyboard_surface == menu.surface);
    auto shifted = output_bounds;
    shifted.x = 217;
    REQUIRE(desktop.update_output(part3_output_handle(output.get()), shifted));
    target = {};
    REQUIRE(fixture.scene.window_at(shifted.x + 83, 12, target) && target.id == a &&
            target.sx == 7 && target.sy == 2);
    REQUIRE(wait_pixel(connection, left, output.get(), 83, 12, 0));

    REQUIRE(fixture.scene.visible_output(menu_surface) == handle);
    REQUIRE(fixture.scene.visible_output(child_surface) == handle);
    Popup submenu;
    REQUIRE(connection.open_popup(left, submenu, nullptr, left.button_serial, 0, &menu));
    REQUIRE(desktop.dispatch(action(ActionKind::cycle_focus_mru)).value_or(false));
    seat.key(KEY_C);
    REQUIRE(connection.roundtrip(left) && connection.roundtrip(right));
    REQUIRE(submenu.dismissed && menu.dismissed && left.keys == 0 && right.keys == 1);
    REQUIRE(right.keyboard_surface == right.surface && desktop.keyboard_window() == b);
    REQUIRE(wait_pixel(connection, right, output.get(), 83, 12, green));
    REQUIRE(!fixture.scene.visible_output(menu_surface) &&
            !fixture.scene.visible_output(child_surface));
    desktop.remove_output(part3_output_handle(output.get()));
    return true;
}

bool run_pending_opening_focus(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto right_bounds = output_bounds;
    right_bounds.x = output_bounds.width;
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", right_bounds));
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

    const auto opening_id = desktop.windows().focused_window();
    REQUIRE(opening_id && opening_id != second_id);
    seat.key(KEY_C);
    REQUIRE(desktop.dispatch(action(ActionKind::close_window)));
    REQUIRE(connection.roundtrip(second) && connection.roundtrip(opening));
    REQUIRE(second.keys == 1 && second.close_requested && !opening.close_requested);
    REQUIRE(desktop.dispatch(action(ActionKind::close_window), 0, opening_id));
    REQUIRE(connection.roundtrip(opening) && opening.close_requested);

    // Both keys must go to the visible client just clicked, before dispatch
    // can process a resize buffer or the opening deadline.
    seat.click(30, 45);
    seat.key(KEY_A);
    seat.click(210, 45);
    seat.key(KEY_B);
    REQUIRE(desktop.dispatch(action(ActionKind::close_window)));
    seaside_scene_target shown{};
    REQUIRE(fixture.scene.window_at(110, 70, shown) && shown.id == second_id);
    REQUIRE(connection.roundtrip(first) && connection.roundtrip(second) &&
            connection.roundtrip(remote));
    REQUIRE(first.keys == 1 && second.keys == 1 && remote.keys == 1 && remote.close_requested);
    REQUIRE(remote.keyboard_surface == remote.surface);

    REQUIRE(connection.attach_configured(second, blue));
    REQUIRE(wait_pixel(connection, opening, left.get(), 110, 70, green));
    REQUIRE(desktop.windows().focused_window() == remote_id);
    remote.destroy_window();
    REQUIRE(connection.roundtrip(remote));
    const auto empty_focus = desktop.dispatch(action(ActionKind::close_window));
    REQUIRE(!empty_focus && empty_focus.error() == seaside::DesktopActionError::no_target);
    desktop.remove_output(part3_output_handle(right.get()));
    desktop.remove_output(part3_output_handle(left.get()));
    return true;
}

bool run_alt_tab_during_opening(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto *left_output = part3_output_handle(left.get());
    auto *right_output = part3_output_handle(right.get());
    REQUIRE(desktop.add_output(left_output, "left", output_bounds));
    REQUIRE(desktop.add_output(right_output, "right", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client remote, current, pending;
    WindowId remote_id{}, current_id{};
    seat.click(210, 45);
    REQUIRE(map_tiled(fixture, connection, remote, remote_id, 160, 90, 0, blue));
    seat.click(30, 45);
    REQUIRE(map_tiled(fixture, connection, current, current_id, 160, 90, 0, red));
    REQUIRE(connection.connect(pending) && connection.open(pending));
    REQUIRE(connection.attach(pending, 8, 8, true, green));
    REQUIRE(desktop.windows().focused_window() != current_id);
    REQUIRE(desktop.keyboard_window() == current_id);
    seat.key_state(KEY_LEFTALT, true);
    seat.key(KEY_TAB);
    seat.key_state(KEY_LEFTALT, false);
    REQUIRE(desktop.keyboard_window() == remote_id);
    REQUIRE(connection.wait(remote, [&] { return remote.keyboard_surface == remote.surface; }));
    desktop.remove_output(right_output);
    desktop.remove_output(left_output);
    return true;
}

bool run_fullscreen_idle_focus(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    auto *left_output = part3_output_handle(left.get());
    auto *right_output = part3_output_handle(right.get());
    REQUIRE(desktop.add_output(left_output, "left", output_bounds));
    REQUIRE(desktop.add_output(right_output, "right", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());

    Connection connection(fixture.display);
    Client fullscreen, neighbor, opening;
    WindowId fullscreen_id{};
    REQUIRE(map_tiled(fixture, connection, fullscreen, fullscreen_id, 160, 90, 0, red));
    REQUIRE(connection.connect(neighbor) && connection.open(neighbor));
    REQUIRE(connection.attach_configured(neighbor, blue));
    REQUIRE(wait_tiled(connection, fullscreen, 80, 90));
    REQUIRE(connection.attach_configured(fullscreen, red));
    REQUIRE(
        connection.wait(neighbor, [&] { return neighbor.keyboard_surface == neighbor.surface; }));
    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, fullscreen_id)
                .value_or(false));
    REQUIRE(connection.wait(fullscreen, [&] { return fullscreen.configured.fullscreen; }));
    REQUIRE(connection.attach_configured(fullscreen, red));
    REQUIRE(desktop.keyboard_window() == fullscreen_id);
    REQUIRE(desktop.fullscreen_inhibits_idle(left_output) &&
            !desktop.fullscreen_inhibits_idle(right_output));

    // The newly mapped tile is selected in policy while the displayed
    // fullscreen window keeps keyboard focus until the new tile has resized.
    REQUIRE(connection.connect(opening) && connection.open(opening));
    REQUIRE(connection.attach(opening, 8, 8, true, green));
    REQUIRE(desktop.windows().focused_window() != fullscreen_id);
    REQUIRE(desktop.keyboard_window() == fullscreen_id);
    REQUIRE(desktop.fullscreen_inhibits_idle(left_output) &&
            !desktop.fullscreen_inhibits_idle(right_output));

    REQUIRE(connection.attach(opening, opening.configured.width, opening.configured.height, false,
                              green));
    REQUIRE(connection.wait(opening, [&] { return opening.keyboard_surface == opening.surface; }));
    REQUIRE(!desktop.fullscreen_inhibits_idle(left_output) &&
            !desktop.fullscreen_inhibits_idle(right_output));
    desktop.remove_output(right_output);
    desktop.remove_output(left_output);
    return true;
}

enum class OpeningFinish { right_first, left_first, close_right, close_focused, timeout };

template <OpeningFinish finish> bool run_overlapping_openings(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    OpeningDeadline deadline(desktop);
    auto left = fixture.output();
    auto right = fixture.output();
    REQUIRE(left && right);
    REQUIRE(desktop.add_output(part3_output_handle(left.get()), "left", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(right.get()), "right", {160, 0, 160, 90}));
    Connection connection(fixture.display);
    Client a, b, c, d;
    WindowId left_old{}, right_old{};
    desktop.button(0, BTN_LEFT, true, 0, 210, 45);
    REQUIRE(map_tiled(fixture, connection, b, right_old, 160, 90, 0, blue));
    desktop.button(0, BTN_LEFT, true, 0, 30, 45);
    REQUIRE(map_tiled(fixture, connection, a, left_old, 160, 90, 0, red));
    REQUIRE(connection.connect(c) && connection.connect(d));
    REQUIRE(connection.open(c) && connection.attach_configured(c, green));
    const auto left_new = desktop.windows().focused_window();
    REQUIRE(left_new && left_new != left_old && wait_tiled(connection, a, 80, 90));
    desktop.button(right_old, BTN_LEFT, true, 0, 210, 45);
    REQUIRE(connection.open(d) && connection.attach_configured(d, green));
    const auto right_new = desktop.windows().focused_window();
    REQUIRE(right_new && right_new != right_old && wait_tiled(connection, b, 80, 90));
    auto target = [&](int x) {
        seaside_scene_target shown{};
        return fixture.scene.window_at(x, 45, shown) ? shown.id : 0;
    };
    REQUIRE(target(110) == left_old && target(270) == right_old);
    REQUIRE(deadline.arms == 1);
    if constexpr (finish == OpeningFinish::timeout) {
        REQUIRE(deadline.start());
        REQUIRE(connection.wait(c, [&] { return target(110) == *left_new; }));
    } else if constexpr (finish == OpeningFinish::close_right) {
        // Removing the second opening changes the right layout again. The
        // surviving left participant still owns its outstanding configure.
        d.destroy_window();
        REQUIRE(connection.roundtrip(d));
        REQUIRE(target(110) == left_old && target(270) == right_old);
        REQUIRE(connection.attach_configured(a, red));
    } else if constexpr (finish == OpeningFinish::close_focused) {
        REQUIRE(desktop.keyboard_window() == right_old);
        b.destroy_window();
        REQUIRE(connection.roundtrip(b));
        REQUIRE(!desktop.keyboard_window() && target(110) == left_old);
        REQUIRE(connection.attach_configured(a, red));
        REQUIRE(desktop.keyboard_window() == right_new);
    } else {
        const bool right_first = finish == OpeningFinish::right_first;
        REQUIRE(connection.attach_configured(right_first ? b : a, right_first ? blue : red));
        REQUIRE(target(110) == left_old && target(270) == right_old);
        REQUIRE(connection.attach_configured(right_first ? a : b, right_first ? red : blue));
    }
    REQUIRE(target(110) == *left_new);
    REQUIRE(target(270) == (finish == OpeningFinish::close_right ? right_old : *right_new));
    REQUIRE(wait_pixel(connection, c, left.get(), 110, 45, green));
    REQUIRE(wait_pixel(connection, b, right.get(), 110, 45,
                       finish == OpeningFinish::close_right ? blue : green));
    desktop.remove_output(part3_output_handle(right.get()));
    desktop.remove_output(part3_output_handle(left.get()));
    REQUIRE(!desktop.failed() && !fixture.scene.snapshot().failed);
    return true;
}

bool run_client_gestures(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
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

    desktop.enable_frame_paced_resize();
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
    const WindowRect expanded{moved.x - 6, moved.y - 4, moved.width + 6, moved.height + 4};
    REQUIRE(connection.wait(client, [&] { return windows.view(*id).geometry == expanded; }));
    REQUIRE(connection.wait(client, [&] {
        return client.configured.width == expanded.width &&
               client.configured.height == expanded.height;
    }));
    REQUIRE(connection.attach_configured(client));
    REQUIRE(check_idle_resize(fixture, connection, client,
                              {{seat.x - 0.8, seat.y - 0.3}, {seat.x - 0.1, seat.y - 0.8}}));
    // The client gesture is absolute from the original top-left press.
    desktop.motion(seat.x - 9, seat.y - 6);
    desktop.motion(seat.x + 6.2, seat.y + 4.1);
    REQUIRE(connection.wait(client, [&] { return windows.view(*id).geometry == moved; }));
    REQUIRE(connection.wait(client, [&] { return client.configured.width == moved.width; }));
    REQUIRE(connection.attach_configured(client));
    seat.x += 32;
    seat.y += 18;
    desktop.motion(seat.x, seat.y);
    const WindowRect clamped{moved.x + moved.width - 40, moved.y + moved.height - 26, 40, 26};
    REQUIRE(connection.wait(client, [&] { return windows.view(*id).geometry == clamped; }));
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
                        XDG_TOPLEVEL_RESIZE_EDGE_RIGHT);
    REQUIRE(connection.wait(client, [&] { return client.configured.resizing; }));
    REQUIRE(check_idle_resize(fixture, connection, client, {{seat.x + 0.7, seat.y + 10}}));
    desktop.motion(seat.x + 8, seat.y);
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
    REQUIRE(first && second && desktop.add_output(first, "HEADLESS-1", output_bounds) &&
            desktop.add_output(second, "HEADLESS-2", {160, 0, 160, 90}));
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
    {
        Popup menu;
        REQUIRE(connection.open_popup(client, menu, &panel));
        seat.click(16, 17);
        REQUIRE(connection.roundtrip(client) && client.pointer_surface == menu.surface &&
                client.keyboard_surface == client.surface);
    }
    seat.click(4, 5);
    REQUIRE(connection.roundtrip(client));
    {
        Popup denied;
        REQUIRE(!connection.open_popup(client, denied, &panel, client.button_serial));
        REQUIRE(denied.dismissed && client.keyboard_surface == client.surface);
    }
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
    REQUIRE(client.keyboard_surface == popup.surface);
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
    const auto *server_seat = seat.input->seat();
    REQUIRE(connection.wait(client, [&] { return part3_seat_has_drag(server_seat); }));
    const auto fullscreen_during_drag =
        desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, id);
    REQUIRE(fullscreen_during_drag && *fullscreen_during_drag);
    REQUIRE(connection.wait(client, [&] { return client.configured.fullscreen; }));
    REQUIRE(!part3_seat_has_drag(server_seat));
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    seat.key(KEY_F8);
    REQUIRE(connection.wait(client, [&] { return !client.configured.fullscreen; }));

    auto config = seaside::default_config();
    config.window.gaps_outer = 4;
    config.window.gaps_inner = 0;
    config.window.border_width = 0;
    desktop.apply_config(config);
    const auto banner =
        seaside::render_config_banner("Configuration error\nInvalid value", output_bounds.width, 1);
    REQUIRE(banner);
    scene.set_config_error(
        seaside::Diagnostic{.path = {}, .field = {}, .message = "Invalid value"});
    const auto banner_height = banner->logical_height;
    const WindowRect below_banner{4, reserved.y + banner_height + 4, 152,
                                  reserved.height - banner_height - 8};
    REQUIRE(connection.wait(client,
                            [&] { return desktop.windows().view(id).geometry == below_banner; }));
    REQUIRE(wait_pixel(connection, client, primary.get(), 4, banner_height + 5, green));
    const auto &banner_pixel = banner->pixels;
    const auto banner_color =
        static_cast<std::uint32_t>(banner_pixel[0] | banner_pixel[1] << 8 | banner_pixel[2] << 16);
    REQUIRE(wait_pixel(connection, client, other.get(), 0, 0, banner_color));

    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_maximize), 0, id).value_or(false));
    REQUIRE(desktop.windows().view(id).geometry == below_banner);
    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, id).value_or(false));
    REQUIRE(desktop.windows().view(id).geometry == output_bounds);
    REQUIRE(connection.wait(client, [&] { return client.configured.fullscreen; }));
    REQUIRE(connection.attach_configured(client, red));
    REQUIRE(wait_pixel(connection, client, primary.get(), 0, 0, red));
    REQUIRE(wait_pixel(connection, client, other.get(), 0, 0, banner_color));
    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_fullscreen), 0, id).value_or(false));
    REQUIRE(desktop.windows().view(id).geometry == below_banner);
    REQUIRE(wait_pixel(connection, client, primary.get(), 0, 0, banner_color));
    REQUIRE(desktop.dispatch(action(ActionKind::toggle_window_maximize), 0, id).value_or(false));
    scene.set_config_error(std::nullopt);
    config.window.gaps_outer = 0;
    desktop.apply_config(config);
    REQUIRE(
        connection.wait(client, [&] { return desktop.windows().view(id).geometry == reserved; }));
    REQUIRE(wait_pixel(connection, client, primary.get(), 4, 5, green));

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
    REQUIRE(handle && desktop.add_output(handle, "HEADLESS-1", output_bounds));

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

struct Confinement {
    unsigned entered = 0;
    unsigned exited = 0;
};

bool map_pointer_clients(const Fixture &fixture, Connection &connection, Client &passive,
                         Client &capture, int width, int height) {
    REQUIRE(connection.connect(passive) && connection.open(passive));
    REQUIRE(connection.attach(passive, ordinary_width, ordinary_height));
    xdg_toplevel_set_fullscreen(passive.toplevel, nullptr);
    REQUIRE(connection.wait(passive, [&] { return passive.configured.fullscreen; }));
    REQUIRE(connection.attach_configured(passive));

    fixture.desktop.button(0, BTN_LEFT, true, 0, 170, 45);
    REQUIRE(connection.connect(capture) && connection.open(capture));
    REQUIRE(connection.attach(capture, ordinary_width, ordinary_height));
    xdg_toplevel_set_fullscreen(capture.toplevel, nullptr);
    REQUIRE(connection.wait(capture, [&] { return capture.configured.fullscreen; }));
    REQUIRE(connection.attach(capture, width, height));
    REQUIRE(connection.wait(capture, [&] {
        return capture.keyboard_surface == capture.surface && capture.constraints &&
               capture.pointer;
    }));
    return true;
}

bool run_fullscreen_confinement(const Fixture &fixture, int width, int height, int content_left,
                                bool reproject = false) {
    auto &desktop = fixture.desktop;
    auto primary = fixture.output();
    auto secondary = fixture.output();
    constexpr WindowRect neighbor{160, 0, 160, 90};
    REQUIRE(primary && secondary);
    REQUIRE(desktop.add_output(part3_output_handle(primary.get()), "main", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(secondary.get()), "neighbor", neighbor));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    Connection connection(fixture.display);
    Client passive, capture;
    REQUIRE(map_pointer_clients(fixture, connection, passive, capture, width, height));
    seat.input->set_auto_center_on_lock(true);
    const int inside = content_left + 10;
    seaside_scene_target point{};
    REQUIRE(fixture.scene.window_at(inside, 45, point) && point.id == desktop.keyboard_window());

    seat.x = 155;
    seat.y = 45;
    seat.input->refresh_pointer();
    REQUIRE(connection.wait(passive, [&] { return passive.pointer_surface == passive.surface; }));
    Confinement trace;
    const std::unique_ptr<zwp_confined_pointer_v1, decltype(&zwp_confined_pointer_v1_destroy)>
        confine(zwp_pointer_constraints_v1_confine_pointer(
                    capture.constraints, capture.surface, capture.pointer, nullptr,
                    ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT),
                zwp_confined_pointer_v1_destroy);
    const zwp_confined_pointer_v1_listener listener{
        .confined = [](void *data,
                       zwp_confined_pointer_v1 *) { ++static_cast<Confinement *>(data)->entered; },
        .unconfined =
            [](void *data, zwp_confined_pointer_v1 *) {
                ++static_cast<Confinement *>(data)->exited;
            }};
    zwp_confined_pointer_v1_add_listener(confine.get(), &listener, &trace);
    REQUIRE(connection.roundtrip(capture) && trace.entered == 0);
    part3_input_relative(seat.devices.get(), inside - 155, 0, 9, 0);
    REQUIRE(connection.wait(capture, [&] { return trace.entered == 1; }));
    REQUIRE(capture.pointer_surface == capture.surface && seat.x == inside && seat.placements == 0);
    REQUIRE(connection.roundtrip(passive));
    const auto passive_motions = passive.pointer_motions;

    part3_input_relative(seat.devices.get(), -80, 0, -60, 0);
    REQUIRE(connection.roundtrip(capture) && connection.roundtrip(passive));
    std::fprintf(stderr, "Confinement %dx%d: x=%.3f activation=%u deactivation=%u\n", width, height,
                 seat.x, trace.entered, trace.exited);
    REQUIRE(seat.x >= content_left && seat.x < content_left + 1 && trace.exited == 0 &&
            passive.pointer_surface == nullptr && passive.pointer_motions == passive_motions);

    part3_input_absolute(seat.devices.get(), 0.1, 0.5);
    REQUIRE(connection.roundtrip(capture) && connection.roundtrip(passive));
    REQUIRE(seat.x >= content_left && seat.x < content_left + 1 && trace.exited == 0);
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    REQUIRE(connection.roundtrip(capture) && connection.roundtrip(passive));
    REQUIRE(capture.button_serial != 0 && passive.button_serial == 0);
    if (reproject) {
        REQUIRE(part3_output_set_scale(secondary.get(), 1.25F) &&
                part3_output_set_transform(secondary.get(), WL_OUTPUT_TRANSFORM_90) &&
                desktop.update_output(part3_output_handle(secondary.get()), {160, 0, 72, 128}));
        REQUIRE(connection.roundtrip(capture));
        REQUIRE(trace.entered == 1 && trace.exited == 0);
        part3_input_relative(seat.devices.get(), -100, -100, -80, -80);
        REQUIRE(connection.roundtrip(capture) && connection.roundtrip(passive));
        REQUIRE(seat.x >= 160 && seat.x < 161 && seat.y >= 43 && seat.y < 44 && trace.exited == 0 &&
                passive.pointer_surface == nullptr);
    }
    return true;
}

struct LockEvents {
    const Client &client;
    unsigned entered = 0, exited = 0;
    bool entered_after_pointer = false;
};

struct RelativeEvents {
    unsigned count = 0;
    double dx = 0, dy = 0, raw_x = 0, raw_y = 0;
};

bool run_fullscreen_lock(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto primary = fixture.output();
    auto secondary = fixture.output();
    constexpr WindowRect neighbor{160, 0, 160, 90};
    REQUIRE(primary && secondary);
    REQUIRE(desktop.add_output(part3_output_handle(primary.get()), "main", output_bounds));
    REQUIRE(desktop.add_output(part3_output_handle(secondary.get()), "neighbor", neighbor));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    seat.input->set_auto_center_on_lock(true);
    Connection connection(fixture.display);
    Client passive, capture;
    REQUIRE(map_pointer_clients(fixture, connection, passive, capture, 300, 300));
    REQUIRE(passive.relative_manager && capture.relative_manager);

    seat.x = 155;
    seat.y = 45;
    seat.input->refresh_pointer();
    REQUIRE(connection.wait(passive, [&] { return passive.pointer_surface == passive.surface; }));
    REQUIRE(seat.placements == 0 && !seat.locked);

    const zwp_relative_pointer_v1_listener relative_listener{
        .relative_motion = [](void *data, zwp_relative_pointer_v1 *, std::uint32_t, std::uint32_t,
                              wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t raw_x, wl_fixed_t raw_y) {
            auto &trace = *static_cast<RelativeEvents *>(data);
            trace = {trace.count + 1, wl_fixed_to_double(dx), wl_fixed_to_double(dy),
                     wl_fixed_to_double(raw_x), wl_fixed_to_double(raw_y)};
        }};
    RelativeEvents capture_relative, passive_relative;
    using RelativePointer =
        std::unique_ptr<zwp_relative_pointer_v1, decltype(&zwp_relative_pointer_v1_destroy)>;
    const auto subscribe = [&](Client &client, RelativeEvents &trace) {
        RelativePointer relative(zwp_relative_pointer_manager_v1_get_relative_pointer(
                                     client.relative_manager, client.pointer),
                                 zwp_relative_pointer_v1_destroy);
        zwp_relative_pointer_v1_add_listener(relative.get(), &relative_listener, &trace);
        return relative;
    };
    const auto capture_stream = subscribe(capture, capture_relative);
    const auto passive_stream = subscribe(passive, passive_relative);
    REQUIRE(connection.roundtrip(passive));

    const std::unique_ptr<wl_region, decltype(&wl_region_destroy)> region(
        wl_compositor_create_region(capture.compositor), wl_region_destroy);
    wl_region_add(region.get(), 0, 0, 40, 300);
    LockEvents trace{capture};
    const std::unique_ptr<zwp_locked_pointer_v1, decltype(&zwp_locked_pointer_v1_destroy)> lock(
        zwp_pointer_constraints_v1_lock_pointer(capture.constraints, capture.surface,
                                                capture.pointer, region.get(),
                                                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT),
        zwp_locked_pointer_v1_destroy);
    const zwp_locked_pointer_v1_listener listener{
        .locked =
            [](void *data, zwp_locked_pointer_v1 *) {
                auto &events = *static_cast<LockEvents *>(data);
                ++events.entered;
                events.entered_after_pointer =
                    events.client.pointer_surface == events.client.surface;
            },
        .unlocked = [](void *data,
                       zwp_locked_pointer_v1 *) { ++static_cast<LockEvents *>(data)->exited; }};
    zwp_locked_pointer_v1_add_listener(lock.get(), &listener, &trace);
    REQUIRE(connection.roundtrip(capture));
    REQUIRE(seat.x == 155 && trace.entered == 0);
    zwp_locked_pointer_v1_set_region(lock.get(), nullptr);
    REQUIRE(connection.roundtrip(capture) && trace.entered == 0);
    wl_surface_commit(capture.surface);
    REQUIRE(connection.wait(capture, [&] { return trace.entered == 1; }));
    REQUIRE(trace.entered_after_pointer && seat.locked && seat.placements == 1 && seat.x == 240 &&
            seat.y == 45 && capture_relative.count == 0);

    const auto regular = capture.pointer_motions;
    part3_input_relative(seat.devices.get(), -250, -40, -201, -28);
    part3_input_absolute(seat.devices.get(), 0.1, 0.8);
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    REQUIRE(connection.roundtrip(capture) && connection.roundtrip(passive));
    REQUIRE(seat.x == 240 && seat.y == 45 && seat.placements == 1 &&
            capture.pointer_motions == regular && capture_relative.count == 1 &&
            capture_relative.dx == -250 && capture_relative.dy == -40 &&
            capture_relative.raw_x == -201 && capture_relative.raw_y == -28 &&
            passive_relative.count == 0 && passive.pointer_surface == nullptr &&
            passive.button_serial == 0 && capture.button_serial != 0);

    {
        Popup popup;
        REQUIRE(connection.open_popup(capture, popup, nullptr, capture.button_serial));
        REQUIRE(connection.roundtrip(capture) && trace.exited == 1 && !seat.locked);
        seat.input->refresh_pointer();
        REQUIRE(connection.roundtrip(capture) && trace.entered == 1 && seat.placements == 1);
        seat.input->set_auto_center_on_lock(false);
    }
    REQUIRE(connection.wait(capture, [&] { return trace.entered == 2; }));
    REQUIRE(seat.x == 240 && seat.placements == 1);
    zwp_locked_pointer_v1_set_region(lock.get(), region.get());
    wl_surface_commit(capture.surface);
    REQUIRE(connection.wait(capture, [&] { return trace.exited == 2; }));
    part3_input_relative(seat.devices.get(), -40, 0, -36, 0);
    REQUIRE(connection.wait(capture, [&] { return trace.entered == 3; }));
    REQUIRE(seat.x == 200 && seat.placements == 1 && seat.locked);
    seat.input->set_auto_center_on_lock(true);
    seat.input->refresh_pointer();
    REQUIRE(connection.roundtrip(capture) && seat.x == 200 && seat.placements == 1 &&
            trace.entered == 3 && trace.exited == 2);

    seat.key(KEY_F8);
    REQUIRE(connection.wait(capture, [&] { return !capture.configured.fullscreen; }));
    REQUIRE(connection.attach(capture, ordinary_width, ordinary_height));
    REQUIRE(trace.exited == 3 && !seat.locked);
    zwp_locked_pointer_v1_set_region(lock.get(), nullptr);
    wl_surface_commit(capture.surface);
    REQUIRE(connection.roundtrip(capture) && trace.entered == 3 && seat.placements == 1);

    const auto focused = desktop.keyboard_window();
    REQUIRE(focused);
    const auto window = desktop.windows().view(*focused).geometry;
    part3_input_relative(seat.devices.get(), window.x + 2 - seat.x, window.y + 2 - seat.y, 8, 3);
    REQUIRE(connection.wait(capture, [&] { return trace.entered == 4; }));
    REQUIRE(seat.placements == 2 && seat.x == window.x + window.width / 2.0 &&
            seat.y == window.y + window.height / 2.0);
    seat.input->set_auto_center_on_lock(false);
    desktop.remove_output(part3_output_handle(secondary.get()));
    REQUIRE(connection.wait(capture, [&] { return trace.exited == 4; }));
    REQUIRE(!seat.locked);
    return true;
}

bool run_lock_around_compositor_drag(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto primary = fixture.output();
    auto secondary = fixture.output();
    REQUIRE(primary && secondary);
    REQUIRE(desktop.add_output(part3_output_handle(primary.get()), "main", output_bounds));
    REQUIRE(
        desktop.add_output(part3_output_handle(secondary.get()), "neighbor", {160, 0, 160, 90}));
    LayerSeat seat(fixture);
    REQUIRE(seat.start());
    seat.input->set_auto_center_on_lock(true);
    Connection connection(fixture.display);
    Client passive, capture;
    REQUIRE(connection.connect(passive) && connection.open(passive));
    REQUIRE(connection.attach(passive, ordinary_width, ordinary_height));
    const auto first = desktop.keyboard_window();
    REQUIRE(first);
    seat.click(240, 45);
    REQUIRE(connection.connect(capture) && connection.open(capture));
    REQUIRE(connection.attach(capture, ordinary_width, ordinary_height));
    const auto second = desktop.keyboard_window();
    REQUIRE(second && second != first && capture.constraints && capture.pointer);
    const auto passive_box = desktop.windows().view(*first).geometry;
    seat.click(passive_box.x + 2, passive_box.y + 2);
    REQUIRE(desktop.keyboard_window() == first);

    const auto original = desktop.windows().view(*second).geometry;
    seat.x = original.x + 2;
    seat.y = original.y + 2;
    seat.input->refresh_pointer();
    REQUIRE(connection.wait(capture, [&] { return capture.pointer_surface == capture.surface; }));
    unsigned activations = 0;
    const zwp_locked_pointer_v1_listener listener{
        .locked = [](void *data, zwp_locked_pointer_v1 *) { ++*static_cast<unsigned *>(data); },
        .unlocked = [](void *, zwp_locked_pointer_v1 *) {},
    };
    const std::unique_ptr<zwp_locked_pointer_v1, decltype(&zwp_locked_pointer_v1_destroy)> lock(
        zwp_pointer_constraints_v1_lock_pointer(capture.constraints, capture.surface,
                                                capture.pointer, nullptr,
                                                ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT),
        zwp_locked_pointer_v1_destroy);
    zwp_locked_pointer_v1_add_listener(lock.get(), &listener, &activations);
    REQUIRE(connection.roundtrip(capture) && activations == 0);

    seat.key_state(KEY_LEFTMETA, true);
    part3_input_button(seat.devices.get(), BTN_LEFT, true);
    REQUIRE(desktop.keyboard_window() == second);
    REQUIRE(seat.placements == 0 && desktop.windows().view(*second).geometry == original);
    part3_input_relative(seat.devices.get(), 5, 0, 5, 0);
    REQUIRE(connection.roundtrip(capture) && activations == 0 && capture.button_serial == 0);
    auto moved = original;
    moved.x += 5;
    REQUIRE(desktop.windows().view(*second).geometry == moved);
    part3_input_button(seat.devices.get(), BTN_LEFT, false);
    seat.key_state(KEY_LEFTMETA, false);
    REQUIRE(connection.wait(capture, [&] { return activations == 1; }));
    REQUIRE(seat.placements == 1 && seat.locked && capture.button_serial == 0);
    REQUIRE(seat.x == moved.x + moved.width / 2.0 && seat.y == moved.y + moved.height / 2.0);
    REQUIRE(desktop.windows().view(*second).geometry == moved);

    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(desktop.keyboard_window() == first && !seat.locked);
    seat.click(moved.x + 2, moved.y + 2);
    REQUIRE(
        connection.wait(capture, [&] { return activations == 2 && capture.button_serial > 0; }));
    REQUIRE(seat.placements == 2 && seat.locked);
    return true;
}

bool run_dwindle(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "main", output_bounds));
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
    REQUIRE(wait_pixel(connection, first, output.get(), 80, 4, active_border));
    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(windows.focused_window() == a && windows.view(b).maximized);
    REQUIRE(wait_pixel(connection, first, output.get(), 20, 45, red));
    REQUIRE(wait_pixel(connection, first, output.get(), 5, 45, active_border));
    REQUIRE(wait_pixel(connection, first, output.get(), 80, 4, inactive_border));
    REQUIRE(desktop.action(action(ActionKind::cycle_focus_mru)));
    REQUIRE(windows.focused_window() == b && windows.view(b).maximized);
    REQUIRE(wait_pixel(connection, first, output.get(), 20, 45, green));
    REQUIRE(wait_pixel(connection, first, output.get(), 80, 4, active_border));
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
    REQUIRE(desktop.add_output(part3_output_handle(primary.get()), "main", output_bounds));
    constexpr WindowRect adjacent{160, 0, 160, 90};
    REQUIRE(desktop.add_output(part3_output_handle(neighbor.get()), "adjacent", adjacent));
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

    REQUIRE(desktop.action(action(ActionKind::workspace, 4)));
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
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "headless", output_bounds));
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
    REQUIRE(desktop.action(action(ActionKind::move_to_workspace, 4)));
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
    REQUIRE(windows.focused_window() == second_id && windows.focused_workspace() == 4);
    xdg_activation_v1_activate(first.activation, token.c_str(), first.surface);
    REQUIRE(connection.roundtrip(first) && attention->count == 1);
    desktop.on_attention({});

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
    REQUIRE(windows.view(*second_id).geometry.y > 0);
    xdg_toplevel_set_fullscreen(second.toplevel, nullptr);
    REQUIRE(connection.wait(second, [&] { return second.configured.fullscreen; }));
    REQUIRE(connection.attach_configured(second, blue));
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, blue));
    seaside_scene_target target{};
    REQUIRE(scene.window_at(1, 1, target) && target.id == *second_id);
    xdg_toplevel_unset_fullscreen(second.toplevel);
    REQUIRE(connection.wait(second, [&] { return !second.configured.fullscreen; }));
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, color));
    scene.set_config_error(std::nullopt);
    REQUIRE(wait_pixel(connection, second, output.get(), 0, 0, 0));
    return true;
}

bool run_binding_reload(const Fixture &fixture) {
    auto &desktop = fixture.desktop;
    const auto &windows = desktop.windows();
    auto output = fixture.output();
    REQUIRE(output);
    REQUIRE(desktop.add_output(part3_output_handle(output.get()), "reload", output_bounds));
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

extern "C" wl_event_source *__wrap_wl_event_loop_add_timer(wl_event_loop *loop,
                                                           wl_event_loop_timer_func_t callback,
                                                           void *data) {
    auto *timer = __real_wl_event_loop_add_timer(loop, callback, data);
    if (auto *deadline = OpeningDeadline::active; deadline && data == deadline->desktop) {
        deadline->timer = timer;
    }
    return timer;
}

extern "C" int __wrap_wl_event_source_timer_update(wl_event_source *source, int delay) {
    if (auto *deadline = OpeningDeadline::active; deadline && source == deadline->timer) {
        deadline->delay = delay;
        if (delay > 0) {
            ++deadline->arms;
            return 0;
        }
    }
    return __real_wl_event_source_timer_update(source, delay);
}

int main(int argc, char **argv) {
    auto config = seaside::default_config();
    if (argc == 1) {
        config.window.tiling = false;
        return run_fixture(config, run_part4) && run_fixture(config, run_sparse_view_ids) &&
                       run_fixture(config, run_workspace_topology) &&
                       run_fixture(config, run_cross_output_cycle) &&
                       run_fixture(config, run_scratchpad) &&
                       run_fixture(config, run_floating_borders) &&
                       run_fixture(config, run_floating_resize_dialogs) &&
                       run_fixture(config, run_floating_resize_limits) &&
                       run_fixture(config, run_floating_delayed_commits) &&
                       run_fixture(config, run_floating_resize_edges) &&
                       run_fixture(config, run_client_gestures) &&
                       run_fixture(config, run_popup_shutdown<false>) &&
                       run_fixture(config, run_popup_shutdown<true>)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    const std::string_view scenario = argv[1];
    if (argc == 3 && scenario == "wayland-input") {
        config.window.tiling = false;
        return run_fixture(
                   config,
                   [&](const Fixture &fixture) { return run_wayland_input(fixture, argv[2]); })
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "titlebars") {
        return run_fixture(config, run_titlebars) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
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
    if (argc == 2 && scenario == "fullscreen-confinement") {
        config.window.tiling = false;
        auto stretch = config;
        stretch.render.fullscreen_aspect = seaside::FullscreenAspect::stretch;
        return run_fixture(config,
                           [](const Fixture &fixture) {
                               return run_fullscreen_confinement(fixture, 320, 180, 160, true);
                           }) &&
                       run_fixture(config,
                                   [](const Fixture &fixture) {
                                       return run_fullscreen_confinement(fixture, 60, 60, 195);
                                   }) &&
                       run_fixture(stretch,
                                   [](const Fixture &fixture) {
                                       return run_fullscreen_confinement(fixture, 320, 320, 160);
                                   })
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }
    if (argc == 2 && scenario == "fullscreen-lock") {
        config.window.tiling = false;
        return run_fixture(config, run_fullscreen_lock) &&
                       run_fixture(config, run_lock_around_compositor_drag)
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
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
        auto edge_drag = transaction;
        edge_drag.window.allow_edge_drag = true;
        config.window.gaps_outer = 4;
        config.window.gaps_inner = 6;
        return run_fixture(config, run_dwindle) && run_fixture(edge_drag, run_edge_drag) &&
                       run_fixture(transaction, run_tile_popup) &&
                       run_fixture(transaction, run_opening_transaction) &&
                       run_fixture(transaction, run_workspace_during_opening) &&
                       run_fixture(transaction, run_pending_opening_focus) &&
                       run_fixture(transaction, run_alt_tab_during_opening) &&
                       run_fixture(transaction, run_fullscreen_idle_focus) &&
                       run_fixture(transaction,
                                   run_overlapping_openings<OpeningFinish::right_first>) &&
                       run_fixture(transaction,
                                   run_overlapping_openings<OpeningFinish::left_first>) &&
                       run_fixture(transaction,
                                   run_overlapping_openings<OpeningFinish::close_right>) &&
                       run_fixture(transaction,
                                   run_overlapping_openings<OpeningFinish::close_focused>) &&
                       run_fixture(transaction, run_overlapping_openings<OpeningFinish::timeout>)
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

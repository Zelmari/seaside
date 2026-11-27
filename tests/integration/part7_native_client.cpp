#include "pointer-constraints-client-protocol.h"
#include "relative-pointer-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <charconv>
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
#include <string_view>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>
#include <wayland-client.h>

using Clock = std::chrono::steady_clock;

enum class pointer_mode { disabled, none, lock, confine };

struct probe_region {
    bool specified = false;
    int32_t x = 0, y = 0, width = 0, height = 0;
};

struct client {
    struct wl_display *display;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *shell;
    struct wl_surface *surface;
    struct xdg_toplevel *toplevel;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
    struct zwp_relative_pointer_v1 *relative;
    struct zwp_locked_pointer_v1 *lock;
    struct zwp_confined_pointer_v1 *confine;
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
    bool fullscreen;
    bool oneshot;
    bool defer_constraint;
    pointer_mode mode;
    probe_region region;
    probe_region input_region;
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
static void request_constraint(struct client *client);

static struct wl_region *make_region(struct client *client, const probe_region &region) {
    if (!region.specified) {
        return nullptr;
    }
    struct wl_region *proxy = wl_compositor_create_region(client->compositor);
    if (region.width && region.height) {
        wl_region_add(proxy, region.x, region.y, region.width, region.height);
    }
    return proxy;
}

static void print_region(const char *kind, const probe_region &region) {
    if (!region.specified) {
        printf("%s null\n", kind);
    } else {
        printf("%s %d %d %d %d\n", kind, region.x, region.y, region.width, region.height);
    }
}

static void set_input_region(struct client *client) {
    struct wl_region *region = make_region(client, client->input_region);
    wl_surface_set_input_region(client->surface, region);
    if (region) {
        wl_region_destroy(region);
    }
    print_region("pending-input-region", client->input_region);
}

static void locked(void *, struct zwp_locked_pointer_v1 *) {
    puts("locked");
}
static void unlocked(void *, struct zwp_locked_pointer_v1 *) {
    puts("unlocked");
}
static void confined(void *, struct zwp_confined_pointer_v1 *) {
    puts("confined");
}
static void unconfined(void *, struct zwp_confined_pointer_v1 *) {
    puts("unconfined");
}

static const struct zwp_locked_pointer_v1_listener lock_listener = {.locked = locked,
                                                                    .unlocked = unlocked};
static const struct zwp_confined_pointer_v1_listener confine_listener = {.confined = confined,
                                                                         .unconfined = unconfined};

static void request_constraint(struct client *client) {
    if (!client->configured || !client->pointer || client->lock || client->confine ||
        (client->mode != pointer_mode::lock && client->mode != pointer_mode::confine)) {
        return;
    }
    struct wl_region *region = make_region(client, client->region);
    const uint32_t lifetime = client->oneshot ? ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT
                                              : ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT;
    const bool lock = client->mode == pointer_mode::lock;
    printf("request %s %s\n", lock ? "lock" : "confine",
           client->oneshot ? "oneshot" : "persistent");
    print_region("request-region", client->region);
    if (lock) {
        client->lock = zwp_pointer_constraints_v1_lock_pointer(client->constraints, client->surface,
                                                               client->pointer, region, lifetime);
        zwp_locked_pointer_v1_add_listener(client->lock, &lock_listener, client);
    } else {
        client->confine = zwp_pointer_constraints_v1_confine_pointer(
            client->constraints, client->surface, client->pointer, region, lifetime);
        zwp_confined_pointer_v1_add_listener(client->confine, &confine_listener, client);
    }
    if (region) {
        wl_region_destroy(region);
    }
}

static void release_constraint(struct client *client) {
    if (client->lock) {
        zwp_locked_pointer_v1_destroy(client->lock);
        client->lock = nullptr;
        puts("destroy lock");
    }
    if (client->confine) {
        zwp_confined_pointer_v1_destroy(client->confine);
        client->confine = nullptr;
        puts("destroy confine");
    }
}

static void pointer_enter(void *, struct wl_pointer *, uint32_t serial, struct wl_surface *,
                          wl_fixed_t x, wl_fixed_t y) {
    printf("pointer-enter %u %.3f %.3f\n", serial, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_leave(void *, struct wl_pointer *, uint32_t serial, struct wl_surface *) {
    printf("pointer-leave %u\n", serial);
}

static void pointer_motion(void *, struct wl_pointer *, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    printf("pointer-motion %u %.3f %.3f\n", time, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_button(void *, struct wl_pointer *, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state) {
    printf("pointer-button %u %u %u %u\n", serial, time, button, state);
}

static void pointer_axis(void *, struct wl_pointer *, uint32_t time, uint32_t axis,
                         wl_fixed_t value) {
    printf("pointer-axis %u %u %.3f\n", time, axis, wl_fixed_to_double(value));
}

static const auto pointer_listener = [] {
    struct wl_pointer_listener listener{};
    listener.enter = pointer_enter;
    listener.leave = pointer_leave;
    listener.motion = pointer_motion;
    listener.button = pointer_button;
    listener.axis = pointer_axis;
    return listener;
}();

static void relative_motion(void *, struct zwp_relative_pointer_v1 *, uint32_t hi, uint32_t lo,
                            wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t raw_x, wl_fixed_t raw_y) {
    printf("relative-motion %u:%u %.3f %.3f %.3f %.3f\n", hi, lo, wl_fixed_to_double(dx),
           wl_fixed_to_double(dy), wl_fixed_to_double(raw_x), wl_fixed_to_double(raw_y));
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {.relative_motion =
                                                                              relative_motion};

static void keyboard_keymap(void *, struct wl_keyboard *, uint32_t, int32_t fd, uint32_t) {
    close(fd);
}

static void keyboard_enter(void *, struct wl_keyboard *, uint32_t serial, struct wl_surface *,
                           struct wl_array *) {
    printf("keyboard-enter %u\n", serial);
}

static void keyboard_leave(void *, struct wl_keyboard *, uint32_t serial, struct wl_surface *) {
    printf("keyboard-leave %u\n", serial);
}

static void keyboard_key(void *, struct wl_keyboard *, uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state) {
    printf("keyboard-key %u %u %u %u\n", serial, time, key, state);
}

static void keyboard_modifiers(void *, struct wl_keyboard *, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t) {}

static const struct wl_keyboard_listener keyboard_listener = {.keymap = keyboard_keymap,
                                                              .enter = keyboard_enter,
                                                              .leave = keyboard_leave,
                                                              .key = keyboard_key,
                                                              .modifiers = keyboard_modifiers,
                                                              .repeat_info = nullptr};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities) {
    auto *client = static_cast<struct client *>(data);
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !client->pointer) {
        client->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
        if (client->relative_manager) {
            client->relative = zwp_relative_pointer_manager_v1_get_relative_pointer(
                client->relative_manager, client->pointer);
            zwp_relative_pointer_v1_add_listener(client->relative, &relative_listener, client);
        }
        if (!client->defer_constraint) {
            request_constraint(client);
        }
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !client->keyboard) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    }
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_capabilities,
                                                      .name = nullptr};

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
    if (client->mode != pointer_mode::disabled) {
        printf("surface-commit %d %d\n", client->width, client->height);
    }
}

static void surface_configured(void *data, struct xdg_surface *surface, uint32_t serial) noexcept {
    auto *client = static_cast<struct client *>(data);
    xdg_surface_ack_configure(surface, serial);
    if (client->mode != pointer_mode::disabled) {
        printf("surface-configure %u\n", serial);
    }
    const bool initial = !client->configured;
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
    if (initial && !client->defer_constraint) {
        request_constraint(client);
    }
}

static void toplevel_configured(void *data, struct xdg_toplevel *toplevel, int32_t width,
                                int32_t height, struct wl_array *states) noexcept {
    (void)toplevel;
    auto *client = static_cast<struct client *>(data);
    if (client->mode != pointer_mode::disabled) {
        bool fullscreen = false;
        bool activated = false;
        const auto *begin = static_cast<const uint32_t *>(states->data);
        for (size_t i = 0; i < states->size / sizeof(*begin); ++i) {
            fullscreen |= begin[i] == XDG_TOPLEVEL_STATE_FULLSCREEN;
            activated |= begin[i] == XDG_TOPLEVEL_STATE_ACTIVATED;
        }
        printf("toplevel-configure %d %d fullscreen=%d activated=%d\n", width, height, fullscreen,
               activated);
    }
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
    } else if (client->mode != pointer_mode::disabled &&
               strcmp(interface, wl_seat_interface.name) == 0 && !client->seat) {
        client->seat =
            static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(client->seat, &seat_listener, client);
    } else if (client->mode != pointer_mode::disabled &&
               strcmp(interface, zwp_pointer_constraints_v1_interface.name) == 0) {
        client->constraints = static_cast<zwp_pointer_constraints_v1 *>(
            wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    } else if (client->mode != pointer_mode::disabled &&
               strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) == 0) {
        client->relative_manager = static_cast<zwp_relative_pointer_manager_v1 *>(
            wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
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

static bool parse_number(std::string_view &value, int32_t &number, char delimiter = '\0') {
    const auto end = value.data() + value.size();
    const auto parsed = std::from_chars(value.data(), end, number);
    if (parsed.ec != std::errc{}) {
        return false;
    }
    if (delimiter == '\0') {
        return parsed.ptr == end;
    }
    if (parsed.ptr == end || *parsed.ptr != delimiter) {
        return false;
    }
    value.remove_prefix(parsed.ptr - value.data() + 1);
    return true;
}

static bool parse_region(std::string_view value, probe_region &region) {
    probe_region parsed{};
    if (value == "null") {
        region = parsed;
        return true;
    }
    parsed.specified = true;
    if (value != "empty" &&
        (!parse_number(value, parsed.x, ',') || !parse_number(value, parsed.y, ',') ||
         !parse_number(value, parsed.width, ',') || !parse_number(value, parsed.height) ||
         parsed.width <= 0 || parsed.height <= 0)) {
        return false;
    }
    region = parsed;
    return true;
}

static bool parse_option(const char *word, struct client *client) noexcept {
    if (strcmp(word, "--fixed-size") == 0) {
        client->fixed_size = true;
        return true;
    }
    std::string_view option = word;
    if (option == "--fullscreen" || option == "--windowed") {
        client->fullscreen = option == "--fullscreen";
        return true;
    }
    if (option == "--oneshot") {
        client->oneshot = true;
        return true;
    }
    if (option == "--defer-constraint") {
        client->defer_constraint = true;
        return true;
    }
    if (option.starts_with("--size=")) {
        option.remove_prefix(sizeof("--size=") - 1);
        client->fixed_size = true;
        return parse_number(option, client->width, 'x') && parse_number(option, client->height) &&
               client->width > 0 && client->width <= 4096 && client->height > 0 &&
               client->height <= 4096;
    }
    if (option.starts_with("--region=")) {
        return parse_region(option.substr(sizeof("--region=") - 1), client->region);
    }
    if (option.starts_with("--input-region=")) {
        return parse_region(option.substr(sizeof("--input-region=") - 1), client->input_region);
    }
    if (option == "--pointer=none" || option == "--pointer=lock" || option == "--pointer=confine") {
        client->mode = option == "--pointer=none"   ? pointer_mode::none
                       : option == "--pointer=lock" ? pointer_mode::lock
                                                    : pointer_mode::confine;
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

static bool pointer_command(struct client *client, std::string_view command) {
    if (command == "request") {
        request_constraint(client);
    } else if (command == "release") {
        release_constraint(client);
    } else if (command == "fullscreen") {
        xdg_toplevel_set_fullscreen(client->toplevel, nullptr);
    } else if (command == "windowed") {
        xdg_toplevel_unset_fullscreen(client->toplevel);
    } else if (command == "commit") {
        wl_surface_commit(client->surface);
        puts("surface-commit state");
    } else if (command.starts_with("input-region ") &&
               parse_region(command.substr(sizeof("input-region ") - 1), client->input_region)) {
        set_input_region(client);
    } else if (command.starts_with("region ") &&
               parse_region(command.substr(sizeof("region ") - 1), client->region)) {
        struct wl_region *region = make_region(client, client->region);
        if (client->lock) {
            zwp_locked_pointer_v1_set_region(client->lock, region);
        }
        if (client->confine) {
            zwp_confined_pointer_v1_set_region(client->confine, region);
        }
        if (region) {
            wl_region_destroy(region);
        }
        print_region("pending-region", client->region);
    } else {
        return false;
    }
    return true;
}

static void read_command(struct client *client) noexcept {
    char line[128];
    if (!fgets(line, sizeof(line), stdin) || strcmp(line, "close\n") == 0) {
        client->stop = true;
    } else if (parse_pixel(line, &client->pixel)) {
        client->dirty = true;
        if (client->configured && !client->frame) {
            draw(client);
        }
    } else if (client->mode == pointer_mode::disabled ||
               !pointer_command(client, std::string_view(line).substr(0, strcspn(line, "\n")))) {
        fputs("Invalid fixture command\n", stderr);
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
    if (client->relative) {
        zwp_relative_pointer_v1_destroy(client->relative);
    }
    if (client->relative_manager) {
        zwp_relative_pointer_manager_v1_destroy(client->relative_manager);
    }
    if (client->constraints) {
        zwp_pointer_constraints_v1_destroy(client->constraints);
    }
    if (client->pointer) {
        wl_pointer_destroy(client->pointer);
    }
    if (client->keyboard) {
        wl_keyboard_destroy(client->keyboard);
    }
    if (client->seat) {
        wl_seat_destroy(client->seat);
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
    bool valid = argc >= 3 && parse_pixel(argv[2], &client.pixel);
    for (int i = 3; valid && i < argc; ++i) {
        valid = parse_option(argv[i], &client);
    }
    if (!valid) {
        fputs(
            "Usage: seaside-part7-native-client TITLE RRGGBB [--fixed-size|--delay-resize=MS]\n"
            "       [--size=WIDTHxHEIGHT] [--fullscreen|--windowed] [--pointer=none|lock|confine]\n"
            "       [--region=REGION] [--input-region=REGION] [--oneshot] [--defer-constraint]\n"
            "REGION: x,y,width,height, empty or null\n",
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
        !client.shell ||
        (client.mode != pointer_mode::disabled &&
         (!client.seat || !client.constraints || !client.relative_manager))) {
        destroy_client(&client, registry);
        return EXIT_FAILURE;
    }
    client.surface = wl_compositor_create_surface(client.compositor);
    struct xdg_surface *xdg =
        client.surface ? xdg_wm_base_get_xdg_surface(client.shell, client.surface) : NULL;
    struct xdg_toplevel *toplevel = xdg ? xdg_surface_get_toplevel(xdg) : nullptr;
    client.toplevel = toplevel;
    if (toplevel) {
        xdg_surface_add_listener(xdg, &surface_listener, &client);
        xdg_toplevel_add_listener(toplevel, &toplevel_listener, &client);
        xdg_toplevel_set_title(toplevel, argv[1]);
        xdg_toplevel_set_app_id(toplevel, "org.seaside.Part7.NativeFixture");
        if (client.fullscreen) {
            xdg_toplevel_set_fullscreen(toplevel, nullptr);
        }
        if (client.input_region.specified) {
            set_input_region(&client);
        }
        wl_surface_commit(client.surface);
        if (client.mode != pointer_mode::disabled) {
            puts("surface-commit initial");
        }
        run(&client);
        release_constraint(&client);
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

#include "scene.hpp"
#include "server.hpp"
#include "tty_drm.h"
#include "tty_input.h"
#include "tty_terminal.hpp"
#include "wlroots_bridge.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <linux/vt.h>
#include <memory>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int unavailable = 77;
constexpr auto start_timeout = 5s;
constexpr int check_interval_ms = 25;
constexpr auto display_duration = 15s;
constexpr auto terminal_duration = 60s;
constexpr std::string_view device_prefix = "/dev/dri/card";

bool card_path(std::string_view path) {
    if (!path.starts_with(device_prefix)) {
        return false;
    }
    path.remove_prefix(device_prefix.size());
    return !path.empty() &&
           std::ranges::all_of(path, [](char value) { return value >= '0' && value <= '9'; });
}

bool active_console() {
    std::array<char, 64> name{};
    if (ttyname_r(STDIN_FILENO, name.data(), name.size()) != 0) {
        return false;
    }
    constexpr std::string_view prefix = "/dev/tty";
    const std::string_view path(name.data());
    if (!path.starts_with(prefix)) {
        return false;
    }
    const auto number = path.substr(prefix.size());
    unsigned int vt = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), vt);
    vt_stat active{};
    return error == std::errc{} && end == number.data() + number.size() && vt > 0 &&
           tcgetpgrp(STDIN_FILENO) == getpgrp() && ioctl(STDIN_FILENO, VT_GETSTATE, &active) == 0 &&
           vt == active.v_active;
}

struct Fixture {
    seaside::Server &server;
    seaside::Scene &scene;
    seaside_tty_drm *drm;
    std::string_view socket;
    bool terminal_mode;
    seaside::test::TerminalChild terminal{};
    seaside_tty_input *input = nullptr;
    wl_event_source *timer = nullptr;
    Clock::time_point deadline = Clock::now() + start_timeout;
    const char *stop_reason = "signal";
    bool ready = false;
    bool failed = false;
    bool stopping = false;
    bool terminal_started = false;
    bool terminal_mapped = false;
    bool terminal_exited = false;

    static void move_pointer(void *data, bool absolute, double x, double y) noexcept {
        auto &fixture = *static_cast<Fixture *>(data);
        if (!fixture.stopping) {
            seaside_tty_drm_move(fixture.drm, absolute, x, y);
        }
    }

    static void exit_requested(void *data) noexcept {
        auto &fixture = *static_cast<Fixture *>(data);
        // Escape is a test-fixture exit gesture and remains separate from the
        // compositor's approved, configurable actions.
        if (fixture.ready && !fixture.stopping) {
            fixture.stop("escape");
        }
    }

    static void focus_terminal(void *data, wlr_surface *surface) noexcept {
        auto &fixture = *static_cast<Fixture *>(data);
        seaside_tty_input_set_focus(fixture.input, surface);
        fixture.terminal_mapped |= surface != nullptr;
    }

    static void launch_terminal(void *data) noexcept {
        auto &fixture = *static_cast<Fixture *>(data);
        if (!fixture.ready || fixture.stopping || fixture.terminal_started) {
            return;
        }
        auto child = fixture.terminal.launch(fixture.socket);
        if (!child) {
            std::fprintf(stderr, "TTY terminal: launch failed: %s\n", std::strerror(child.error()));
            fixture.stop("terminal-launch", true);
            return;
        }
        fixture.terminal_started = true;
        // Register the PID before returning to Wayland dispatch. A client may
        // already have connected, but its XDG requests cannot yet be dispatched.
        if (!fixture.scene.watch_client(*child, focus_terminal, &fixture)) {
            fixture.stop("terminal-focus-watch", true);
        }
    }

    bool poll_terminal() noexcept {
        if (!terminal_started) {
            return false;
        }
        const auto result = terminal.poll();
        if (!result) {
            scene.clear_client_watch();
            std::fprintf(stderr, "TTY terminal: child failed: %s\n", std::strerror(result.error()));
            stop("terminal-child", true);
            return true;
        }
        if (!*result) {
            return false;
        }
        scene.clear_client_watch();
        const int status = **result;
        terminal_exited = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        stop(terminal_exited ? "terminal-exited" : "terminal-exit-error", !terminal_exited);
        return true;
    }

    void stop(const char *reason, bool failure = false) noexcept {
        stopping = true;
        stop_reason = reason;
        failed = failure;
        server.terminate();
    }

    static int observe(void *data) noexcept;
};

struct Result {
    struct seaside_tty_drm_observation display;
    const char *stop_reason;
    bool passed;
    bool terminal_mapped;
    uint64_t forwarded_keys;
};

std::expected<Result, const char *> run_fixture(const char *device, bool terminal_mode) {
    // The DRM bridge's session is tied to the display loop. Keep its owner
    // alive across Server cleanup, which releases the backend before the loop.
    std::unique_ptr<seaside_tty_drm, decltype(&seaside_tty_drm_destroy)> drm(
        nullptr, seaside_tty_drm_destroy);
    auto created = seaside::Server::create();
    if (!created) {
        return std::unexpected("display");
    }
    auto &server = **created;
    if (!server.install_shutdown_signals()) {
        return std::unexpected("shutdown-signals");
    }
    auto scene = seaside::Scene::create(server.display());
    if (!scene) {
        return std::unexpected("scene");
    }
    // Avoid claiming a default wayland-N name while testing the console path.
    const auto socket = server.listen("seaside-tty-fixture");
    if (!socket) {
        return std::unexpected("wayland-socket");
    }
    auto *loop = wl_display_get_event_loop(server.display());
    wlr_backend *backend = nullptr;
    wlr_renderer *renderer = nullptr;
    drm.reset(seaside_tty_drm_create(server.display(), device, &backend, &renderer));
    if (!drm) {
        return std::unexpected("seat-drm-renderer");
    }
    // Input discovery may happen inside backend_start. Keep local ownership of
    // the pair until its listeners are attached and Server takes both handles.
    std::unique_ptr<wlr_renderer, decltype(&seaside_wlr_renderer_destroy)> pending_renderer(
        renderer, seaside_wlr_renderer_destroy);
    std::unique_ptr<wlr_backend, decltype(&seaside_wlr_backend_destroy)> pending_backend(
        backend, seaside_wlr_backend_destroy);
    if (server.shutdown_requested()) {
        return std::unexpected("shutdown-during-seat-setup");
    }
    Fixture fixture{server, **scene, drm.get(), *socket, terminal_mode};
    const seaside_tty_input_callbacks callbacks{
        .motion = Fixture::move_pointer,
        .exit_requested = terminal_mode ? nullptr : Fixture::exit_requested,
        .launch_requested = terminal_mode ? Fixture::launch_terminal : nullptr,
    };
    std::unique_ptr<seaside_tty_input, decltype(&seaside_tty_input_destroy)> input(
        seaside_tty_input_create(backend, &callbacks, &fixture), seaside_tty_input_destroy);
    if (!input) {
        return std::unexpected("input-listeners");
    }
    fixture.input = input.get();
    if (terminal_mode && !seaside_tty_input_enable_keyboard(input.get(), server.display())) {
        return std::unexpected("wayland-keyboard");
    }
    if (!server.start(pending_backend.release(), pending_renderer.release())) {
        return std::unexpected("backend-start");
    }
    auto *output = seaside_tty_drm_enable(drm.get(), server.allocator(), server.renderer());
    if (!output || !(*scene)->add_output(output)) {
        return std::unexpected("output-or-cursor-setup");
    }
    std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> timer(
        wl_event_loop_add_timer(loop, Fixture::observe, &fixture), wl_event_source_remove);
    fixture.timer = timer.get();
    fixture.deadline = Clock::now() + start_timeout;
    if (!timer || wl_event_source_timer_update(timer.get(), check_interval_ms) < 0) {
        return std::unexpected("observation-timer");
    }
    if (!server.run()) {
        return std::unexpected("dispatch");
    }

    const auto display = seaside_tty_drm_observation(drm.get());
    const auto devices = seaside_tty_input_get_snapshot(input.get());
    const auto scene_state = (*scene)->snapshot();
    (*scene)->clear_client_watch();
    // The watch invokes a callback into the input bridge. Clear it before
    // either owner is destroyed, and reap any child left after a failed trial.
    const bool child_released = fixture.terminal.stop();
    const bool interaction_passed = terminal_mode
                                        ? fixture.terminal_mapped && fixture.terminal_exited &&
                                              devices.forwarded_key_presses > 0
                                        : display.presented_moves > 0;
    const bool passed = fixture.ready && display.active && display.cursor_presented &&
                        !fixture.failed && !display.failed && !devices.failed &&
                        !scene_state.failed && interaction_passed && child_released;
    return Result{display, child_released ? fixture.stop_reason : "terminal-cleanup", passed,
                  fixture.terminal_mapped, devices.forwarded_key_presses};
}

int Fixture::observe(void *data) noexcept {
    auto &fixture = *static_cast<Fixture *>(data);
    if (fixture.stopping) {
        return 0;
    }
    const auto display = seaside_tty_drm_observation(fixture.drm);
    const auto devices = seaside_tty_input_get_snapshot(fixture.input);
    const auto scene = fixture.scene.snapshot();
    if (display.failed || devices.failed || scene.failed) {
        fixture.stop("output-cursor-or-input-failed", true);
        return 0;
    }
    const auto now = Clock::now();
    if (now >= fixture.deadline) {
        if (!fixture.ready) {
            fixture.stop("startup-timeout", true);
        } else if (fixture.terminal_mode) {
            fixture.stop("terminal-timeout", true);
        } else {
            fixture.stop("observation-ended");
        }
        return 0;
    }
    if (!fixture.ready && display.cursor_presented && devices.pointers > 0 &&
        (!fixture.terminal_mode || devices.keyboards > 0)) {
        fixture.ready = true;
        fixture.deadline = now + (fixture.terminal_mode ? terminal_duration : display_duration);
    }
    if (fixture.poll_terminal()) {
        return 0;
    }
    if (wl_event_source_timer_update(fixture.timer, check_interval_ms) < 0) {
        fixture.stop("observation-timer-failed", true);
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    const bool terminal_mode = argc >= 2 && std::string_view(argv[1]) == "--terminal";
    const int device_index = terminal_mode ? 2 : 1;
    const char *device = argc > device_index ? argv[device_index] : "/dev/dri/card0";
    if (argc > device_index + 1 || !card_path(device)) {
        std::fputs("Usage: seaside-tty-fixture [--terminal] [/dev/dri/cardN]\n", stderr);
        return 2;
    }
    if (!active_console()) {
        std::fputs("TTY fixture BLOCKED: run as a foreground job on the active Linux console\n",
                   stderr);
        return unavailable;
    }
    std::fputs(terminal_mode
                   ? "TTY terminal fixture: Super+Return opens foot. Type exit to finish.\n"
                   : "TTY fixture: move the pointer on the black screen. Escape exits early.\n",
               stderr);
    // Report after the backend and seat have been released, including failures
    // during setup when the console might already be in graphics mode.
    const auto result = run_fixture(device, terminal_mode);
    if (!result) {
        std::fprintf(stderr, "TTY fixture FAIL stage=%s\n", result.error());
        return EXIT_FAILURE;
    }
    const auto &display = result->display;
    if (terminal_mode) {
        std::fprintf(
            stderr, "TTY terminal %s stop=%s mapped=%s keys=%" PRIu64 " cursor-plane=%" PRIu32 "\n",
            result->passed ? "PASS" : "FAIL", result->stop_reason,
            result->terminal_mapped ? "yes" : "no", result->forwarded_keys,
            display.cursor_plane_id);
        return result->passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    std::fprintf(stderr,
                 "TTY fixture %s stop=%s connector=%" PRIu32 " cursor-plane=%" PRIu32
                 " presented-frames=%" PRIu64 " presented-moves=%" PRIu64 "/%" PRIu64 "\n",
                 result->passed ? "PASS" : "FAIL", result->stop_reason, display.connector_id,
                 display.cursor_plane_id, display.presented_frames, display.presented_moves,
                 display.cursor_moves);
    return result->passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

#include "scene.hpp"
#include "server.hpp"
#include "tty_drm.h"
#include "tty_input.h"
#include "wlroots_bridge.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <linux/vt.h>
#include <memory>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int unavailable = 77;
constexpr auto start_timeout = 5s;
constexpr int check_interval_ms = 25;
constexpr auto display_duration = 15s;
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
    seaside_tty_input *input = nullptr;
    wl_event_source *timer = nullptr;
    Clock::time_point deadline = Clock::now() + start_timeout;
    const char *stop_reason = "signal";
    bool ready = false;
    bool failed = false;
    bool stopping = false;

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
        if (fixture.ready) {
            fixture.stop("escape");
        }
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
};

std::expected<Result, const char *> run_fixture(const char *device) {
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
    if (!server.listen("seaside-tty-fixture")) {
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
    Fixture fixture{server, **scene, drm.get()};
    const seaside_tty_input_callbacks callbacks{Fixture::move_pointer, Fixture::exit_requested};
    std::unique_ptr<seaside_tty_input, decltype(&seaside_tty_input_destroy)> input(
        seaside_tty_input_create(backend, &callbacks, &fixture), seaside_tty_input_destroy);
    if (!input) {
        return std::unexpected("input-listeners");
    }
    fixture.input = input.get();
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
    const bool passed = fixture.ready && display.active && display.cursor_presented &&
                        !fixture.failed && !display.failed && !devices.failed &&
                        !scene_state.failed && display.presented_moves > 0;
    return Result{display, fixture.stop_reason, passed};
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
        fixture.stop(fixture.ready ? "observation-ended" : "startup-timeout", !fixture.ready);
        return 0;
    }
    if (!fixture.ready && display.cursor_presented && devices.pointers > 0) {
        fixture.ready = true;
        fixture.deadline = now + display_duration;
    }
    if (wl_event_source_timer_update(fixture.timer, check_interval_ms) < 0) {
        fixture.stop("observation-timer-failed", true);
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    const char *device = argc == 2 ? argv[1] : "/dev/dri/card0";
    if (argc > 2 || !card_path(device)) {
        std::fputs("Usage: seaside-tty-fixture [/dev/dri/cardN]\n", stderr);
        return 2;
    }
    if (!active_console()) {
        std::fputs("TTY fixture BLOCKED: run as a foreground job on the active Linux console\n",
                   stderr);
        return unavailable;
    }
    std::fprintf(stderr,
                 "TTY fixture: move the pointer on the black screen. Escape exits early.\n");
    // Report after the backend and seat have been released, including failures
    // during setup when the console might already be in graphics mode.
    const auto result = run_fixture(device);
    if (!result) {
        std::fprintf(stderr, "TTY fixture FAIL stage=%s\n", result.error());
        return EXIT_FAILURE;
    }
    const auto &display = result->display;
    std::fprintf(stderr,
                 "TTY fixture %s stop=%s connector=%" PRIu32 " cursor-plane=%" PRIu32
                 " presented-frames=%" PRIu64 " presented-moves=%" PRIu64 "/%" PRIu64 "\n",
                 result->passed ? "PASS" : "FAIL", result->stop_reason, display.connector_id,
                 display.cursor_plane_id, display.presented_frames, display.presented_moves,
                 display.cursor_moves);
    return result->passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

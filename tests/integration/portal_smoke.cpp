#include "output_capture.h"
#include "part3_bridge.h"
#include "scene.hpp"
#include "server.hpp"
#include "session_bus.hpp"
#include "session_portal.hpp"
#include "wlroots_bridge.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int start_received(int fd, uint32_t, void *data) {
    char byte = 0;
    *static_cast<bool *>(data) = read(fd, &byte, 1) == 1 && byte == '\n';
    return 0;
}

template <typename Done>
bool dispatch_until(wl_display *display, Clock::duration limit, Done done) {
    auto *loop = wl_display_get_event_loop(display);
    const auto deadline = Clock::now() + limit;
    while (!done() && Clock::now() < deadline) {
        wl_display_flush_clients(display);
        if (wl_event_loop_dispatch(loop, 25) < 0 && errno != EINTR) {
            return false;
        }
    }
    return done();
}

bool wait_for_handshake(wl_display *display, Clock::duration limit) {
    bool proceed = false;
    const std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> start(
        wl_event_loop_add_fd(wl_display_get_event_loop(display), STDIN_FILENO, WL_EVENT_READABLE,
                             start_received, &proceed),
        wl_event_source_remove);
    return start && dispatch_until(display, limit, [&] { return proceed; });
}
} // namespace

int main(int argc, char **argv) {
    const bool stream = argc == 3 && std::string_view(argv[2]) == "--stream";
    if (argc != 2 && !stream) {
        return EXIT_FAILURE;
    }
    auto server = seaside::Server::create();
    if (!server) {
        return EXIT_FAILURE;
    }
    auto *display = (*server)->display();
    auto scene = seaside::Scene::create(display);
    if (!scene) {
        return EXIT_FAILURE;
    }
    auto *backend = part3_headless_backend_create(display);
    auto *renderer = part3_pixman_renderer_create();
    // Valid arguments transfer ownership to Server before any startup stage.
    if (!backend || !renderer) {
        if (backend) {
            seaside_wlr_backend_destroy(backend);
        }
        if (renderer) {
            seaside_wlr_renderer_destroy(renderer);
        }
        return EXIT_FAILURE;
    }
    if (!(*server)->start(backend, renderer)) {
        return EXIT_FAILURE;
    }
    const std::unique_ptr<part3_output, decltype(&part3_output_destroy)> output(
        part3_output_create(display, backend, (*server)->allocator(), renderer),
        part3_output_destroy);
    auto *head = part3_output_handle(output.get());
    if (!head || !(*scene)->add_output(head) || !(*server)->listen("seaside-portal-smoke")) {
        return EXIT_FAILURE;
    }
    const std::unique_ptr<seaside_output_capture, decltype(&seaside_output_capture_destroy)>
        capture(seaside_output_capture_create(display), seaside_output_capture_destroy);
    if (!capture || !seaside_output_capture_add_output(capture.get(), head, 0, 0)) {
        return EXIT_FAILURE;
    }
    if (stream &&
        !(*scene)->enable_window_capture((*server)->allocator(), renderer,
                                         seaside_output_capture_copy_manager(capture.get()))) {
        return EXIT_FAILURE;
    }
    if (!wait_for_handshake(display, 20s)) {
        std::fputs("Portal fixture did not receive the startup handshake\n", stderr);
        return EXIT_FAILURE;
    }
    auto bus = seaside::SessionBus::connect(wl_display_get_event_loop(display), argv[1]);
    if (!bus) {
        return EXIT_FAILURE;
    }
    bool failed = false;
    unsigned reports = 0;
    auto probe = seaside::PortalProbe::start(
        **bus, getuid(),
        [&](pid_t pid, uid_t uid) {
            return seaside_output_capture_client_ready(capture.get(), pid, uid) &&
                   (!stream || (*scene)->window_capture_client_ready(pid, uid));
        },
        [&](seaside::StartupService service, seaside::ServiceReport result) {
            std::fprintf(stderr, "portal service=%d failure=%d native=%d\n",
                         static_cast<int>(service), static_cast<int>(result.failure),
                         result.native_error);
            failed |= result.failure != seaside::ServiceFailure::none;
            ++reports;
        },
        7s, {},
        stream ? seaside::PortalProbe::Sources::monitor_and_window
               : seaside::PortalProbe::Sources::monitor);
    if (probe && dispatch_until(display, 8s, [&] { return failed || reports == 2; }) && !failed) {
        std::puts("portal smoke PASS frontend and capture backend");
        std::fflush(stdout);
        return !stream || wait_for_handshake(display, 36s) ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}

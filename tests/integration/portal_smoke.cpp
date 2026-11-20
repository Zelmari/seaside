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
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
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
        capture(seaside_output_capture_create(display, head), seaside_output_capture_destroy);
    if (!capture) {
        return EXIT_FAILURE;
    }
    auto *loop = wl_display_get_event_loop(display);
    bool proceed = false;
    {
        const std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> start(
            wl_event_loop_add_fd(loop, STDIN_FILENO, WL_EVENT_READABLE, start_received, &proceed),
            wl_event_source_remove);
        if (!start || !dispatch_until(display, 12s, [&] { return proceed; })) {
            std::fputs("Portal fixture did not receive the startup handshake\n", stderr);
            return EXIT_FAILURE;
        }
    }
    auto bus = seaside::SessionBus::connect(loop, argv[1]);
    if (!bus) {
        return EXIT_FAILURE;
    }
    bool failed = false;
    unsigned reports = 0;
    auto probe = seaside::PortalProbe::start(
        **bus, getuid(),
        [&](pid_t pid, uid_t uid) {
            return seaside_output_capture_client_ready(capture.get(), pid, uid);
        },
        [&](seaside::StartupService service, seaside::ServiceReport result) {
            std::fprintf(stderr, "portal service=%d failure=%d native=%d\n",
                         static_cast<int>(service), static_cast<int>(result.failure),
                         result.native_error);
            failed |= result.failure != seaside::ServiceFailure::none;
            ++reports;
        },
        7s);
    if (probe && dispatch_until(display, 8s, [&] { return failed || reports == 2; }) && !failed) {
        std::puts("portal smoke PASS frontend and capture backend");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}

#include "part3_bridge.h"
#include "scene.hpp"
#include "server.hpp"
#include "wlroots_bridge.h"
#include "xdg-shell-client-protocol.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <latch>
#include <mutex>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>

namespace {
constexpr int client_width = 16;
constexpr int client_height = 8;
constexpr std::uint32_t client_pixel = 0xFF335577;

class RuntimeDirectory {
  public:
    RuntimeDirectory() {
        if (const auto *value = std::getenv("XDG_RUNTIME_DIR")) {
            previous_runtime_ = value;
        }
        const auto root = std::filesystem::canonical(previous_runtime_.value_or("/tmp"));
        path_ = (root / "seaside-part3-XXXXXX").string();
        if (!mkdtemp(path_.data())) {
            throw std::system_error(errno, std::generic_category(), "fixture runtime directory");
        }
        if (setenv("XDG_RUNTIME_DIR", path_.c_str(), 1) < 0) {
            const int error = errno;
            rmdir(path_.c_str());
            throw std::system_error(error, std::generic_category(), "fixture runtime environment");
        }
    }
    ~RuntimeDirectory() {
        if (previous_runtime_) {
            setenv("XDG_RUNTIME_DIR", previous_runtime_->c_str(), 1);
        } else {
            unsetenv("XDG_RUNTIME_DIR");
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            std::fprintf(stderr, "part3 runtime cleanup failed: %s\n", error.message().c_str());
        }
    }
    const char *path() const noexcept { return path_.c_str(); }

  private:
    std::string path_;
    std::optional<std::string> previous_runtime_;
};

struct Client {
    wl_display *display = nullptr;
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    xdg_surface *xdg = nullptr;
    unsigned configures = 0;
    unsigned frames = 0;
    unsigned activations = 0;
    bool expect_focus = false;
    bool invalid_configure = false;
};

void shell_ping(void *, xdg_wm_base *shell, std::uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}
const xdg_wm_base_listener shell_listener{.ping = shell_ping};

void registry_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface,
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
void registry_remove(void *, wl_registry *, std::uint32_t) {}
const wl_registry_listener registry_listener{.global = registry_global,
                                             .global_remove = registry_remove};

void surface_configure(void *data, xdg_surface *surface, std::uint32_t serial) {
    auto &client = *static_cast<Client *>(data);
    client.configures++;
    xdg_surface_ack_configure(surface, serial);
}
const xdg_surface_listener surface_listener{.configure = surface_configure};

void toplevel_configure(void *data, xdg_toplevel *, std::int32_t width, std::int32_t height,
                        wl_array *states) {
    auto &client = *static_cast<Client *>(data);
    const bool activated =
        states->size == sizeof(std::uint32_t) &&
        *static_cast<const std::uint32_t *>(states->data) == XDG_TOPLEVEL_STATE_ACTIVATED;
    if (activated) {
        ++client.activations;
    }
    client.invalid_configure = client.invalid_configure || width != 0 || height != 0 ||
                               (states->size != 0 && !(client.expect_focus && activated));
    if (client.invalid_configure) {
        std::fprintf(stderr, "part3 client configure width=%d height=%d state-bytes=%zu\n", width,
                     height, states->size);
    }
}
void toplevel_close(void *data, xdg_toplevel *) {
    static_cast<Client *>(data)->invalid_configure = true;
}
void toplevel_bounds(void *, xdg_toplevel *, std::int32_t, std::int32_t) {}
void toplevel_capabilities(void *data, xdg_toplevel *, wl_array *capabilities) {
    if (capabilities->size != 0) {
        static_cast<Client *>(data)->invalid_configure = true;
    }
}
const xdg_toplevel_listener toplevel_listener{
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_bounds,
    .wm_capabilities = toplevel_capabilities,
};

void frame_done(void *data, wl_callback *callback, std::uint32_t) {
    static_cast<Client *>(data)->frames++;
    wl_callback_destroy(callback);
}
const wl_callback_listener frame_listener{.done = frame_done};

template <class Predicate> bool wait_for(Client &client, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        while (wl_display_prepare_read(client.display) != 0) {
            if (wl_display_dispatch_pending(client.display) < 0) {
                return false;
            }
        }
        if (ready()) {
            wl_display_cancel_read(client.display);
            return true;
        }
        const bool writable = wl_display_flush(client.display) < 0 && errno == EAGAIN;
        if (wl_display_get_error(client.display) != 0) {
            wl_display_cancel_read(client.display);
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            wl_display_cancel_read(client.display);
            return false;
        }
        pollfd descriptor{.fd = wl_display_get_fd(client.display),
                          .events = static_cast<short>(POLLIN | (writable ? POLLOUT : 0)),
                          .revents = 0};
        const int status = poll(&descriptor, 1, static_cast<int>(remaining));
        if (status <= 0 || !(descriptor.revents & POLLIN)) {
            wl_display_cancel_read(client.display);
            if (status < 0 && errno == EINTR) {
                continue;
            }
            if (status > 0 && (descriptor.revents & POLLOUT) &&
                !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                continue;
            }
            return false;
        }
        if (wl_display_read_events(client.display) < 0 ||
            wl_display_dispatch_pending(client.display) < 0) {
            return false;
        }
    }
    return true;
}

bool exercise_client(const char *socket, bool abrupt, bool hold, int completion_fd) {
    if (unsetenv("WAYLAND_SOCKET") < 0) {
        return false;
    }
    Client client{.expect_focus = !abrupt || hold};
    client.display = wl_display_connect(socket);
    if (!client.display) {
        return false;
    }
    auto *registry = wl_display_get_registry(client.display);
    wl_registry_add_listener(registry, &registry_listener, &client);
    if (wl_display_roundtrip(client.display) < 0 || !client.compositor || !client.shm ||
        !client.shell) {
        wl_display_disconnect(client.display);
        return false;
    }
    auto *surface = wl_compositor_create_surface(client.compositor);
    client.xdg = xdg_wm_base_get_xdg_surface(client.shell, surface);
    xdg_surface_add_listener(client.xdg, &surface_listener, &client);
    auto *toplevel = xdg_surface_get_toplevel(client.xdg);
    xdg_toplevel_add_listener(toplevel, &toplevel_listener, &client);
    xdg_toplevel_set_app_id(toplevel, "seaside.part3.fixture");
    wl_surface_commit(surface);
    bool passed = wait_for(client, [&] { return client.configures >= 1; });
    constexpr std::size_t length = client_width * client_height * sizeof(std::uint32_t);
    int fd = memfd_create("seaside-part3-client", MFD_CLOEXEC);
    void *memory = MAP_FAILED;
    wl_buffer *buffer = nullptr;
    if (fd < 0 || ftruncate(fd, length) < 0) {
        passed = false;
    } else {
        memory = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) {
            passed = false;
        } else {
            auto *pixels = static_cast<std::uint32_t *>(memory);
            for (std::size_t i = 0; i < length / sizeof(*pixels); i++) {
                pixels[i] = client_pixel;
            }
            auto *pool = wl_shm_create_pool(client.shm, fd, length);
            buffer = wl_shm_pool_create_buffer(pool, 0, client_width, client_height,
                                               client_width * sizeof(std::uint32_t),
                                               WL_SHM_FORMAT_ARGB8888);
            wl_shm_pool_destroy(pool);
        }
    }
    const unsigned rounds = abrupt ? 1 : 2;
    for (unsigned i = 0; passed && i < rounds; i++) {
        const unsigned activations_before_map = client.activations;
        auto *frame = wl_surface_frame(surface);
        wl_callback_add_listener(frame, &frame_listener, &client);
        wl_surface_attach(surface, buffer, 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, client_width, client_height);
        wl_surface_commit(surface);
        passed = wait_for(client, [&] {
            return client.frames >= i + 1 &&
                   (!client.expect_focus || client.activations > activations_before_map);
        });
        if (abrupt) {
            break;
        }
        const unsigned configured = client.configures;
        xdg_toplevel_set_maximized(toplevel);
        xdg_toplevel_set_fullscreen(toplevel, nullptr);
        passed = passed && wait_for(client, [&] { return client.configures > configured; });
        wl_surface_attach(surface, nullptr, 0, 0);
        wl_surface_commit(surface);
        passed = passed && wl_display_roundtrip(client.display) >= 0;
        if (i + 1 < rounds) {
            const unsigned prior = client.configures;
            wl_surface_commit(surface);
            passed = passed && wait_for(client, [&] { return client.configures > prior; });
        }
    }
    passed =
        passed && !client.invalid_configure && (client.activations != 0) == client.expect_focus;
    if (hold) {
        const unsigned char result = passed;
        ssize_t count;
        do {
            count = write(completion_fd, &result, 1);
        } while (count < 0 && errno == EINTR);
        passed = passed && count == 1;
        if (passed) {
            // Keep the server-side surface mapped until its owner shuts down.
            // A bounded read witnesses the real socket closure, not a fixture
            // callback pretending that client teardown occurred.
            (void)wait_for(client, [] { return false; });
            const int error = wl_display_get_error(client.display);
            passed = error == EPIPE || error == ECONNRESET;
        }
    }
    if (!abrupt) {
        xdg_toplevel_destroy(toplevel);
        xdg_surface_destroy(client.xdg);
        wl_surface_destroy(surface);
        passed = passed && wl_display_roundtrip(client.display) >= 0;
        if (buffer) {
            wl_buffer_destroy(buffer);
        }
        xdg_wm_base_destroy(client.shell);
        wl_shm_destroy(client.shm);
        wl_compositor_destroy(client.compositor);
        wl_registry_destroy(registry);
    } else {
        // Free client bookkeeping without sending any protocol destructors.
        // The server still owns live objects until the socket EOF, while LSan
        // can continue checking the fixture instead of suppressing real leaks.
        for (auto *proxy :
             {reinterpret_cast<wl_proxy *>(toplevel), reinterpret_cast<wl_proxy *>(client.xdg),
              reinterpret_cast<wl_proxy *>(surface), reinterpret_cast<wl_proxy *>(buffer),
              reinterpret_cast<wl_proxy *>(client.shell), reinterpret_cast<wl_proxy *>(client.shm),
              reinterpret_cast<wl_proxy *>(client.compositor),
              reinterpret_cast<wl_proxy *>(registry)}) {
            if (proxy) {
                wl_proxy_destroy(proxy);
            }
        }
    }
    // The abrupt path intentionally leaves every protocol object alive. A real
    // disconnection must retire server-side views without client cooperation.
    wl_display_disconnect(client.display);
    if (memory != MAP_FAILED) {
        munmap(memory, length);
    }
    if (fd >= 0) {
        close(fd);
    }
    return passed;
}

struct RunState {
    seaside::Server *server;
    bool client_passed = false;
    bool expired = false;
    int signal;
    wl_event_source *done = nullptr;
};

struct FocusObservation {
    wlr_surface *surface = nullptr;
    unsigned focused = 0;
    unsigned cleared = 0;
    bool valid = true;
};

void focus_changed(void *data, wlr_surface *surface) {
    auto &observation = *static_cast<FocusObservation *>(data);
    observation.valid &= (observation.surface == nullptr) != (surface == nullptr);
    observation.surface = surface;
    if (surface) {
        ++observation.focused;
    } else {
        ++observation.cleared;
    }
}

int deadline(void *data) {
    auto &state = *static_cast<RunState *>(data);
    state.expired = true;
    state.server->terminate();
    return 0;
}

int child_done(int fd, std::uint32_t, void *data) {
    auto &state = *static_cast<RunState *>(data);
    unsigned char result = 0;
    const auto count = read(fd, &result, 1);
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
        return 0;
    }
    state.client_passed = count == 1 && result == 1;
    wl_event_source_remove(state.done);
    state.done = nullptr;
    if (kill(getpid(), state.signal) < 0) {
        state.server->terminate();
    }
    return 0;
}

bool run_round(bool abrupt, int stop_signal, bool hold = false) {
    RuntimeDirectory directory;
    FocusObservation focus;
    const std::filesystem::path runtime(directory.path());
    auto created = seaside::Server::create();
    if (!created) {
        std::filesystem::remove(runtime);
        return false;
    }
    auto server = std::move(*created);
    auto scene_created = seaside::Scene::create(server->display());
    if (!scene_created) {
        return false;
    }
    auto scene = std::move(*scene_created);
    auto *backend = part3_headless_backend_create(server->display());
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
    if (!server->start(backend, renderer)) {
        return false;
    }
    std::unique_ptr<part3_output, decltype(&part3_output_destroy)> output(
        part3_output_create(server->display(), backend, server->allocator(), renderer),
        part3_output_destroy);
    auto socket = server->listen();
    if (!output || !socket || !scene->add_output(part3_output_handle(output.get())) ||
        scene->add_output(part3_output_handle(output.get())) || server->listen() ||
        !server->install_shutdown_signals()) {
        return false;
    }
    const std::string socket_path = (runtime / *socket).string();
    auto *loop = wl_display_get_event_loop(server->display());
    const auto first_frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!part3_output_get_snapshot(output.get()).black_frames &&
           std::chrono::steady_clock::now() < first_frame_deadline) {
        wl_event_loop_dispatch(loop, 10);
    }
    int pipe[2];
    if (pipe2(pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
        return false;
    }
    pid_t child = fork();
    if (child == 0) {
        close(pipe[0]);
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, SIGINT);
        sigaddset(&unblocked, SIGTERM);
        sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
        if (fcntl(pipe[1], F_SETFD, 0) < 0) {
            _exit(2);
        }
        char descriptor[32];
        std::snprintf(descriptor, sizeof(descriptor), "%d", pipe[1]);
        execl("/proc/self/exe", "seaside-part3-fixture", "--client", socket->data(), descriptor,
              hold     ? "hold"
              : abrupt ? "abrupt"
                       : "orderly",
              static_cast<char *>(nullptr));
        _exit(2);
    }
    close(pipe[1]);
    // Register before dispatch can accept the child connection. The abrupt
    // round deliberately uses the wrong PID for a client with the same UID.
    const bool expect_focus = !abrupt || hold;
    const bool watched =
        child > 0 && scene->watch_client(expect_focus ? child : getpid(), focus_changed, &focus);
    // Mask installation precedes every helper thread. Keeping one alive while
    // the process receives SIGTERM/SIGINT exercises the signalfd delivery
    // contract that a single-threaded shutdown test cannot establish.
    std::latch worker_ready(1);
    std::atomic<bool> worker_masked{false};
    std::atomic<bool> worker_alive{false};
    std::mutex worker_mutex;
    std::condition_variable_any worker_condition;
    std::jthread helper([&](std::stop_token stop) {
        sigset_t mask;
        const bool queried = pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0;
        worker_masked =
            queried && sigismember(&mask, SIGTERM) == 1 && sigismember(&mask, SIGINT) == 1;
        worker_alive = true;
        worker_ready.count_down();
        std::unique_lock lock(worker_mutex);
        worker_condition.wait(lock, stop, [] { return false; });
        worker_alive = false;
    });
    worker_ready.wait();
    RunState state{.server = server.get(), .signal = stop_signal};
    state.done = wl_event_loop_add_fd(loop, pipe[0], WL_EVENT_READABLE, child_done, &state);
    auto *timer = wl_event_loop_add_timer(loop, deadline, &state);
    bool passed = watched && state.done && timer && wl_event_source_timer_update(timer, 15000) >= 0;
    if (passed) {
        passed = server->run().has_value() && state.client_passed && !state.expired;
    }
    passed = passed && worker_masked && worker_alive;
    helper.request_stop();
    helper.join();
    if (timer) {
        wl_event_source_remove(timer);
    }
    if (state.done) {
        wl_event_source_remove(state.done);
    }
    close(pipe[0]);
    if (hold) {
        passed = passed && scene->snapshot().mapped_toplevels == 1 && focus.surface;
        server->finish();
    }
    scene->clear_client_watch();
    if (child > 0) {
        if (!passed) {
            kill(child, SIGKILL);
        }
        int status = 0;
        pid_t waited;
        do {
            waited = waitpid(child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        passed = passed && waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    // Client disconnect and pipe completion can be delivered in either order.
    // Drain the actual Wayland EOF before asserting retirement, without sleeps.
    const auto retire_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!hold && scene->snapshot().live_toplevels &&
           std::chrono::steady_clock::now() < retire_deadline) {
        wl_event_loop_dispatch(loop, 10);
    }
    const auto snapshot = scene->snapshot();
    const auto pixels = part3_output_get_snapshot(output.get());
    const unsigned expected = abrupt ? 1 : 2;
    const unsigned expected_focus_events = expect_focus ? expected : 0;
    passed = passed && focus.valid && focus.surface == nullptr &&
             focus.focused == expected_focus_events && focus.cleared == expected_focus_events;
    passed = passed && snapshot.live_toplevels == 0 && snapshot.mapped_toplevels == 0 &&
             snapshot.map_events == expected && snapshot.unmap_events == expected &&
             snapshot.destroyed_toplevels == 1 && snapshot.buffer_commits == expected &&
             snapshot.initial_configures == expected && !snapshot.failed &&
             pixels.black_frames > 0 && pixels.client_frames >= expected &&
             pixels.invalid_frames == 0;
    if (!passed) {
        std::fprintf(stderr,
                     "part3 fixture failed: abrupt=%d child=%d expired=%d live=%zu maps=%llu "
                     "unmaps=%llu configures=%llu commits=%llu black=%u client=%u invalid=%u "
                     "focus=%u clear=%u focus-valid=%d\n",
                     abrupt, state.client_passed, state.expired, snapshot.live_toplevels,
                     static_cast<unsigned long long>(snapshot.map_events),
                     static_cast<unsigned long long>(snapshot.unmap_events),
                     static_cast<unsigned long long>(snapshot.initial_configures),
                     static_cast<unsigned long long>(snapshot.buffer_commits), pixels.black_frames,
                     pixels.client_frames, pixels.invalid_frames, focus.focused, focus.cleared,
                     focus.valid);
    }
    output.reset();
    passed = passed && scene->snapshot().outputs == 0;
    // Exercise both owner teardown orders. The display-destroy listener must
    // retire scene state even if its C++ owner survives Server::finish().
    if (abrupt) {
        server->finish();
        scene.reset();
    } else {
        scene.reset();
        server->finish();
    }
    server->finish();
    passed = passed && !std::filesystem::exists(socket_path) &&
             !std::filesystem::exists(socket_path + ".lock");
    return std::filesystem::remove(runtime) && passed;
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::strcmp(argv[1], "--client") == 0) {
        const int descriptor = std::atoi(argv[3]);
        const bool hold = std::strcmp(argv[4], "hold") == 0;
        const unsigned char result =
            exercise_client(argv[2], hold || std::strcmp(argv[4], "abrupt") == 0, hold, descriptor);
        ssize_t count = 1;
        if (!hold) {
            do {
                count = write(descriptor, &result, 1);
            } while (count < 0 && errno == EINTR);
        }
        close(descriptor);
        return result == 1 && count == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (argc != 1) {
        return EXIT_FAILURE;
    }
    try {
        const bool passed =
            run_round(false, SIGTERM) && run_round(true, SIGINT) && run_round(true, SIGTERM, true);
        std::printf("{\"event\":\"part3-headless\",\"status\":\"%s\",\"rounds\":%d}\n",
                    passed ? "pass" : "fail", passed ? 3 : 0);
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "part3 fixture: %s\n", error.what());
        return EXIT_FAILURE;
    }
}

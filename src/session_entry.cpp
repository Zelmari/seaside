#include "config_transaction.hpp"
#include "desktop.hpp"
#include "output_capture.h"
#include "scene.hpp"
#include "server.hpp"
#include "session_activation.hpp"
#include "session_portal.hpp"
#include "session_services.hpp"
#include "session_systemd.hpp"
#include "tty_drm.h"
#include "tty_input.h"
#include "wlroots_bridge.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon-keysyms.h>

extern char **environ;

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using seaside::ServiceFailure;
using seaside::ServiceReport;
using seaside::StartupService;

constexpr auto appearance_timeout = 5s;
constexpr auto preflight_timeout = 10s;

class Commands {
  public:
    explicit Commands(std::string_view socket) {
        for (auto **entry = environ; *entry; ++entry) {
            const std::string_view value(*entry);
            if (!value.starts_with("WAYLAND_SOCKET=") && !value.starts_with("WAYLAND_DISPLAY=") &&
                !value.starts_with("DISPLAY=") && !value.starts_with("XDG_CURRENT_DESKTOP=") &&
                !value.starts_with("XDG_SESSION_TYPE=") &&
                !value.starts_with("DBUS_SESSION_BUS_ADDRESS=")) {
                values_.emplace_back(value);
            }
        }
        values_.push_back("WAYLAND_DISPLAY=" + std::string(socket));
        values_.emplace_back("XDG_CURRENT_DESKTOP=Seaside");
        values_.emplace_back("XDG_SESSION_TYPE=wayland");
        values_.push_back("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" +
                          std::to_string(getuid()) + "/bus");
        for (auto &value : values_) {
            environment_.push_back(value.data());
        }
        environment_.push_back(nullptr);
    }

    std::expected<pid_t, int> launch(const std::string &command) {
        // Reserve the reaper slot before a child can start. Launching the
        // default terminal with exec keeps its lifetime tied to that slot.
        children_.push_back(0);
        char *const argv[]{const_cast<char *>("sh"), const_cast<char *>("-c"),
                           const_cast<char *>(command == "foot" ? "exec foot" : command.c_str()),
                           nullptr};
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attr;
        const int attr_status = posix_spawnattr_init(&attr);
        const int action_status = posix_spawn_file_actions_init(&actions);
        int error = attr_status ? attr_status : action_status;
        if (!error) {
            sigset_t reset, unblocked;
            sigemptyset(&reset);
            sigemptyset(&unblocked);
            for (const int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
                sigaddset(&reset, signal);
            }
            error = posix_spawnattr_setsigdefault(&attr, &reset);
            if (!error) {
                error = posix_spawnattr_setsigmask(&attr, &unblocked);
            }
            if (!error) {
                error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF |
                                                            POSIX_SPAWN_SETSIGMASK);
            }
            if (!error) {
                error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                                         O_RDONLY | O_NOCTTY, 0);
            }
            if (!error) {
                // The client receives the Wayland socket name, not the
                // compositor's DRM, input, or service descriptors.
                error = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
            }
            if (!error) {
                error = posix_spawn(&children_.back(), "/bin/sh", &actions, &attr, argv,
                                    environment_.data());
            }
        }
        if (!action_status) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if (!attr_status) {
            posix_spawnattr_destroy(&attr);
        }
        if (error) {
            children_.pop_back();
            return std::unexpected(error);
        }
        return children_.back();
    }

    void reap() noexcept {
        for (auto child = children_.begin(); child != children_.end();) {
            int status = 0;
            const auto reaped = waitpid(*child, &status, WNOHANG);
            if (reaped == *child || (reaped < 0 && errno == ECHILD)) {
                if (reaped == *child && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
                    std::fprintf(stderr, "seaside: command pid=%d wait-status=%d\n", reaped,
                                 status);
                }
                child = children_.erase(child);
            } else {
                ++child;
            }
        }
    }

  private:
    std::vector<std::string> values_;
    std::vector<char *> environment_;
    std::vector<pid_t> children_;
};

struct Outcome {
    const char *failure_stage = nullptr;
    std::optional<seaside::StartupResult> preflight_failure;
    bool cleanup_failed = false;
};

class Session {
  public:
    Session(seaside::Server &server, seaside::Scene &scene, seaside_tty_drm *drm,
            std::string_view socket, seaside::LoadedConfig config, seaside::ConfigStore &store,
            const seaside::ConfigPaths &paths)
        : server_(server), scene_(scene), drm_(drm), socket_(socket),
          loop_(wl_display_get_event_loop(server.display())), config_(std::move(config)),
          store_(store), paths_(paths), commands_(socket), desktop_(scene, config_.config) {}

    ~Session() noexcept { seaside_tty_drm_set_output_callbacks(drm_, nullptr, nullptr); }

    bool observe(seaside_tty_input *input) noexcept {
        input_ = input;
        desktop_.set_input(input);
        const seaside_tty_drm_output_callbacks callbacks{
            .added = output_added,
            .removed = output_removed,
        };
        seaside_tty_drm_set_output_callbacks(drm_, &callbacks, this);
        timer_.reset(wl_event_loop_add_timer(loop_, on_tick, this));
        appearance_deadline_ = Clock::now() + appearance_timeout;
        return timer_ && wl_event_source_timer_update(timer_.get(), 1) == 0;
    }

    Outcome shut_down() noexcept {
        stopping_ = true;
        timer_.reset();
        portal_.reset();
        systemd_.reset();
        pipewire_.reset();
        gate_.reset();
        if (activation_) {
            outcome_.cleanup_failed = !activation_->stop();
            activation_.reset();
        }
        seaside_tty_drm_set_output_callbacks(drm_, nullptr, nullptr);
        desktop_.set_input(nullptr);
        commands_.reap();
        config_io_.reset();
        workers_.reset();
        if (!ready_ && !outcome_.failure_stage && !outcome_.preflight_failure) {
            outcome_.failure_stage = "interrupted-before-ready";
        }
        return outcome_;
    }

    static void pointer_motion(void *data, bool absolute, double x, double y) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_) {
            seaside_tty_drm_move(session.drm_, absolute, x, y);
            if (seaside_tty_drm_cursor_position(session.drm_, &x, &y)) {
                session.desktop_.motion(x, y);
            }
        }
    }

    static bool pointer_target(void *data, seaside_tty_pointer_target *target) noexcept {
        auto &session = *static_cast<Session *>(data);
        double x, y;
        seaside_scene_target hit{.surface = target->surface, .id = 0, .sx = 0, .sy = 0};
        if (session.stopping_ || !seaside_tty_drm_cursor_position(session.drm_, &x, &y) ||
            !session.scene_.window_at(x, y, hit)) {
            return false;
        }
        *target = {hit.surface, hit.sx, hit.sy, hit.id};
        return true;
    }

    static bool pointer_button(void *data, std::uint64_t window, std::uint32_t button, bool pressed,
                               std::uint8_t mods) noexcept {
        auto &session = *static_cast<Session *>(data);
        double x, y;
        return session.ready_ && !session.stopping_ &&
               seaside_tty_drm_cursor_position(session.drm_, &x, &y) &&
               session.desktop_.button(window, button, pressed, mods, x, y);
    }

    static bool pointer_absolute_delta(void *data, double x, double y, double *dx,
                                       double *dy) noexcept {
        auto &session = *static_cast<Session *>(data);
        return seaside_tty_drm_absolute_delta(session.drm_, x, y, dx, dy);
    }

    static void pointer_locked(void *data, bool locked) noexcept {
        auto &session = *static_cast<Session *>(data);
        seaside_tty_drm_set_cursor_visible(session.drm_, !locked);
    }

    static void modifiers_changed(void *data, std::uint8_t modifiers) noexcept {
        auto &session = *static_cast<Session *>(data);
        session.desktop_.modifiers_changed(modifiers);
    }

    static bool shortcut_pressed(void *data, uint8_t mods, uint32_t keysym) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (session.stopping_) {
            return false;
        }
        if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
            if (!seaside_tty_drm_change_vt(session.drm_, keysym - XKB_KEY_XF86Switch_VT_1 + 1)) {
                std::fprintf(stderr, "seaside: VT switch rejected\n");
            }
            return true;
        }
        if (!session.ready_) {
            return false;
        }
        const auto &bindings = session.config_.config.keybinds;
        const auto action = bindings.find({mods, seaside::ChordKind::keyboard, keysym});
        if (action == bindings.end()) {
            return false;
        }
        switch (action->second.kind) {
        case seaside::ActionKind::exit:
            session.desktop_.finish_focus_cycle();
            session.server_.terminate();
            return true;
        case seaside::ActionKind::exec:
            session.desktop_.finish_focus_cycle();
            session.launch(action->second.text);
            return true;
        default:
            return session.desktop_.action(action->second, mods);
        }
    }

  private:
    void fail(const char *stage) noexcept {
        stopping_ = true;
        outcome_.failure_stage = stage;
        server_.terminate();
    }

    void launch(const std::string &command) noexcept {
        try {
            const auto pid = commands_.launch(command);
            if (!pid) {
                std::fprintf(stderr, "seaside: command launch failed: %s\n",
                             std::strerror(pid.error()));
                return;
            }
        } catch (...) {
            std::fputs("seaside: couldn't prepare command launch\n", stderr);
        }
    }

    void persist_config() noexcept {
        if (config_.source != seaside::ConfigSource::user) {
            return;
        }
        try {
            auto queue = seaside::WorkQueue::create(loop_, {.workers = 1});
            if (queue) {
                workers_ = std::move(*queue);
                config_io_.emplace(*workers_, store_);
                auto saved = config_io_->persist(
                    paths_.recovery, std::move(config_.text), 0,
                    [](std::optional<seaside::StorageError> error) {
                        if (error) {
                            std::fprintf(stderr, "seaside: recovery file write failed: %s\n",
                                         error->error.message().c_str());
                        }
                    });
                if (saved) {
                    return;
                }
            }
        } catch (...) {
        }
        std::fputs("seaside: couldn't queue recovery file write\n", stderr);
    }

    void startup_result(seaside::StartupResult result) noexcept {
        if (result.phase != seaside::StartupPhase::ready) {
            stopping_ = true;
            outcome_.preflight_failure = result;
            server_.terminate();
            return;
        }
        preflight_passed_ = true;
    }

    void mark_ready(uint32_t cursor_plane) noexcept {
        ready_ = true;
        std::fprintf(stderr, "seaside READY cursor-plane=%" PRIu32 "\n", cursor_plane);
        persist_config();
        for (const auto &command : config_.config.autostart.commands) {
            launch(command);
        }
    }

    void report(StartupService service, ServiceReport detail) noexcept {
        if (stopping_) {
            return;
        }
        const auto delivered = gate_->report(service, detail);
        if (!delivered && delivered.error() != seaside::StartupError::stopped) {
            fail("preflight-report");
        }
    }

    void report_external_fault(StartupService service, ServiceReport detail) noexcept {
        if (stopping_) {
            return;
        }
        if (!ready_) {
            // Another mandatory check may still be pending after this
            // service first replied. Losing it now invalidates preflight.
            stopping_ = true;
            outcome_.preflight_failure =
                seaside::StartupResult{seaside::StartupPhase::failed, service, detail};
            server_.terminate();
            return;
        }
        std::fprintf(stderr, "seaside service fault service=%s reason=%s(%d)\n",
                     seaside::startup_service_name(service),
                     seaside::service_result_name(detail.failure), detail.native_error);
    }

    std::chrono::milliseconds remaining() const noexcept {
        return std::chrono::ceil<std::chrono::milliseconds>(preflight_deadline_ - Clock::now());
    }

    void start_portal_probe() noexcept {
        const auto timeout = remaining();
        if (timeout <= 0ms) {
            report(StartupService::portal_frontend, {ServiceFailure::timeout, -ETIMEDOUT});
            return;
        }
        auto portal = seaside::PortalProbe::start(
            *bus_, getuid(),
            [this](pid_t pid, uid_t uid) {
                return seaside_output_capture_client_ready(capture_.get(), pid, uid);
            },
            [this](StartupService service, ServiceReport detail) { report(service, detail); },
            timeout,
            [this](StartupService service, ServiceReport detail) {
                report_external_fault(service, detail);
            });
        if (!portal) {
            report(StartupService::portal_frontend, portal.error());
        } else {
            portal_ = std::move(*portal);
        }
    }

    void start_activation() noexcept {
        const auto timeout = remaining();
        if (timeout <= 0ms) {
            report(StartupService::capture_backend, {ServiceFailure::timeout, -ETIMEDOUT});
            return;
        }
        auto activation = seaside::PortalActivation::start(
            *bus_, socket_,
            [this](StartupService service, ServiceReport detail) {
                if (detail.failure == ServiceFailure::none && !stopping_ &&
                    gate_->result().phase == seaside::StartupPhase::checking) {
                    start_portal_probe();
                } else if (detail.failure != ServiceFailure::none) {
                    report(service, detail);
                }
            },
            timeout);
        if (!activation) {
            report(StartupService::capture_backend, activation.error());
        } else {
            activation_ = std::move(*activation);
        }
    }

    void begin_preflight() noexcept {
        preflight_deadline_ = Clock::now() + preflight_timeout;
        auto gate = seaside::StartupGate::create(
            loop_, [this](seaside::StartupResult result) { startup_result(result); },
            preflight_timeout);
        if (!gate) {
            fail("preflight-gate");
            return;
        }
        gate_ = std::move(*gate);
        auto bus = seaside::open_login_bus(loop_, getuid());
        if (!bus) {
            report(StartupService::systemd_user, bus.error());
            return;
        }
        bus_ = std::move(*bus);
        auto systemd = seaside::SystemdUserProbe::start(
            *bus_, getuid(),
            [this](ServiceReport detail) {
                report(StartupService::systemd_user, detail);
                if (detail.failure == ServiceFailure::none && !stopping_ &&
                    gate_->result().phase == seaside::StartupPhase::checking) {
                    start_activation();
                }
            },
            preflight_timeout);
        if (!systemd) {
            report(StartupService::systemd_user, systemd.error());
            return;
        }
        systemd_ = std::move(*systemd);
        auto pipewire = seaside::PipeWireProbe::create(
            loop_, {.timeout = preflight_timeout},
            [this](seaside::PipeWireResult result) {
                report(StartupService::pipewire, seaside::pipewire_service_report(result));
            },
            [this](int error) {
                report_external_fault(
                    StartupService::pipewire,
                    seaside::pipewire_service_report({seaside::PipeWireStatus::failed, error}));
            });
        if (!pipewire) {
            report(StartupService::pipewire, seaside::pipewire_service_report(pipewire.error()));
        } else {
            pipewire_ = std::move(*pipewire);
        }
    }

    bool attach_capture(wlr_output *output) noexcept {
        if (capture_) {
            if (!seaside_output_capture_set_output(capture_.get(), output)) {
                return false;
            }
        } else {
            capture_.reset(seaside_output_capture_create(server_.display(), output));
            if (!capture_) {
                return false;
            }
        }
        const auto bounds = desktop_.output_bounds(output);
        if (!seaside_output_capture_position(capture_.get(), bounds.x, bounds.y)) {
            seaside_output_capture_clear_output(capture_.get());
            return false;
        }
        capture_output_ = output;
        return true;
    }

    static bool output_added(void *data, wlr_output *output, const char *connector,
                             const char *identity, int x, int y, int width, int height) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.desktop_.add_output(output, connector, identity ? identity : "",
                                         {x, y, width, height})) {
            return false;
        }
        if (!session.capture_output_ && !session.attach_capture(output)) {
            session.desktop_.remove_output(output);
            return false;
        }
        return true;
    }

    static void output_removed(void *data, wlr_output *output, const char *) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (session.capture_output_ == output) {
            seaside_output_capture_clear_output(session.capture_.get());
            session.capture_output_ = nullptr;
        }
        session.desktop_.remove_output(output);
        if (!session.capture_output_) {
            if (auto *fallback = session.desktop_.focused_output_handle();
                fallback && !session.attach_capture(fallback)) {
                session.fail("output-capture-reattachment");
            }
        }
    }

    static int on_tick(void *data) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (session.stopping_) {
            return 0;
        }
        auto display = seaside_tty_drm_observation(session.drm_);
        const auto input = seaside_tty_input_get_snapshot(session.input_);
        if (display.failed || input.failed || session.scene_.snapshot().failed ||
            session.desktop_.failed()) {
            session.fail("output-cursor-or-input");
            return 0;
        }
        if (display.output_pending &&
            !seaside_tty_drm_enable_pending(session.drm_, session.server_.allocator(),
                                            session.server_.renderer())) {
            session.fail("output-cursor-capture-setup");
            return 0;
        }
        if (display.output_pending) {
            seaside_tty_input_refresh_pointer(session.input_);
        }
        if (!session.gate_) {
            if (Clock::now() >= session.appearance_deadline_) {
                session.fail("hardware-cursor-presentation");
                return 0;
            }
            if (display.active && display.cursor_presented) {
                // The display loop is already live, so a newly activated
                // portal client can connect while readiness checks run.
                session.begin_preflight();
            }
        }
        // Preflight can finish during a VT switch. Wait for the resumed
        // output's hardware cursor before calling the session ready.
        if (!session.ready_ && session.preflight_passed_ && display.active &&
            display.cursor_presented) {
            session.mark_ready(display.cursor_plane_id);
        }
        session.commands_.reap();
        if (!session.stopping_ &&
            wl_event_source_timer_update(session.timer_.get(), session.ready_ ? 100 : 25) < 0) {
            session.fail("observation-timer");
        }
        return 0;
    }

    seaside::Server &server_;
    seaside::Scene &scene_;
    seaside_tty_drm *drm_;
    std::string_view socket_;
    wl_event_loop *loop_;
    seaside::LoadedConfig config_;
    seaside::ConfigStore &store_;
    const seaside::ConfigPaths &paths_;
    Commands commands_;
    seaside::Desktop desktop_;
    seaside_tty_input *input_ = nullptr;
    wlr_output *capture_output_ = nullptr;
    std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> timer_{
        nullptr, wl_event_source_remove};
    std::unique_ptr<seaside_output_capture, decltype(&seaside_output_capture_destroy)> capture_{
        nullptr, seaside_output_capture_destroy};
    std::unique_ptr<seaside::SessionBus> bus_;
    std::unique_ptr<seaside::StartupGate> gate_;
    std::unique_ptr<seaside::SystemdUserProbe> systemd_;
    std::unique_ptr<seaside::PipeWireProbe> pipewire_;
    std::unique_ptr<seaside::PortalActivation> activation_;
    std::unique_ptr<seaside::PortalProbe> portal_;
    std::unique_ptr<seaside::WorkQueue> workers_;
    std::optional<seaside::AsyncConfigIo> config_io_;
    Clock::time_point appearance_deadline_, preflight_deadline_;
    Outcome outcome_;
    bool preflight_passed_ = false;
    bool ready_ = false;
    bool stopping_ = false;
};

std::expected<Outcome, const char *> run_session(seaside::LoadedConfig config,
                                                 seaside::ConfigStore &store,
                                                 const seaside::ConfigPaths &paths) {
    // The seat owner outlives Server, which releases the backend and its
    // borrowed display resources first.
    std::unique_ptr<seaside_tty_drm, decltype(&seaside_tty_drm_destroy)> drm(
        nullptr, seaside_tty_drm_destroy);
    auto server = seaside::Server::create();
    if (!server || !(*server)->install_shutdown_signals()) {
        return std::unexpected("display-or-signals");
    }
    auto scene = seaside::Scene::create((*server)->display());
    const auto socket = (*server)->listen();
    if (!scene || !socket) {
        return std::unexpected("scene-or-wayland-socket");
    }
    wlr_backend *backend = nullptr;
    wlr_renderer *renderer = nullptr;
    drm.reset(seaside_tty_drm_create((*server)->display(), nullptr, &backend, &renderer));
    if (!drm) {
        return std::unexpected("seat-drm-renderer");
    }
    std::unique_ptr<wlr_renderer, decltype(&seaside_wlr_renderer_destroy)> pending_renderer(
        renderer, seaside_wlr_renderer_destroy);
    std::unique_ptr<wlr_backend, decltype(&seaside_wlr_backend_destroy)> pending_backend(
        backend, seaside_wlr_backend_destroy);
    if ((*server)->shutdown_requested()) {
        return std::unexpected("shutdown-during-seat-setup");
    }
    Session session{**server, **scene, drm.get(), *socket, std::move(config), store, paths};
    const seaside_tty_input_callbacks callbacks{
        .motion = Session::pointer_motion,
        .exit_requested = nullptr,
        .launch_requested = nullptr,
        .shortcut_pressed = Session::shortcut_pressed,
        .modifiers_changed = Session::modifiers_changed,
        .pointer_target = Session::pointer_target,
        .pointer_button = Session::pointer_button,
        .pointer_absolute_delta = Session::pointer_absolute_delta,
        .pointer_locked = Session::pointer_locked,
    };
    std::unique_ptr<seaside_tty_input, decltype(&seaside_tty_input_destroy)> input(
        seaside_tty_input_create(backend, &callbacks, &session), seaside_tty_input_destroy);
    if (!input || !seaside_tty_input_enable_keyboard(input.get(), (*server)->display())) {
        return std::unexpected("input-or-wayland-keyboard");
    }
    if (!(*server)->start(pending_backend.release(), pending_renderer.release())) {
        return std::unexpected("backend-start");
    }
    if (!session.observe(input.get())) {
        return std::unexpected("observation-timer");
    }
    const bool dispatched = (*server)->run().has_value();
    auto result = session.shut_down();
    if (!dispatched) {
        return std::unexpected("dispatch");
    }
    return result;
}

std::filesystem::path xdg_path(const char *variable, const std::filesystem::path &fallback) {
    const char *value = std::getenv(variable);
    return value && value[0] == '/' ? std::filesystem::path(value) : fallback;
}

int report_exit(const std::expected<Outcome, const char *> &result) noexcept {
    if (!result || result->failure_stage) {
        std::fprintf(stderr, "seaside FAIL stage=%s\n",
                     result ? result->failure_stage : result.error());
    } else if (result->preflight_failure) {
        const auto &failure = *result->preflight_failure;
        std::fprintf(
            stderr, "seaside FAIL service=%s reason=%s(%d)\n",
            failure.service ? seaside::startup_service_name(*failure.service) : "preflight",
            seaside::service_result_name(failure.detail.failure), failure.detail.native_error);
    } else if (!result->cleanup_failed) {
        std::fputs("seaside STOP clean\n", stderr);
        return EXIT_SUCCESS;
    }
    if (result && result->cleanup_failed) {
        std::fputs("seaside FAIL portal cleanup incomplete\n", stderr);
    }
    return EXIT_FAILURE;
}
} // namespace

int main(int argc, char **) try {
    if (argc != 1) {
        std::fputs("Usage: seaside\n", stderr);
        return EXIT_FAILURE;
    }
    if (!seaside_tty_active_console()) {
        std::fputs("seaside FAIL: start on the active Linux console as a foreground job\n", stderr);
        return EXIT_FAILURE;
    }
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || runtime != "/run/user/" + std::to_string(getuid())) {
        std::fputs("seaside FAIL: the login has no standard Arch user runtime directory\n", stderr);
        return EXIT_FAILURE;
    }
    const char *home = std::getenv("HOME");
    if (!home || home[0] != '/') {
        std::fputs("seaside FAIL: the login has no absolute home directory\n", stderr);
        return EXIT_FAILURE;
    }
    const std::filesystem::path home_path(home);
    const seaside::ConfigPaths paths{
        xdg_path("XDG_CONFIG_HOME", home_path / ".config") / "seaside/config.toml",
        xdg_path("XDG_STATE_HOME", home_path / ".local/state") / "seaside/last-good-config.toml"};
    seaside::ConfigStore store;
    auto config = seaside::load_startup_config(store, paths);
    if (config.diagnostic) {
        const auto &error = *config.diagnostic;
        std::fprintf(stderr, "seaside configuration: %s:%u:%u %s\n", error.path.c_str(), error.line,
                     error.column, error.message.c_str());
    }
    return report_exit(run_session(std::move(config), store, paths));
} catch (const std::exception &error) {
    std::fprintf(stderr, "seaside FAIL: %s\n", error.what());
    return EXIT_FAILURE;
}

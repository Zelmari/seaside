#include "output_capture.h"
#include "satellite.hpp"
#include "scene.hpp"
#include "server.hpp"
#include "session_activation.hpp"
#include "session_control.hpp"
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
using seaside::Action;
using seaside::ActionKind;
using seaside::DesktopActionError;
using seaside::IpcError;
using seaside::IpcJson;
using seaside::IpcResult;
using seaside::LiveConfig;
using seaside::ServiceFailure;
using seaside::ServiceReport;
using seaside::StartupService;

constexpr auto appearance_timeout = 5s;
constexpr auto preflight_timeout = 10s;

std::optional<seaside::LoadedConfig> read_recovery(const seaside::ConfigStore &store,
                                                   const seaside::ConfigPaths &paths,
                                                   const seaside::Diagnostic &error) {
    auto recovery = store.read(paths.recovery, true);
    if (!recovery || !*recovery) {
        return std::nullopt;
    }
    auto candidate = seaside::parse_config(**recovery, paths.recovery.string());
    if (!candidate) {
        return std::nullopt;
    }
    return seaside::LoadedConfig{std::move(*candidate), std::move(**recovery),
                                 seaside::ConfigSource::recovery, error};
}

seaside::LoadedConfig startup_defaults(seaside::Diagnostic error) {
    return {seaside::default_config(), std::string(seaside::default_template()),
            seaside::ConfigSource::defaults, std::move(error)};
}

class Commands {
  public:
    Commands(std::string_view socket, const std::string &control_socket,
             const seaside::Satellite *satellite) {
        for (auto **entry = environ; *entry; ++entry) {
            const std::string_view value(*entry);
            if (!value.starts_with("WAYLAND_SOCKET=") && !value.starts_with("WAYLAND_DISPLAY=") &&
                !value.starts_with("SEASIDE_SOCKET=") && !value.starts_with("DISPLAY=") &&
                !value.starts_with("XAUTHORITY=") && !value.starts_with("XDG_CURRENT_DESKTOP=") &&
                !value.starts_with("XDG_SESSION_TYPE=") &&
                !value.starts_with("DBUS_SESSION_BUS_ADDRESS=")) {
                values_.emplace_back(value);
            }
        }
        values_.push_back("WAYLAND_DISPLAY=" + std::string(socket));
        values_.push_back("SEASIDE_SOCKET=" + control_socket);
        if (satellite) {
            values_.push_back("DISPLAY=" + std::string(satellite->display_name()));
            values_.push_back("XAUTHORITY=" + std::string(satellite->authority_path()));
        }
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
            std::string_view socket, const seaside::ConfigPaths &paths)
        : server_(server), scene_(scene), drm_(drm), socket_(socket),
          loop_(wl_display_get_event_loop(server.display())), paths_(paths),
          desktop_(scene, seaside::default_config()), outputs_(drm, desktop_),
          control_(scene, desktop_, outputs_) {}

    ~Session() noexcept { seaside_tty_drm_set_output_callbacks(drm_, nullptr, nullptr); }

    bool configure(seaside::LoadedConfig config, const seaside::ConfigStore &store,
                   seaside::Wallpaper wallpaper, std::string_view runtime) {
        capture_.reset(seaside_output_capture_create(server_.display()));
        if (!capture_ ||
            !scene_.enable_window_capture(server_.allocator(), server_.renderer(),
                                          seaside_output_capture_copy_manager(capture_.get()))) {
            return false;
        }
        if (!seaside_tty_drm_enable_pending(drm_, server_.allocator(), server_.renderer())) {
            return false;
        }
        if (auto applied = outputs_.apply(config.config, false); !applied) {
            auto diagnostic = config.diagnostic.value_or(std::move(applied.error()));
            if (diagnostic.path.empty()) {
                diagnostic.path = paths_.user.string();
            }
            std::optional<seaside::LoadedConfig> recovered;
            if (config.source == seaside::ConfigSource::user) {
                recovered = read_recovery(store, paths_, diagnostic);
            }
            if (recovered) {
                auto decoded = seaside::decode_wallpaper(recovered->config.wallpaper.path);
                if (decoded && outputs_.apply(recovered->config, false)) {
                    config = std::move(*recovered);
                    wallpaper = std::move(*decoded);
                } else {
                    recovered.reset();
                }
            }
            if (!recovered) {
                config = startup_defaults(std::move(diagnostic));
                wallpaper = {};
                if (!outputs_.apply(config.config, false)) {
                    return false;
                }
            }
        }
        startup_commands_ = config.config.autostart.commands;
        desktop_.apply_initial_config(config.config);
        seaside_tty_input_set_sensitivity(input_, config.config.input.mouse.sensitivity);
        attaching_startup_ = false;
        for (const auto &[connector, output] : outputs_.outputs()) {
            if (output.enabled) {
                const auto &state = output.status;
                if (!attach_desktop_output(output.handle, connector, output.identity,
                                           {state.settings.x, state.settings.y, state.logical_width,
                                            state.logical_height})) {
                    return false;
                }
            }
        }
        outputs_.set_primary();

        auto image = scene_.prepare_wallpaper(std::move(wallpaper));
        if (!image) {
            std::fputs("seaside: could not prepare startup wallpaper buffer\n", stderr);
            return false;
        }
        scene_.set_wallpaper(std::move(*image));
        auto live = LiveConfig::create(
            loop_, store, paths_, std::move(config),
            [path = paths_.user.string()](const seaside::Config &candidate,
                                          const seaside::Config &active)
                -> std::expected<LiveConfig::Prepared, seaside::Diagnostic> {
                if (candidate.wallpaper == active.wallpaper) {
                    return LiveConfig::Prepared{};
                }
                auto image = seaside::decode_wallpaper(candidate.wallpaper.path);
                if (!image) {
                    auto error = std::move(image.error());
                    error.message += " (" + error.path + ")";
                    error.path = path;
                    return std::unexpected(std::move(error));
                }
                return *std::move(image);
            },
            [this](const seaside::Config &active, const seaside::Config &candidate,
                   const LiveConfig::Prepared &prepared) {
                return apply_config(active, candidate, prepared);
            },
            [this](LiveConfig::Change change, const LiveConfig::Prepared &) {
                config_changed(change);
            });
        if (!live) {
            std::fprintf(stderr, "seaside: configuration watch: %s\n", live.error().c_str());
            return false;
        }
        config_ = std::move(*live);
        auto ipc = seaside::IpcServer::create(
            loop_, socket_,
            [this](std::string_view method, const IpcJson &params) {
                return request(method, params);
            },
            [this] { stop_requested(); });
        if (!ipc) {
            std::fprintf(stderr, "seaside: control socket: %s\n", ipc.error().c_str());
            return false;
        }
        ipc_ = std::move(*ipc);
        control_.set_renderer(seaside_wlr_renderer_name(server_.renderer()));
        auto satellite = seaside::Satellite::reserve(loop_, socket_, runtime);
        if (satellite) {
            satellite_ = std::move(*satellite);
        } else {
            std::fprintf(stderr, "seaside: could not reserve an X11 display: %s\n",
                         std::strerror(satellite.error()));
        }
        commands_.emplace(socket_, ipc_->path(), satellite_.get());
        desktop_.on_projection([this] { publish(); });
        desktop_.on_attention([this](seaside::WindowId window, seaside::WorkspaceId workspace) {
            attention(window, workspace);
        });
        if (const auto &diagnostic = config_->transaction().diagnostic()) {
            show_config_error(*diagnostic);
        }
        publish();
        return true;
    }

    bool observe(seaside_tty_input *input) noexcept {
        input_ = input;
        desktop_.set_input(input);
        const seaside_tty_drm_output_callbacks callbacks{
            .added = output_added,
            .removed = output_removed,
            .observed = output_observed,
        };
        seaside_tty_drm_set_output_callbacks(drm_, &callbacks, this);
        timer_.reset(wl_event_loop_add_timer(loop_, on_tick, this));
        appearance_deadline_ = Clock::now() + appearance_timeout;
        return timer_ && wl_event_source_timer_update(timer_.get(), 1) == 0;
    }

    Outcome shut_down() noexcept {
        stopping_ = true;
        timer_.reset();
        satellite_.reset();
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
        commands_->reap();
        config_.reset();
        ipc_.reset();
        if (!clean_exit_ && !ready_ && !outcome_.failure_stage && !outcome_.preflight_failure) {
            outcome_.failure_stage = "interrupted-before-ready";
        }
        return outcome_;
    }

    static void pointer_motion(void *data, bool absolute, double x, double y) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_ && !session.ipc_exiting_) {
            seaside_tty_drm_move(session.drm_, absolute, x, y);
            seaside_output_capture_cursor_changed(session.capture_.get());
            if (seaside_tty_drm_cursor_position(session.drm_, &x, &y)) {
                session.desktop_.motion(x, y);
                session.scene_.capture_pointer(x, y);
            }
        }
    }

    static bool pointer_target(void *data, seaside_tty_pointer_target *target) noexcept {
        auto &session = *static_cast<Session *>(data);
        double x, y;
        seaside_scene_target hit{.surface = target->surface, .id = 0, .sx = 0, .sy = 0};
        if (session.stopping_ || !seaside_tty_drm_cursor_position(session.drm_, &x, &y)) {
            return false;
        }
        session.scene_.move_drag_icon(static_cast<int>(x), static_cast<int>(y));
        if (!session.scene_.window_at(x, y, hit)) {
            return false;
        }
        *target = {hit.surface, hit.sx, hit.sy, hit.id};
        return true;
    }

    static bool pointer_button(void *data, std::uint64_t window, std::uint32_t button, bool pressed,
                               std::uint8_t mods) noexcept {
        auto &session = *static_cast<Session *>(data);
        double x, y;
        return session.ready_ && !session.stopping_ && !session.ipc_exiting_ &&
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
        seaside_output_capture_cursor_changed(session.capture_.get());
        double x, y;
        if (seaside_tty_drm_cursor_position(session.drm_, &x, &y)) {
            session.scene_.capture_pointer(x, y);
        }
    }

    static void pointer_cancelled(void *data) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_) {
            session.desktop_.cancel_drag();
        }
    }

    static void drag_icon(void *data, wlr_drag_icon *icon) noexcept {
        static_cast<Session *>(data)->scene_.set_drag_icon(icon);
    }

    static void modifiers_changed(void *data, std::uint8_t modifiers) noexcept {
        auto &session = *static_cast<Session *>(data);
        session.desktop_.modifiers_changed(modifiers);
    }

    static bool shortcut_pressed(void *data, uint8_t mods, uint32_t keysym) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (session.stopping_ || session.ipc_exiting_) {
            return false;
        }
        if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
            session.desktop_.cancel_drag();
            if (!seaside_tty_drm_change_vt(session.drm_, keysym - XKB_KEY_XF86Switch_VT_1 + 1)) {
                std::fprintf(stderr, "seaside: VT switch rejected\n");
            }
            return true;
        }
        if (!session.ready_) {
            return false;
        }
        const auto &bindings = session.config_->transaction().active().keybinds;
        const auto action = bindings.find({mods, seaside::ChordKind::keyboard, keysym});
        if (action == bindings.end()) {
            return false;
        }
        (void)session.act(action->second, mods);
        return true;
    }

  private:
    std::expected<bool, DesktopActionError>
    act(const Action &action, std::uint8_t modifiers = 0,
        std::optional<seaside::WindowId> window = {},
        std::optional<seaside::WorkspaceId> workspace = {}) noexcept {
        switch (action.kind) {
        case ActionKind::exit:
            desktop_.finish_focus_cycle();
            stop_requested();
            return true;
        case ActionKind::exec:
            desktop_.finish_focus_cycle();
            launch(action.text);
            return true;
        case ActionKind::night_light: {
            desktop_.finish_focus_cycle();
            const auto before = config_->transaction().runtime_night_light();
            const auto enabled = action.night_light == seaside::NightLightAction::toggle
                                     ? !before
                                     : action.night_light == seaside::NightLightAction::on;
            config_->set_runtime_night_light(enabled);
            return before != enabled;
        }
        default:
            return desktop_.dispatch(action, modifiers, window, workspace);
        }
    }

    void stop_requested() noexcept {
        clean_exit_ = true;
        stopping_ = true;
        server_.terminate();
    }

    IpcResult request(std::string_view method, const IpcJson &params) {
        const bool ending = ipc_exiting_ || stopping_;
        if (method.starts_with("query.")) {
            auto last_error = last_error_;
            if (const auto &diagnostic = config_->transaction().diagnostic()) {
                last_error = seaside::ipc_diagnostic(*diagnostic);
            }
            return control_.query(method, *config_, paths_.user.string(),
                                  ending   ? "stopping"
                                  : ready_ ? "ready"
                                           : "starting",
                                  last_error);
        }
        if (method == "action.exit") {
            const bool changed = !ending;
            ipc_exiting_ = true;
            return seaside::ipc_action_result(changed);
        }
        if (!ready_ || ending) {
            return std::unexpected(IpcError{"not_ready", "The session is not accepting actions"});
        }
        if (method.starts_with("trace.")) {
            return std::unexpected(
                IpcError{"unsupported_capability", "Trace recording is scheduled for Part 9"});
        }
        const auto decoded = seaside::decode_ipc_action(method, params);
        return seaside::desktop_action_result(
            act(decoded.action, 0, decoded.window, decoded.workspace));
    }

    std::expected<void, seaside::Diagnostic> apply_config(const seaside::Config &active,
                                                          const seaside::Config &candidate,
                                                          const LiveConfig::Prepared &prepared) {
        auto reject = [this](seaside::Diagnostic error) {
            if (error.path.empty()) {
                error.path = paths_.user.string();
            }
            return std::unexpected(std::move(error));
        };
        std::optional<seaside::SceneImage> wallpaper;
        if (candidate.wallpaper != active.wallpaper) {
            auto pending = scene_.prepare_wallpaper(
                std::static_pointer_cast<const seaside::PixelImage>(prepared));
            if (!pending) {
                return reject(
                    {{}, 0, 0, "wallpaper.path", "Could not allocate the wallpaper buffer"});
            }
            wallpaper = std::move(*pending);
        }
        if (candidate.monitors != active.monitors || candidate.render != active.render) {
            applying_config_ = true;
            try {
                const auto applied = outputs_.apply(candidate);
                applying_config_ = false;
                if (!applied) {
                    return reject(applied.error());
                }
            } catch (...) {
                applying_config_ = false;
                throw;
            }
            reposition_capture();
        }
        if (wallpaper) {
            scene_.set_wallpaper(std::move(*wallpaper));
        }
        seaside_tty_input_set_sensitivity(input_, candidate.input.mouse.sensitivity);
        return {};
    }

    void show_config_error(const seaside::Diagnostic &diagnostic) {
        std::fprintf(stderr, "seaside configuration: %s:%u:%u %s %s\n", diagnostic.path.c_str(),
                     diagnostic.line, diagnostic.column, diagnostic.field.c_str(),
                     diagnostic.message.c_str());
        scene_.set_config_error(diagnostic);
        ipc_->event("config.error",
                    {{"path", diagnostic.path},
                     {"message", diagnostic.message},
                     {"line", diagnostic.line ? IpcJson(diagnostic.line) : IpcJson(nullptr)}});
        config_error_visible_ = true;
    }

    void config_changed(LiveConfig::Change change) {
        const auto &transaction = config_->transaction();
        if (change == LiveConfig::Change::committed) {
            desktop_.apply_config(transaction.active());
            if (std::exchange(config_error_visible_, false)) {
                scene_.set_config_error(std::nullopt);
            }
            ipc_->event("config.recovered", {{"source", "user"}});
            publish();
        } else if (change == LiveConfig::Change::rejected && transaction.diagnostic()) {
            show_config_error(*transaction.diagnostic());
            publish();
        } else if (change == LiveConfig::Change::persistence) {
            if (const auto &error = transaction.persistence_error()) {
                seaside::Diagnostic diagnostic{error->path.string(), 0, 0, "",
                                               error->error.message()};
                std::fprintf(stderr, "seaside recovery snapshot: %s: %s\n", diagnostic.path.c_str(),
                             diagnostic.message.c_str());
                last_error_ = seaside::ipc_diagnostic(diagnostic, "config.persistence");
            } else {
                last_error_ = nullptr;
            }
        }
    }

    void attention(seaside::WindowId window, seaside::WorkspaceId workspace) noexcept try {
        ipc_->event("window.attention", {{"window_id", window}, {"workspace_id", workspace}});
    } catch (const std::exception &error) {
        std::fprintf(stderr, "seaside: could not publish attention event: %s\n", error.what());
    }

    void publish() noexcept try {
        if (!applying_config_ && ipc_ && !stopping_) {
            outputs_.sync_fullscreen(config_->transaction().active());
            outputs_.refresh();
            control_.publish(*ipc_);
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "seaside: could not publish control state: %s\n", error.what());
    }

    void reposition_capture() noexcept {
        for (const auto &[connector, output] : outputs_.outputs()) {
            if (!output.enabled) {
                continue;
            }
            const auto bounds = desktop_.output_bounds(output.handle);
            if (!seaside_output_capture_position(capture_.get(), output.handle, bounds.x,
                                                 bounds.y)) {
                fail("output-capture-position");
            }
        }
    }

    void fail(const char *stage) noexcept {
        stopping_ = true;
        outcome_.failure_stage = stage;
        server_.terminate();
    }

    void launch(const std::string &command) noexcept {
        try {
            const auto pid = commands_->launch(command);
            if (!pid) {
                std::fprintf(stderr, "seaside: command launch failed: %s\n",
                             std::strerror(pid.error()));
                return;
            }
        } catch (...) {
            std::fputs("seaside: couldn't prepare command launch\n", stderr);
        }
    }

    void startup_result(seaside::StartupResult result) noexcept {
        if (ipc_exiting_) {
            return;
        }
        if (result.phase != seaside::StartupPhase::ready) {
            stopping_ = true;
            outcome_.preflight_failure = result;
            server_.terminate();
            return;
        }
        preflight_passed_ = true;
    }

    void mark_ready(uint32_t cursor_plane) noexcept {
        double x, y;
        if (seaside_tty_drm_cursor_position(drm_, &x, &y)) {
            scene_.capture_pointer(x, y);
        }
        if (satellite_ && !satellite_->enable()) {
            std::fputs("seaside: could not enable X11 listeners\n", stderr);
        }
        ready_ = true;
        std::fprintf(stderr, "seaside READY cursor-plane=%" PRIu32 "\n", cursor_plane);
        config_->session_ready();
        for (const auto &command : startup_commands_) {
            launch(command);
        }
    }

    void report(StartupService service, ServiceReport detail) noexcept {
        if (stopping_ || ipc_exiting_) {
            return;
        }
        const auto delivered = gate_->report(service, detail);
        if (!delivered && delivered.error() != seaside::StartupError::stopped) {
            fail("preflight-report");
        }
    }

    void report_external_fault(StartupService service, ServiceReport detail) noexcept {
        if (stopping_ || ipc_exiting_) {
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
        const auto name = seaside::startup_service_name(service);
        const auto reason = seaside::service_result_name(detail.failure);
        std::fprintf(stderr, "seaside service fault service=%s reason=%s(%d)\n", name, reason,
                     detail.native_error);
        try {
            ipc_->event("integration.failed", {{"name", name}, {"message", reason}});
        } catch (...) {
        }
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
                return seaside_output_capture_client_ready(capture_.get(), pid, uid) &&
                       scene_.window_capture_client_ready(pid, uid);
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
                if (detail.failure == ServiceFailure::none && !stopping_ && !ipc_exiting_ &&
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
                if (detail.failure == ServiceFailure::none && !stopping_ && !ipc_exiting_ &&
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

    bool attach_desktop_output(wlr_output *output, std::string_view connector,
                               std::string_view identity, seaside::WindowRect bounds) noexcept {
        if (!desktop_.add_output(output, connector, identity, bounds)) {
            return false;
        }
        if (!seaside_output_capture_add_output(capture_.get(), output, bounds.x, bounds.y)) {
            desktop_.remove_output(output);
            return false;
        }
        return true;
    }

    static bool output_added(void *data, wlr_output *output, const char *connector,
                             const char *identity, int x, int y, int width, int height) noexcept {
        auto &session = *static_cast<Session *>(data);
        session.outputs_.enabled(connector, true);
        if (session.attaching_startup_) {
            return true;
        }
        session.output_config_pending_ = true;
        return session.attach_desktop_output(output, connector, identity ? identity : "",
                                             {x, y, width, height});
    }

    static void output_removed(void *data, wlr_output *output, const char *connector) noexcept {
        auto &session = *static_cast<Session *>(data);
        session.outputs_.enabled(connector, false);
        seaside_output_capture_remove_output(session.capture_.get(), output);
        session.desktop_.remove_output(output);
    }

    static void output_observed(void *data, wlr_output *output, const char *connector,
                                const char *identity, bool connected, const char *reason) noexcept {
        auto &session = *static_cast<Session *>(data);
        try {
            session.outputs_.observe(output, connector, identity ? identity : "", connected,
                                     reason);
            session.publish();
        } catch (...) {
            session.fail("output-status");
        }
    }

    void apply_detected_outputs() noexcept try {
        if (!std::exchange(output_config_pending_, false)) {
            return;
        }
        applying_config_ = true;
        auto result = outputs_.apply(config_->transaction().active());
        applying_config_ = false;
        if (!result) {
            auto error = std::move(result.error());
            error.path = paths_.user.string();
            last_error_ = seaside::ipc_diagnostic(error, "output");
            std::fprintf(stderr, "seaside output settings: %s\n", error.message.c_str());
        }
        reposition_capture();
        publish();
    } catch (...) {
        applying_config_ = false;
        fail("output-settings");
    }

    static int on_tick(void *data) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (session.stopping_ || session.ipc_exiting_) {
            return 0;
        }
        auto display = seaside_tty_drm_observation(session.drm_);
        if (!display.active) {
            session.desktop_.cancel_drag();
            seaside_tty_input_cancel_drag(session.input_);
        }
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
            session.apply_detected_outputs();
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
        session.commands_->reap();
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
    const seaside::ConfigPaths &paths_;
    std::vector<std::string> startup_commands_;
    std::unique_ptr<seaside::Satellite> satellite_;
    std::optional<Commands> commands_;
    seaside::Desktop desktop_;
    seaside::OutputConfig outputs_;
    seaside::SessionControl control_;
    std::unique_ptr<LiveConfig> config_;
    std::unique_ptr<seaside::IpcServer> ipc_;
    IpcJson last_error_ = nullptr;
    seaside_tty_input *input_ = nullptr;
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
    Clock::time_point appearance_deadline_, preflight_deadline_;
    Outcome outcome_;
    bool attaching_startup_ = true;
    bool applying_config_ = false;
    bool output_config_pending_ = false;
    bool config_error_visible_ = false;
    bool ipc_exiting_ = false;
    bool clean_exit_ = false;
    bool preflight_passed_ = false;
    bool ready_ = false;
    bool stopping_ = false;
};

std::expected<Outcome, const char *>
run_session(seaside::LoadedConfig config, seaside::Wallpaper wallpaper, seaside::ConfigStore &store,
            const seaside::ConfigPaths &paths, std::string_view runtime) {
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
    Session session{**server, **scene, drm.get(), *socket, paths};
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
        .pointer_cancelled = Session::pointer_cancelled,
        .drag_icon = Session::drag_icon,
    };
    std::unique_ptr<seaside_tty_input, decltype(&seaside_tty_input_destroy)> input(
        seaside_tty_input_create(backend, &callbacks, &session), seaside_tty_input_destroy);
    if (!input || !seaside_tty_input_enable_keyboard(input.get(), (*server)->display())) {
        return std::unexpected("input-or-wayland-keyboard");
    }
    if (!session.observe(input.get())) {
        return std::unexpected("observation-timer");
    }
    if (!(*server)->start(pending_backend.release(), pending_renderer.release())) {
        return std::unexpected("backend-start");
    }
    if (!session.configure(std::move(config), store, std::move(wallpaper), runtime)) {
        return std::unexpected("configuration-or-ipc");
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
    const std::string runtime = "/run/user/" + std::to_string(getuid());
    const char *runtime_env = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime_env || runtime != runtime_env) {
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
    auto wallpaper = seaside::decode_wallpaper(config.config.wallpaper.path);
    if (!wallpaper) {
        auto error = config.diagnostic ? *config.diagnostic : wallpaper.error();
        if (!config.diagnostic) {
            error.message += " (" + error.path + ")";
            error.path = paths.user.string();
        }
        if (config.source == seaside::ConfigSource::user) {
            if (auto recovered = read_recovery(store, paths, error)) {
                wallpaper = seaside::decode_wallpaper(recovered->config.wallpaper.path);
                if (wallpaper) {
                    config = std::move(*recovered);
                }
            }
        }
        if (!wallpaper) {
            config = startup_defaults(std::move(error));
            wallpaper = seaside::Wallpaper{};
        }
    }
    return report_exit(
        run_session(std::move(config), std::move(*wallpaper), store, paths, runtime));
} catch (const std::exception &error) {
    std::fprintf(stderr, "seaside FAIL: %s\n", error.what());
    return EXIT_FAILURE;
}

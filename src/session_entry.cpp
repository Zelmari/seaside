#include "build_info.hpp"
#include "idle_power.hpp"
#include "ipc_protocol.hpp"
#include "output_capture.h"
#include "satellite.hpp"
#include "scene.hpp"
#include "server.hpp"
#include "session_activation.hpp"
#include "session_control.hpp"
#include "session_environment.hpp"
#include "session_portal.hpp"
#include "session_preferences.hpp"
#include "session_services.hpp"
#include "session_systemd.hpp"
#include "trace.hpp"
#include "tty_drm.h"
#include "tty_input.h"
#include "wlroots_bridge.h"

#include <algorithm>
#include <array>
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
constexpr auto probe_timeout = 10s;

struct RecoveryRetry {
    void request() noexcept {
        if (!running && !due) {
            due = Clock::now() + 250ms;
        }
    }

    bool begin(Clock::time_point now) noexcept {
        if (!due || now < *due) {
            return false;
        }
        due.reset();
        running = true;
        return true;
    }

    void failed() noexcept {
        running = false;
        due = Clock::now() + delay;
        delay = std::min(delay * 2, 8s);
    }

    std::optional<Clock::time_point> due;
    std::chrono::seconds delay = 1s;
    bool running = false;
};

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

    std::expected<pid_t, int> launch(const std::string &command, seaside::ColorScheme scheme) {
        // Reserve the reaper slot before a child can start. Launching the
        // default terminal with exec keeps its lifetime tied to that slot.
        children_.push_back(0);
        const char *line = command.c_str();
        if (command == "foot") {
            line = "exec foot";
        } else if (command == "wofi --show drun" && !std::getenv("GTK_THEME")) {
            // The GTK3 launcher does not consume the standard appearance key.
            line = scheme == seaside::ColorScheme::dark ? "GTK_THEME=Adwaita:dark wofi --show drun"
                                                        : "GTK_THEME=Adwaita wofi --show drun";
        }
        char *const argv[]{const_cast<char *>("sh"), const_cast<char *>("-c"),
                           const_cast<char *>(line), nullptr};
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
    bool cleanup_failed = false;
};

class Session {
  public:
    Session(seaside::Server &server, seaside::Scene &scene, seaside_tty_drm *drm,
            std::string_view socket, const seaside::ConfigPaths &paths)
        : server_(server), scene_(scene), drm_(drm), socket_(socket),
          loop_(wl_display_get_event_loop(server.display())), paths_(paths),
          desktop_(scene, seaside::default_config()), outputs_(drm, desktop_),
          control_(scene, desktop_, outputs_), power_(loop_) {
        desktop_.enable_frame_paced_resize();
    }

    ~Session() noexcept {
        scene_.set_frame_observer(nullptr, nullptr);
        seaside_tty_drm_set_output_callbacks(drm_, nullptr, nullptr);
    }

    bool configure(seaside::LoadedConfig config, const seaside::ConfigStore &store,
                   seaside::Wallpaper wallpaper, std::string_view runtime) {
        auto trace = seaside::Trace::create(loop_, paths_.recovery.parent_path());
        if (!trace) {
            std::fprintf(stderr, "seaside: trace setup: %s\n", trace.error().c_str());
            return false;
        }
        trace_ = std::move(*trace);
        idle_ = seaside::IdlePower::create(
            loop_, server_.display(),
            [this](wlr_output *output, bool on) {
                const bool changed = seaside_tty_drm_set_output_power(drm_, output, on);
                idle_power_changed_ |= changed;
                return changed;
            },
            [this](wlr_surface *surface) { return scene_.visible_output(surface); },
            [this](wlr_output *output) { return desktop_.fullscreen_inhibits_idle(output); });
        if (!idle_) {
            return false;
        }
        capture_.reset(seaside_output_capture_create(server_.display()));
        if (!capture_ ||
            !scene_.enable_window_capture(server_.allocator(), server_.renderer(),
                                          seaside_output_capture_copy_manager(capture_.get()))) {
            return false;
        }
        if (!seaside_tty_drm_enable_pending(drm_, server_.allocator(), server_.renderer(),
                                            outputs_.admission_preference(config.config))) {
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
        desktop_.apply_config(config.config);
        seaside_tty_input_set_sensitivity(input_, config.config.input.mouse.sensitivity);
        seaside_tty_input_set_inverse_scroll(input_, config.config.input.mouse.scroll_direction ==
                                                         seaside::ScrollDirection::inverse);
        attaching_startup_ = false;
        const auto attach = [this](const seaside::OutputConfig::Output &output) {
            const auto &state = output.status;
            return attach_desktop_output(output, {state.settings.x, state.settings.y,
                                                  state.logical_width, state.logical_height});
        };
        const auto *primary = outputs_.primary();
        if (primary && !attach(*primary)) {
            return false;
        }
        for (const auto &[id, output] : outputs_.outputs()) {
            if (output.enabled && &output != primary && !attach(output)) {
                return false;
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
        apply_render_policy();
        apply_idle_timeout();
        apply_service_preferences();
        auto ipc = seaside::IpcServer::create(
            loop_, socket_,
            [this](std::string_view method, const IpcJson &params) {
                return request(method, params);
            },
            [this] { stop_requested(); },
            [this](std::string_view method, const IpcJson &params,
                   seaside::IpcServer::Completion done) -> std::optional<IpcResult> {
                if (!ready_ || stopping_ || ipc_exiting_) {
                    return std::unexpected(
                        IpcError{"not_ready", "The session is not accepting actions"});
                }
                if (method == "trace.export")
                    return trace_->export_completed(std::move(done));
                auto edit = method == "action.night_light_set"
                                ? seaside::SettingEdit{seaside::Setting::night_light_enabled,
                                                       params.at("enabled").get<bool>()}
                                : seaside::ipc::decode_setting_edit(params);
                if (!edit)
                    return std::unexpected(std::move(edit.error()));
                config_->edit(std::move(*edit), [this, done = std::move(done)](
                                                    LiveConfig::EditResult result) {
                    if (!result) {
                        done(std::unexpected(IpcError{std::move(result.error().code),
                                                      std::move(result.error().message)}));
                        return;
                    }
                    auto reply = seaside::ipc_action_result(*result);
                    reply["revision"] = config_->revision();
                    done(std::move(reply));
                });
                return std::nullopt;
            });
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
        desktop_.on_projection([this] {
            idle_->visibility_changed();
            publish();
        });
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
            .cursor_presented = cursor_presented,
        };
        seaside_tty_drm_set_output_callbacks(drm_, &callbacks, this);
        timer_.reset(wl_event_loop_add_timer(loop_, on_tick, this));
        return timer_ && wl_event_source_timer_update(timer_.get(), 1) == 0;
    }

    Outcome shut_down() noexcept {
        stopping_ = true;
        timer_.reset();
        scene_.set_frame_observer(nullptr, nullptr);
        idle_->set_active(false);
        portal_.reset();
        systemd_.reset();
        pipewire_.reset();
        power_.stop();
        if (activation_) {
            // Waiting for the private units also dispatches the Wayland loop.
            // Read-only IPC and already registered callbacks still use the
            // session configuration, diagnostics and X11 reservation here.
            outcome_.cleanup_failed = !activation_->stop();
            activation_.reset();
        }
        settings_.reset();
        // End the bridge and its Xwayland process before closing their Wayland connection.
        satellite_.reset();
        // Disconnect Wayland clients while the seat and surface owners still exist.
        // Destroying the seat first would send data-control device events to
        // clients whose session is already ending.
        wl_display_destroy_clients(server_.display());
        seaside_tty_drm_set_output_callbacks(drm_, nullptr, nullptr);
        desktop_.set_input(nullptr);
        commands_->reap();
        ipc_.reset();
        config_.reset();
        trace_.reset();
        if (!clean_exit_ && !ready_ && !outcome_.failure_stage) {
            outcome_.failure_stage = "interrupted-before-ready";
        }
        return outcome_;
    }

    static void input_activity(void *data, bool) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.idle_ || session.stopping_ || session.ipc_exiting_) {
            return;
        }
        session.idle_->activity();
    }

    static void record_frame(void *data, wlr_output *output,
                             const seaside_scene_frame_observation *observation) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.trace_->running()) {
            session.scene_.set_frame_observer(nullptr, nullptr);
            return;
        }
        try {
            for (const auto &[id, display] : session.outputs_.outputs()) {
                if (display.handle != output) {
                    continue;
                }
                session.trace_->record(
                    {{"event", observation->event},
                     {"output_id", display.id},
                     {"commit_seq", observation->succeeded || observation->commit_seq
                                        ? IpcJson(observation->commit_seq)
                                        : IpcJson(nullptr)},
                     {"succeeded", observation->succeeded},
                     {"presentation_mode", observation->presentation_mode
                                               ? IpcJson(observation->presentation_mode)
                                               : IpcJson(nullptr)},
                     {"tearing", observation->tearing},
                     {"presentation_ns", observation->presentation_ns
                                             ? IpcJson(std::to_string(observation->presentation_ns))
                                             : IpcJson(nullptr)}});
                return;
            }
        } catch (...) {
        }
        session.trace_->drop_sample();
    }

    static void pointer_motion(void *data, seaside_pointer_motion kind, double x,
                               double y) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_ && !session.ipc_exiting_) {
            seaside_tty_drm_move(session.drm_, kind, x, y);
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
        session.cursor_changed();
    }

    void cursor_changed() noexcept {
        seaside_output_capture_cursor_changed(capture_.get());
        double x, y;
        if (seaside_tty_drm_cursor_position(drm_, &x, &y)) {
            scene_.capture_pointer(x, y);
        }
    }

    static void cursor_shape(void *data, const char *shape) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_ && seaside_tty_drm_set_cursor_shape(session.drm_, shape)) {
            session.cursor_changed();
        }
    }

    static void cursor_surface(void *data, wlr_surface *surface, std::int32_t x,
                               std::int32_t y) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_ && seaside_tty_drm_set_cursor_surface(session.drm_, surface, x, y)) {
            session.cursor_changed();
        }
    }

    static void cursor_presented(void *data, wlr_output *output, const timespec *when) noexcept {
        auto &session = *static_cast<Session *>(data);
        if (!session.stopping_) {
            seaside_tty_input_cursor_presented(session.input_, output, when);
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
        std::optional<seaside::WindowId> window = {}) noexcept {
        switch (action.kind) {
        case ActionKind::exit:
            desktop_.finish_focus_cycle();
            stop_requested();
            return true;
        case ActionKind::exec:
            desktop_.finish_focus_cycle();
            launch(action.text);
            return true;
        case ActionKind::night_light:
            desktop_.finish_focus_cycle();
            try {
                config_->edit({seaside::Setting::night_light_enabled, action.night_light},
                              [](LiveConfig::EditResult result) {
                                  if (!result)
                                      std::fprintf(stderr, "seaside: night light: %s\n",
                                                   result.error().message.c_str());
                              });
                return true;
            } catch (...) {
                return std::unexpected(DesktopActionError::invalid_state);
            }
        default:
            return desktop_.dispatch(action, modifiers, window);
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
            auto result = control_.query(method, *config_, *trace_, paths_.user.string(),
                                         ending   ? "stopping"
                                         : ready_ ? "ready"
                                                  : "starting",
                                         last_error);
            if (result && method == "query.status") {
                auto &services = (*result)["integrations"] = IpcJson::object();
                for (std::size_t i = 0; i < integrations_.size(); ++i) {
                    const auto service = static_cast<StartupService>(i);
                    const auto &detail = integrations_[i];
                    auto &status = services[seaside::startup_service_name(service)];
                    if (const auto dependency = blocked_by(service)) {
                        status = {{"state", "unavailable"},
                                  {"reason", "dependency-unavailable"},
                                  {"dependency", seaside::startup_service_name(*dependency)}};
                    } else if (!detail) {
                        status = {{"state", "checking"}};
                    } else if (detail->failure == ServiceFailure::none) {
                        status = {{"state", "available"}};
                    } else {
                        status = {{"state", "unavailable"},
                                  {"reason", seaside::service_result_name(detail->failure)},
                                  {"native_error", detail->native_error}};
                    }
                }
                append_preferences_status(*result);
            }
            if (result && method == "query.settings")
                (*result)["power"] = power_status();
            return result;
        }
        if (method == "action.exit") {
            const bool changed = !ending;
            ipc_exiting_ = true;
            return seaside::ipc_action_result(changed);
        }
        if (!ready_ || ending) {
            return std::unexpected(IpcError{"not_ready", "The session is not accepting actions"});
        }
        if (method == "trace.start") {
            auto response = trace_->start();
            if (response) {
                scene_.set_frame_observer(record_frame, this);
            }
            return response;
        }
        if (method == "trace.stop") {
            scene_.set_frame_observer(nullptr, nullptr);
            return trace_->stop();
        }
        const auto decoded = seaside::decode_ipc_action(method, params);
        return seaside::desktop_action_result(act(decoded.action, 0, decoded.window));
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
        seaside_tty_input_set_inverse_scroll(input_, candidate.input.mouse.scroll_direction ==
                                                         seaside::ScrollDirection::inverse);
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

    void apply_render_policy() {
        const auto &transaction = config_->transaction();
        const auto &config = transaction.active();
        scene_.set_render_policy(config.render.direct_scanout, config.render.allow_tearing,
                                 config.night_light.enabled, config.night_light.temperature_kelvin);
        scene_.set_blur(config.render.blur.enabled, config.render.blur.strength,
                        config.render.blur.passes);
    }

    void apply_idle_timeout() {
        idle_->set_timeout(
            std::chrono::minutes(config_->transaction().active().idle.display_off_after_minutes));
    }

    void apply_service_preferences() noexcept {
        const auto &active = config_->transaction().active();
        if (pipewire_) {
            pipewire_->set_audio_routes(active.audio.default_output, active.audio.default_input);
        }
        if (settings_) {
            settings_->set(active.appearance.color_scheme);
        }
        power_.configure(active.power.profile);
    }

    void append_preferences_status(IpcJson &result) const {
        const auto &active = config_->transaction().active();
        const auto route = [&](bool playback, const std::optional<std::string> &requested) {
            const auto status =
                pipewire_ ? pipewire_->audio_route_status(playback) : seaside::AudioRouteStatus{};
            const char *state = !requested                      ? "auto"
                                : !status.metadata_ready        ? "checking"
                                : !status.available             ? "device-unavailable"
                                : status.effective == requested ? "applied"
                                                                : "pending";
            return IpcJson{{"requested", requested ? IpcJson(*requested) : IpcJson("auto")},
                           {"effective", status.effective ? IpcJson(*status.effective) : nullptr},
                           {"state", state}};
        };
        result["audio"] = {{"output", route(true, active.audio.default_output)},
                           {"input", route(false, active.audio.default_input)}};
        result["appearance"] = {
            {"color_scheme",
             active.appearance.color_scheme == seaside::ColorScheme::dark ? "dark" : "light"}};
        result["power"] = power_status();
    }

    IpcJson power_status() const {
        const auto &profile = config_->transaction().active().power.profile;
        const auto &power = power_.status();
        return {{"requested", profile ? seaside::power_profile_name(*profile) : "system"},
                {"effective", power.effective ? IpcJson(*power.effective) : nullptr},
                {"available", power.available},
                {"state", power.state}};
    }

    void config_changed(LiveConfig::Change change) {
        const auto &transaction = config_->transaction();
        if (change == LiveConfig::Change::committed) {
            apply_render_policy();
            apply_idle_timeout();
            desktop_.apply_config(transaction.active());
            apply_service_preferences();
            if (std::exchange(config_error_visible_, false)) {
                scene_.set_config_error(std::nullopt);
            }
            ipc_->event("config.recovered", {{"source", "user"}});
            ipc_->event("settings.changed", {{"revision", config_->revision()}});
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
        if (!applying_config_ && !output_config_pending_ && ipc_ && !stopping_) {
            outputs_.sync_fullscreen(config_->transaction().active());
            outputs_.refresh();
            control_.publish(*ipc_);
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "seaside: could not publish control state: %s\n", error.what());
    }

    void reposition_capture() noexcept {
        for (const auto &[id, output] : outputs_.outputs()) {
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
            const auto pid =
                commands_->launch(command, config_->transaction().active().appearance.color_scheme);
            if (!pid) {
                std::fprintf(stderr, "seaside: command launch failed: %s\n",
                             std::strerror(pid.error()));
                return;
            }
        } catch (...) {
            std::fputs("seaside: couldn't prepare command launch\n", stderr);
        }
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
        if (cursor_plane) {
            std::fprintf(stderr, "seaside READY cursor-plane=%" PRIu32 "\n", cursor_plane);
        } else {
            std::fputs("seaside READY outputs=none\n", stderr);
        }
        config_->session_ready();
        if (!startup_commands_.empty()) {
            autostart_deadline_ = Clock::now() + probe_timeout;
        }
    }

    void run_autostart() noexcept {
        autostart_deadline_.reset();
        for (const auto &command : startup_commands_) {
            launch(command);
        }
        startup_commands_.clear();
    }

    void report_external_fault(StartupService service, ServiceReport detail) noexcept {
        if (stopping_ || ipc_exiting_) {
            return;
        }
        mark_integration_failed(service, detail);
        (service == StartupService::pipewire ? pipewire_retry_ : portal_retry_).request();
    }

    std::optional<StartupService> blocked_by(StartupService service) const noexcept {
        if (!portal_blocker_ || service == StartupService::systemd_user ||
            service == StartupService::pipewire || service == *portal_blocker_) {
            return {};
        }
        const auto &dependency = integrations_[static_cast<std::size_t>(*portal_blocker_)];
        if (!dependency || dependency->failure == ServiceFailure::none) {
            return {};
        }
        const auto &status = integrations_[static_cast<std::size_t>(service)];
        if (!status || *portal_blocker_ == StartupService::systemd_user ||
            dependency->failure == ServiceFailure::wrong_environment ||
            (*portal_blocker_ == StartupService::portal_frontend &&
             service == StartupService::capture_backend)) {
            return portal_blocker_;
        }
        return {};
    }

    void mark_integration_failed(StartupService service, ServiceReport detail) noexcept {
        if (service != StartupService::pipewire) {
            portal_blocker_ = service;
        }
        auto &current = integrations_[static_cast<std::size_t>(service)];
        const bool changed = !current || current->failure != detail.failure ||
                             current->native_error != detail.native_error;
        current = detail;
        if (!changed) {
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

    void mark_integration_recovered(StartupService service) noexcept {
        const auto before =
            std::exchange(integrations_[static_cast<std::size_t>(service)], ServiceReport{});
        if (!before || before->failure == ServiceFailure::none) {
            return;
        }
        const auto name = seaside::startup_service_name(service);
        std::fprintf(stderr, "seaside service recovered service=%s\n", name);
        try {
            ipc_->event("integration.recovered", {{"name", name}});
        } catch (...) {
        }
    }

    void finish_portal_recovery(StartupService service, ServiceReport detail) noexcept {
        if (stopping_ || ipc_exiting_) {
            return;
        }
        if (detail.failure != ServiceFailure::none) {
            mark_integration_failed(service, detail);
            portal_retry_.failed();
            run_autostart();
        } else if (service == StartupService::capture_backend) {
            // PortalProbe reports its final success after verifying both pinned
            // owners, backend bindings, and an operational ScreenCast route.
            mark_integration_recovered(StartupService::systemd_user);
            mark_integration_recovered(StartupService::portal_frontend);
            mark_integration_recovered(StartupService::capture_backend);
            mark_integration_recovered(StartupService::gtk_backend);
            portal_blocker_.reset();
            portal_retry_ = {};
        }
    }

    void start_portal_probe() noexcept {
        const auto complete = [this](StartupService service, ServiceReport detail) {
            finish_portal_recovery(service, detail);
        };
        auto portal = seaside::PortalProbe::start(
            *bus_, getuid(),
            [this](pid_t pid, uid_t uid) {
                return seaside_output_capture_client_ready(capture_.get(), pid, uid) &&
                       scene_.window_capture_client_ready(pid, uid);
            },
            complete, probe_timeout,
            [this](StartupService service, ServiceReport detail) {
                report_external_fault(service, detail);
            },
            seaside::PortalProbe::Sources::monitor_and_window, activation_->gtk_owner());
        if (!portal) {
            complete(StartupService::portal_frontend, portal.error());
        } else {
            portal_ = std::move(*portal);
        }
    }

    void start_activation() noexcept {
        const auto complete = [this](StartupService service, ServiceReport detail) {
            if (stopping_ || ipc_exiting_) {
                return;
            }
            if (detail.failure == ServiceFailure::none) {
                // An autostarted app may request a portal immediately. Publish
                // the session environment and establish our owners first.
                run_autostart();
                start_portal_probe();
            } else {
                finish_portal_recovery(service, detail);
            }
        };
        if (activation_) {
            if (const auto result = activation_->recover(*bus_, complete, probe_timeout); !result) {
                finish_portal_recovery(StartupService::capture_backend, result.error());
            }
            return;
        }
        auto activation = seaside::PortalActivation::start(
            *bus_, socket_, complete, probe_timeout,
            satellite_ ? satellite_->display_name() : std::string_view{},
            satellite_ ? satellite_->authority_path() : std::string_view{});
        if (activation) {
            activation_ = std::move(*activation);
        } else {
            finish_portal_recovery(StartupService::capture_backend, activation.error());
        }
    }

    void retry_portal() noexcept {
        portal_.reset();
        systemd_.reset();
        if (!bus_ || bus_->last_error() != seaside::BusError::none) {
            auto replacement = seaside::open_login_bus(loop_, getuid());
            if (!replacement) {
                finish_portal_recovery(StartupService::systemd_user, replacement.error());
                return;
            }
            if (activation_) {
                activation_->replace_bus(**replacement);
            }
            settings_.reset();
            bus_ = std::move(*replacement);
        }
        if (!settings_) {
            settings_ = seaside::SettingsPortal::start(
                *bus_, config_->transaction().active().appearance.color_scheme);
        }
        auto systemd = seaside::SystemdUserProbe::start(
            *bus_, getuid(),
            [this](ServiceReport detail) {
                if (stopping_ || ipc_exiting_) {
                    return;
                }
                if (detail.failure == ServiceFailure::none) {
                    mark_integration_recovered(StartupService::systemd_user);
                    start_activation();
                } else {
                    finish_portal_recovery(StartupService::systemd_user, detail);
                }
            },
            probe_timeout);
        if (!systemd) {
            finish_portal_recovery(StartupService::systemd_user, systemd.error());
        } else {
            systemd_ = std::move(*systemd);
        }
    }

    void start_pipewire() noexcept {
        const auto complete = [this](ServiceReport detail) {
            if (stopping_ || ipc_exiting_) {
                return;
            }
            if (detail.failure != ServiceFailure::none) {
                mark_integration_failed(StartupService::pipewire, detail);
                pipewire_retry_.failed();
            } else {
                mark_integration_recovered(StartupService::pipewire);
                pipewire_retry_ = {};
            }
        };
        auto probe = seaside::PipeWireProbe::create(
            loop_, {.timeout = probe_timeout, .audio_ownership = audio_ownership_},
            [complete](seaside::PipeWireResult result) {
                complete(seaside::pipewire_service_report(result));
            },
            [this](int error) {
                report_external_fault(
                    StartupService::pipewire,
                    seaside::pipewire_service_report({seaside::PipeWireStatus::failed, error}));
            });
        if (!probe) {
            complete(seaside::pipewire_service_report(probe.error()));
        } else {
            pipewire_ = std::move(*probe);
            const auto &audio = config_->transaction().active().audio;
            pipewire_->set_audio_routes(audio.default_output, audio.default_input);
        }
    }

    void recover_integrations() noexcept {
        if (bus_ && bus_->last_error() != seaside::BusError::none) {
            constexpr ServiceReport disconnected{ServiceFailure::disconnected, -ENOTCONN};
            report_external_fault(StartupService::systemd_user, disconnected);
        }
        const auto now = Clock::now();
        if (autostart_deadline_ && now >= *autostart_deadline_) {
            run_autostart();
        }
        if (portal_retry_.begin(now)) {
            retry_portal();
        }
        if (pipewire_retry_.begin(now)) {
            pipewire_.reset();
            start_pipewire();
        }
        if (pipewire_) {
            pipewire_->refresh_audio_routes();
        }
    }

    bool attach_desktop_output(const seaside::OutputConfig::Output &output,
                               seaside::WindowRect bounds) noexcept {
        if (!desktop_.add_output(output.handle, output.id, bounds)) {
            return false;
        }
        if (!seaside_output_capture_add_output(capture_.get(), output.handle, bounds.x, bounds.y)) {
            desktop_.remove_output(output.handle);
            return false;
        }
        if (!idle_->add(output.handle)) {
            seaside_output_capture_remove_output(capture_.get(), output.handle);
            desktop_.remove_output(output.handle);
            return false;
        }
        return true;
    }

    static bool output_added(void *data, wlr_output *output, const char *, const char *, int x,
                             int y, int width, int height) noexcept {
        auto &session = *static_cast<Session *>(data);
        const auto *record = session.outputs_.enabled(output, true);
        if (!record) {
            return false;
        }
        if (session.attaching_startup_) {
            return true;
        }
        session.output_config_pending_ = true;
        return session.attach_desktop_output(*record, {x, y, width, height});
    }

    static void output_removed(void *data, wlr_output *output, const char *) noexcept {
        auto &session = *static_cast<Session *>(data);
        session.output_config_pending_ = !session.attaching_startup_;
        session.idle_->remove(output);
        session.outputs_.enabled(output, false);
        seaside_output_capture_remove_output(session.capture_.get(), output);
        session.desktop_.remove_output(output);
    }

    static void output_observed(void *data, wlr_output *output, const char *connector,
                                const char *identity, bool connected, const char *reason) noexcept {
        auto &session = *static_cast<Session *>(data);
        try {
            session.outputs_.observe(output, connector, identity ? identity : "", connected,
                                     reason);
            session.output_config_pending_ = !session.attaching_startup_;
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
            !seaside_tty_drm_enable_pending(
                session.drm_, session.server_.allocator(), session.server_.renderer(),
                session.outputs_.admission_preference(session.config_->transaction().active()))) {
            session.fail("output-cursor-capture-setup");
            return 0;
        }
        if (display.active && (display.output_pending || session.output_config_pending_)) {
            session.apply_detected_outputs();
            seaside_tty_input_refresh_pointer(session.input_);
            display = seaside_tty_drm_observation(session.drm_);
        }
        if (!display.active) {
            session.appearance_deadline_.reset();
        } else if (!session.ready_) {
            const bool has_output = std::ranges::any_of(
                session.outputs_.outputs(), [](const auto &entry) { return entry.second.enabled; });
            // With no usable output there is no cursor to present. The seat
            // and Wayland server can still accept clients and probe services.
            if (display.cursor_presented || (!has_output && !display.output_pending)) {
                session.appearance_deadline_.reset();
                session.mark_ready(display.cursor_plane_id);
                session.portal_retry_.due = session.pipewire_retry_.due = Clock::now();
            } else if (has_output) {
                const auto now = Clock::now();
                if (!session.appearance_deadline_) {
                    session.appearance_deadline_ = now + appearance_timeout;
                } else if (now >= *session.appearance_deadline_) {
                    seaside_tty_drm_reject_unpresented_cursors(session.drm_);
                    session.appearance_deadline_.reset();
                }
            }
        }
        if (session.ready_) {
            session.recover_integrations();
            session.power_.tick(display.active);
        }
        session.idle_->set_active(session.ready_ && display.active);
        if (std::exchange(session.idle_power_changed_, false)) {
            session.publish();
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
    std::unique_ptr<seaside::IdlePower> idle_;
    std::unique_ptr<seaside::Trace> trace_;
    std::unique_ptr<seaside::IpcServer> ipc_;
    IpcJson last_error_ = nullptr;
    seaside_tty_input *input_ = nullptr;
    std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> timer_{
        nullptr, wl_event_source_remove};
    std::unique_ptr<seaside_output_capture, decltype(&seaside_output_capture_destroy)> capture_{
        nullptr, seaside_output_capture_destroy};
    std::unique_ptr<seaside::SessionBus> bus_;
    std::unique_ptr<seaside::SettingsPortal> settings_;
    seaside::PowerProfiles power_;
    std::unique_ptr<seaside::SystemdUserProbe> systemd_;
    std::shared_ptr<seaside::AudioRouteOwnership> audio_ownership_ =
        std::make_shared<seaside::AudioRouteOwnership>();
    std::unique_ptr<seaside::PipeWireProbe> pipewire_;
    std::unique_ptr<seaside::PortalActivation> activation_;
    std::unique_ptr<seaside::PortalProbe> portal_;
    std::array<std::optional<ServiceReport>, 5> integrations_{};
    std::optional<StartupService> portal_blocker_;
    RecoveryRetry portal_retry_, pipewire_retry_;
    std::optional<Clock::time_point> appearance_deadline_, autostart_deadline_;
    Outcome outcome_;
    bool attaching_startup_ = true;
    bool applying_config_ = false;
    bool output_config_pending_ = false;
    bool idle_power_changed_ = false;
    bool config_error_visible_ = false;
    bool ipc_exiting_ = false;
    bool clean_exit_ = false;
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
    auto scene = seaside::Scene::create((*server)->display(), (*server)->compositor());
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
        .cursor_shape = Session::cursor_shape,
        .cursor_surface = Session::cursor_surface,
        .pointer_cancelled = Session::pointer_cancelled,
        .drag_icon = Session::drag_icon,
        .activity = Session::input_activity,
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
    (*scene)->set_linux_dmabuf((*server)->dmabuf());
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

int main(int argc, char **argv) try {
    if (argc == 6 && std::string_view(argv[1]) == "--activation-environment") {
        return seaside::run_session_environment(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::fputs("Usage: seaside [--help | --version]\n"
                   "Start Seaside as your normal user on the active Linux console.\n"
                   "Run seasidectl --help for session controls.\n",
                   stdout);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::printf("seaside %s (revision %s)\n", SEASIDE_BUILD_VERSION, SEASIDE_BUILD_REVISION);
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        std::fputs("Usage: seaside [--help | --version]\n", stderr);
        return EXIT_FAILURE;
    }
    if (!seaside_tty_active_console()) {
        std::fputs("seaside FAIL: start on the active Linux console as a foreground job\n", stderr);
        return EXIT_FAILURE;
    }
    const auto cursor_size = std::to_string(SEASIDE_DEFAULT_CURSOR_SIZE);
    if (setenv("XCURSOR_SIZE", cursor_size.c_str(), 0) != 0) {
        std::perror("seaside: could not set the session cursor size");
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

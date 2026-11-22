#include "session_activation.hpp"

#include "session_bus.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <sys/random.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr char manager_name[] = "org.freedesktop.systemd1";
constexpr char manager_path[] = "/org/freedesktop/systemd1";
constexpr char manager_interface[] = "org.freedesktop.systemd1.Manager";
constexpr char bus_name[] = "org.freedesktop.DBus";
constexpr char bus_path[] = "/org/freedesktop/DBus";
constexpr char unit_prefix[] = "/org/freedesktop/systemd1/unit";

struct Service {
    const char *suffix;
    const char *binary;
    const char *name;
    StartupService kind;
};
constexpr std::array services{
    Service{"wlr", "/usr/lib/xdg-desktop-portal-wlr", "org.freedesktop.impl.portal.desktop.wlr",
            StartupService::capture_backend},
    Service{"gtk", "/usr/lib/xdg-desktop-portal-gtk", "org.freedesktop.impl.portal.desktop.gtk",
            StartupService::capture_backend},
    Service{"frontend", "/usr/lib/xdg-desktop-portal", "org.freedesktop.portal.Desktop",
            StartupService::portal_frontend},
};
constexpr ServiceReport timeout_error{ServiceFailure::timeout, -ETIMEDOUT};
constexpr ServiceReport malformed{ServiceFailure::incompatible, -EBADMSG};

enum class Step { unowned, launch, owner, pid, unit };

using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;

bool valid_socket(std::string_view name) {
    return !name.empty() && name.size() <= 64 && name != "." && name != ".." &&
           name.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") ==
               std::string_view::npos;
}

std::string config_home_environment() {
    const auto *path = std::getenv("XDG_CONFIG_HOME");
    return path && path[0] == '/' ? std::string("XDG_CONFIG_HOME=") + path : std::string{};
}

bool is_missing(sd_bus_message *message) {
    const auto *error = message ? sd_bus_message_get_error(message) : nullptr;
    const auto *sender = message ? sd_bus_message_get_sender(message) : nullptr;
    return error && sender && std::string_view(sender) == bus_name &&
           (sd_bus_error_has_name(error, SD_BUS_ERROR_NAME_HAS_NO_OWNER) > 0 ||
            sd_bus_error_has_name(error, SD_BUS_ERROR_SERVICE_UNKNOWN) > 0);
}

bool unit_is_gone(const BusReply &reply) {
    const auto *error = reply.message ? sd_bus_message_get_error(reply.message) : nullptr;
    return error && (sd_bus_error_has_name(error, "org.freedesktop.systemd1.NoSuchUnit") > 0 ||
                     sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_OBJECT) > 0);
}

ServiceReport call_error(BusError code, int native = 0) {
    switch (code) {
    case BusError::timeout:
        return timeout_error;
    case BusError::disconnected:
    case BusError::stopped:
        return {ServiceFailure::disconnected, native};
    case BusError::remote_error:
        return {ServiceFailure::activation_failed, native};
    default:
        return {ServiceFailure::internal_error, native};
    }
}
} // namespace

struct PortalActivation::Impl final : std::enable_shared_from_this<Impl> {
    struct Unit {
        std::string name;
        std::string path;
        bool requested = false;
    };

    SessionBus *bus;
    std::array<Unit, services.size()> units{};
    std::string wayland;
    std::string bus_address;
    std::string config_home;
    Completion completion;
    Clock::time_point deadline;
    std::size_t stage = 0;
    std::optional<SessionBus::CallId> pending;
    wl_event_source *timer = nullptr;
    bool stopped = false;
    bool recovering = false;

    Impl(SessionBus &connection, std::string_view socket, Completion callback,
         std::chrono::milliseconds limit)
        : bus(&connection), wayland("WAYLAND_DISPLAY=" + std::string(socket)),
          bus_address("DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + std::to_string(getuid()) +
                      "/bus"),
          config_home(config_home_environment()), completion(std::move(callback)),
          deadline(Clock::now() + limit) {}

    ~Impl() { remove_timer(); }

    void remove_timer() noexcept {
        if (auto *source = std::exchange(timer, nullptr)) {
            wl_event_source_remove(source);
        }
    }

    ServiceReport prepare() {
        // Private unit names keep concurrent failed attempts from stopping each
        // other's processes. The bus names remain the official portal names.
        std::uint64_t nonce = 0;
        if (getrandom(&nonce, sizeof(nonce), GRND_NONBLOCK) != sizeof(nonce)) {
            return {ServiceFailure::internal_error, -EIO};
        }
        char id[17]{};
        std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(nonce));
        for (std::size_t index = 0; index < units.size(); ++index) {
            auto &unit = units[index];
            unit.name =
                std::string("seaside-portal-") + id + "-" + services[index].suffix + ".service";
            char *path = nullptr;
            const int encoded = sd_bus_path_encode(unit_prefix, unit.name.c_str(), &path);
            const std::unique_ptr<char, decltype(&free)> owned_path(path, free);
            if (encoded < 0) {
                return {ServiceFailure::internal_error, encoded};
            }
            unit.path = path;
        }
        return {};
    }

    static Message make(SessionBus &bus, bool manager, const char *method, int &error) {
        sd_bus_message *message = nullptr;
        error = sd_bus_message_new_method_call(
            bus.native_handle(), &message, manager ? manager_name : bus_name,
            manager ? manager_path : bus_path, manager ? manager_interface : bus_name, method);
        if (error >= 0) {
            error = sd_bus_message_set_allow_interactive_authorization(message, 0);
        }
        if (error >= 0) {
            error = sd_bus_message_set_auto_start(message, 0);
        }
        return Message(message, sd_bus_message_unref);
    }

    int append_launch(sd_bus_message *message, const Service &service) const {
        // The environment belongs to these unit invocations. A manager-wide
        // SetEnvironment cannot be restored atomically if another session starts.
        const int fixed = sd_bus_message_append(message, "ss", units[stage].name.c_str(), "fail");
        if (fixed < 0) {
            return fixed;
        }
        const int array = sd_bus_message_open_container(message, 'a', "(sv)");
        if (array < 0) {
            return array;
        }
        const int properties = sd_bus_message_append(
            message, "(sv)(sv)(sv)(sv)(sv)", "Type", "s", "exec", "CollectMode", "s",
            "inactive-or-failed", "TimeoutStopUSec", "t", static_cast<std::uint64_t>(1'000'000),
            "TimeoutStartUSec", "t", static_cast<std::uint64_t>(4'000'000), "KillMode", "s",
            "control-group");
        if (properties < 0) {
            return properties;
        }
        // An explicit XDG config directory also contains the external chooser's config.
        const int environment = sd_bus_message_append(
            message, "(sv)", "Environment", "as", config_home.empty() ? 5 : 6, wayland.c_str(),
            bus_address.c_str(), "XDG_CURRENT_DESKTOP=Seaside", "XDG_SESSION_TYPE=wayland",
            "GDK_BACKEND=wayland", config_home.c_str());
        if (environment < 0) {
            return environment;
        }
        const int exec =
            sd_bus_message_append(message, "(sv)(sv)", "UnsetEnvironment", "as", 1, "DISPLAY",
                                  "ExecStart", "a(sasb)", 1, service.binary, 1, service.binary, 0);
        if (exec < 0) {
            return exec;
        }
        const int end = sd_bus_message_close_container(message);
        if (end < 0) {
            return end;
        }
        return sd_bus_message_append(message, "a(sa(sv))", 0);
    }

    ServiceReport request(Step step, const char *owner = nullptr, std::uint32_t pid = 0) noexcept {
        try {
            constexpr std::array methods{"GetNameOwner", "StartTransientUnit", "GetNameOwner",
                                         "GetConnectionUnixProcessID", "GetUnitByPID"};
            const bool manager = step == Step::launch || step == Step::unit;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) {
                return timeout_error;
            }
            int error = 0;
            auto message = make(*bus, manager, methods[static_cast<std::size_t>(step)], error);
            if (error >= 0) {
                switch (step) {
                case Step::launch:
                    error = append_launch(message.get(), services[stage]);
                    break;
                case Step::pid:
                    error = sd_bus_message_append(message.get(), "s", owner);
                    break;
                case Step::unit:
                    error = sd_bus_message_append(message.get(), "u", pid);
                    break;
                default:
                    error = sd_bus_message_append(message.get(), "s", services[stage].name);
                    break;
                }
            }
            if (error < 0) {
                return {ServiceFailure::internal_error, error};
            }
            const auto call = bus->call(message.get(), remaining,
                                        [weak = weak_from_this(), step](const BusReply &reply) {
                                            if (auto self = weak.lock()) {
                                                self->received(step, reply);
                                            }
                                        });
            if (!call) {
                return call_error(call.error());
            }
            pending = *call;
            if (step == Step::launch) {
                units[stage].requested = true;
            }
            return {};
        } catch (...) {
            return {ServiceFailure::internal_error, -ENOMEM};
        }
    }

    void finish(StartupService service, ServiceReport report) {
        remove_timer();
        if (auto callback = std::exchange(completion, {})) {
            callback(service, report);
        }
    }

    static int poll_owner(void *data) noexcept {
        if (auto self = static_cast<Impl *>(data)->weak_from_this().lock()) {
            const auto result = self->request(Step::owner);
            if (result.failure != ServiceFailure::none) {
                // Nested dispatch during completion can free removed timer sources.
                const std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> current(
                    std::exchange(self->timer, nullptr), wl_event_source_remove);
                try {
                    self->finish(services[self->stage].kind, result);
                } catch (...) {
                }
            }
        }
        return 0;
    }

    ServiceReport wait_for_owner() noexcept {
        if (!timer) {
            timer = wl_event_loop_add_timer(bus->event_loop(), poll_owner, this);
            if (!timer) {
                return {ServiceFailure::internal_error, -errno};
            }
        }
        if (wl_event_source_timer_update(timer, 25) < 0) {
            return {ServiceFailure::internal_error, -errno};
        }
        return {};
    }

    void received(Step step, const BusReply &reply) {
        pending.reset();
        if (!completion) {
            return;
        }
        const auto &service = services[stage];
        if (Clock::now() >= deadline) {
            finish(service.kind, timeout_error);
            return;
        }
        ServiceReport result{};
        const char *value = nullptr;
        std::uint32_t pid = 0;
        if (reply.error != BusError::none) {
            if (reply.error == BusError::remote_error && is_missing(reply.message)) {
                if (step == Step::unowned) {
                    if (access(service.binary, X_OK) < 0) {
                        const int error = errno;
                        result = {error == ENOENT ? ServiceFailure::missing
                                                  : ServiceFailure::activation_failed,
                                  -error};
                    } else {
                        result = request(Step::launch);
                    }
                } else if (step == Step::owner) {
                    result = wait_for_owner();
                } else {
                    result = {ServiceFailure::disconnected, reply.native_error};
                }
            } else {
                result = call_error(reply.error, reply.native_error);
            }
        } else if (!reply.message) {
            result = malformed;
        } else {
            switch (step) {
            case Step::unowned:
                if (!recovering || !units[stage].requested) {
                    result = {ServiceFailure::wrong_environment, -EADDRINUSE};
                } else {
                    result = sd_bus_message_read(reply.message, "s", &value) > 0 && value[0] == ':'
                                 ? request(Step::pid, value)
                                 : malformed;
                }
                break;
            case Step::launch:
                result = sd_bus_message_read(reply.message, "o", &value) > 0 ? request(Step::owner)
                                                                             : malformed;
                break;
            case Step::owner:
                result = sd_bus_message_read(reply.message, "s", &value) > 0 && value[0] == ':'
                             ? request(Step::pid, value)
                             : malformed;
                break;
            case Step::pid:
                result = sd_bus_message_read(reply.message, "u", &pid) > 0 && pid > 1
                             ? request(Step::unit, nullptr, pid)
                             : malformed;
                break;
            case Step::unit:
                if (sd_bus_message_read(reply.message, "o", &value) <= 0) {
                    result = malformed;
                } else if (units[stage].path != value) {
                    result = {ServiceFailure::wrong_environment, -EPERM};
                } else if (stage + 1 == services.size()) {
                    finish(StartupService::capture_backend, {});
                    return;
                } else {
                    ++stage;
                    result = request(Step::unowned);
                }
                break;
            }
        }
        if (result.failure != ServiceFailure::none) {
            finish(services[stage].kind, result);
        }
    }

    bool drain(Clock::time_point limit, const unsigned &remaining) noexcept {
        while (remaining && Clock::now() < limit) {
            if (wl_event_loop_dispatch(bus->event_loop(), 25) < 0 && errno != EINTR) {
                return false;
            }
        }
        return remaining == 0;
    }

    bool confirm_stop(const Unit &unit, Clock::time_point limit) {
        std::array<char, 32> last_state{};
        while (Clock::now() < limit) {
            sd_bus_message *raw = nullptr;
            int error = sd_bus_message_new_method_call(bus->native_handle(), &raw, manager_name,
                                                       unit.path.c_str(),
                                                       "org.freedesktop.DBus.Properties", "Get");
            const Message message(raw, sd_bus_message_unref);
            if (error >= 0) {
                error = sd_bus_message_set_auto_start(raw, 0);
            }
            if (error >= 0) {
                error = sd_bus_message_append(raw, "ss", "org.freedesktop.systemd1.Unit",
                                              "ActiveState");
            }
            if (error < 0) {
                std::fprintf(stderr, "seaside: portal %s could not build ActiveState query (%d)\n",
                             unit.name.c_str(), error);
                return false;
            }
            unsigned remaining = 1;
            bool inactive = false;
            bool pending_stop = false;
            const auto duration =
                std::chrono::duration_cast<std::chrono::microseconds>(limit - Clock::now());
            if (duration.count() <= 0) {
                break;
            }
            auto call = bus->call(raw, duration, [&](const BusReply &reply) {
                remaining = 0;
                // systemd may collect the unit as soon as its stop job completes.
                inactive = unit_is_gone(reply);
                if (inactive) {
                    return;
                }
                const char *state = nullptr;
                if (reply.error != BusError::none) {
                    const auto *remote =
                        reply.message ? sd_bus_message_get_error(reply.message) : nullptr;
                    std::fprintf(stderr, "seaside: portal %s ActiveState query failed (%s, %d)\n",
                                 unit.name.c_str(), remote && remote->name ? remote->name : "D-Bus",
                                 reply.native_error);
                } else if (reply.message &&
                           sd_bus_message_read(reply.message, "v", "s", &state) > 0) {
                    const std::string_view value(state);
                    std::snprintf(last_state.data(), last_state.size(), "%s", state);
                    inactive = value == "inactive" || value == "failed";
                    pending_stop = !inactive;
                } else {
                    std::fprintf(stderr, "seaside: portal %s returned malformed ActiveState\n",
                                 unit.name.c_str());
                }
            });
            if (!call) {
                std::fprintf(stderr, "seaside: portal %s could not queue ActiveState query (%d)\n",
                             unit.name.c_str(), static_cast<int>(call.error()));
                return false;
            }
            if (!drain(limit, remaining)) {
                (void)bus->cancel(*call);
                break;
            }
            if (inactive || !pending_stop) {
                return inactive;
            }
            // A StopUnit reply admits the job. Inspect the unit itself before
            // releasing the display and returning to the TTY.
            if (wl_event_loop_dispatch(bus->event_loop(), 25) < 0 && errno != EINTR) {
                std::fprintf(stderr, "seaside: portal %s could not poll ActiveState (%d)\n",
                             unit.name.c_str(), errno);
                return false;
            }
        }
        std::fprintf(stderr, "seaside: portal %s could not confirm stop (last ActiveState=%s)\n",
                     unit.name.c_str(), last_state[0] == '\0' ? "unknown" : last_state.data());
        return false;
    }

    bool stop() noexcept {
        if (std::exchange(stopped, true)) {
            return true;
        }
        remove_timer();
        completion = {};
        if (auto id = std::exchange(pending, std::nullopt)) {
            (void)bus->cancel(*id);
        }
        std::array<SessionBus::CallId, services.size()> calls{};
        unsigned remaining = 0;
        const auto limit = Clock::now() + 2s;
        for (std::size_t index = units.size(); index-- > 0;) {
            if (!units[index].requested) {
                continue;
            }
            try {
                int error = 0;
                auto message = make(*bus, true, "StopUnit", error);
                if (error >= 0) {
                    error = sd_bus_message_append(message.get(), "ss", units[index].name.c_str(),
                                                  "replace");
                }
                if (error < 0) {
                    continue;
                }
                auto call = bus->call(message.get(), 1500ms,
                                      [&remaining](const BusReply &) { --remaining; });
                if (!call) {
                    continue;
                }
                calls[index] = *call;
                ++remaining;
            } catch (...) {
                // Still verify every unit after sending all possible stop requests.
            }
        }
        (void)drain(limit, remaining);
        for (const auto id : calls) {
            if (id) {
                (void)bus->cancel(id);
            }
        }
        // StopUnit can race with unit collection or lose its reply. Only the
        // resulting state of each unit proves whether it is still running.
        bool ok = true;
        for (const auto &unit : units) {
            if (unit.requested) {
                try {
                    ok &= confirm_stop(unit, limit);
                } catch (...) {
                    std::fprintf(stderr, "seaside: portal %s could not verify shutdown\n",
                                 unit.name.c_str());
                    ok = false;
                }
            }
        }
        return ok;
    }
};

PortalActivation::PortalActivation(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<PortalActivation>, ServiceReport>
PortalActivation::start(SessionBus &bus, std::string_view socket, Completion completion,
                        std::chrono::milliseconds timeout) noexcept {
    if (!completion || !bus.event_loop() || !valid_socket(socket) || timeout <= 0ms ||
        timeout > 60s) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -EINVAL});
    }
    try {
        auto impl = std::make_shared<Impl>(bus, socket, std::move(completion), timeout);
        auto activation = std::unique_ptr<PortalActivation>(new PortalActivation(impl));
        if (const auto result = impl->prepare(); result.failure != ServiceFailure::none) {
            return std::unexpected(result);
        }
        if (const auto result = impl->request(Step::unowned);
            result.failure != ServiceFailure::none) {
            return std::unexpected(result);
        }
        return activation;
    } catch (...) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -ENOMEM});
    }
}

PortalActivation::~PortalActivation() noexcept {
    (void)stop();
}

std::expected<void, ServiceReport>
PortalActivation::recover(SessionBus &bus, Completion completion,
                          std::chrono::milliseconds timeout) noexcept {
    const auto &impl = *impl_;
    if (impl.stopped || impl.completion || impl.pending || !completion || !bus.event_loop() ||
        timeout <= 0ms || timeout > 60s) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -EINVAL});
    }
    impl_->bus = &bus;
    impl_->completion = std::move(completion);
    impl_->deadline = Clock::now() + timeout;
    impl_->stage = 0;
    impl_->recovering = true;
    if (const auto result = impl_->request(Step::unowned); result.failure != ServiceFailure::none) {
        impl_->completion = {};
        return std::unexpected(result);
    }
    return {};
}

bool PortalActivation::stop() noexcept {
    const auto impl = impl_;
    return impl->stop();
}

} // namespace seaside

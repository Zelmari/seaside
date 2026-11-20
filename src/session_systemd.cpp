#include "session_systemd.hpp"

#include "session_bus.hpp"

#include <cerrno>
#include <cstdint>
#include <optional>
#include <string_view>
#include <systemd/sd-bus.h>
#include <utility>

namespace seaside {
namespace {
using Clock = std::chrono::steady_clock;
constexpr char systemd_name[] = "org.freedesktop.systemd1";
constexpr char systemd_path[] = "/org/freedesktop/systemd1";
constexpr char systemd_manager[] = "org.freedesktop.systemd1.Manager";
constexpr char bus_name[] = "org.freedesktop.DBus";
constexpr char bus_path[] = "/org/freedesktop/DBus";
constexpr ServiceReport malformed{ServiceFailure::incompatible, -EBADMSG};
constexpr ServiceReport expired{ServiceFailure::timeout, -ETIMEDOUT};

ServiceReport transport_failure(BusError error) noexcept {
    switch (error) {
    case BusError::allocation:
        return {ServiceFailure::internal_error, -ENOMEM};
    case BusError::disconnected:
    case BusError::stopped:
        return {ServiceFailure::disconnected, -ENOTCONN};
    case BusError::full:
        return {ServiceFailure::internal_error, -ENOBUFS};
    case BusError::invalid_arguments:
        return {ServiceFailure::internal_error, -EINVAL};
    case BusError::timeout:
        return expired;
    case BusError::wrong_thread:
        return {ServiceFailure::internal_error, -EPERM};
    default:
        return {ServiceFailure::internal_error, -EIO};
    }
}

ServiceReport reply_failure(const BusReply &reply, bool checking_owner) noexcept {
    if (reply.error != BusError::remote_error) {
        auto result = transport_failure(reply.error);
        if (reply.native_error < 0) {
            result.native_error = reply.native_error;
        }
        return result;
    }

    const auto *error = reply.message ? sd_bus_message_get_error(reply.message) : nullptr;
    const std::string_view name = error && error->name ? error->name : "";
    if (name == SD_BUS_ERROR_SERVICE_UNKNOWN || name == SD_BUS_ERROR_NAME_HAS_NO_OWNER) {
        return {checking_owner ? ServiceFailure::disconnected : ServiceFailure::missing,
                reply.native_error};
    }
    if (name == SD_BUS_ERROR_UNKNOWN_METHOD || name == SD_BUS_ERROR_UNKNOWN_OBJECT ||
        name == SD_BUS_ERROR_UNKNOWN_INTERFACE || name == SD_BUS_ERROR_UNKNOWN_PROPERTY ||
        name == SD_BUS_ERROR_NOT_SUPPORTED || name == SD_BUS_ERROR_INVALID_SIGNATURE) {
        return {ServiceFailure::incompatible, reply.native_error};
    }
    if (name == SD_BUS_ERROR_TIMED_OUT) {
        return expired;
    }
    return {ServiceFailure::activation_failed, reply.native_error};
}

ServiceReport read_credentials(sd_bus_message *message, uid_t expected_uid) noexcept {
    const auto *sender = sd_bus_message_get_sender(message);
    if (!sender || std::string_view(sender) != bus_name ||
        sd_bus_message_has_signature(message, "a{sv}") <= 0 ||
        sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
        return malformed;
    }
    std::optional<std::uint32_t> uid;
    std::optional<std::uint32_t> pid;
    int entry = 0;
    while ((entry = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *key = nullptr;
        if (sd_bus_message_read(message, "s", &key) <= 0) {
            return malformed;
        }
        const std::string_view field(key);
        if (field == "UnixUserID" || field == "ProcessID") {
            std::uint32_t value = 0;
            auto &credential = field == "UnixUserID" ? uid : pid;
            if (credential || sd_bus_message_read(message, "v", "u", &value) <= 0) {
                return malformed;
            }
            credential = value;
        } else if (sd_bus_message_skip(message, "v") <= 0) {
            return malformed;
        }
        if (sd_bus_message_exit_container(message) < 0) {
            return malformed;
        }
    }
    if (entry < 0 || sd_bus_message_exit_container(message) < 0 || !uid || !pid || *pid == 0) {
        return malformed;
    }
    // The Arch host manager runs as PID 1, including when both managers use UID 0.
    if (*uid != expected_uid || *pid == 1) {
        return {ServiceFailure::wrong_environment, -EPERM};
    }
    return {};
}
} // namespace

struct SystemdUserProbe::Impl final : std::enable_shared_from_this<Impl> {
    Impl(SessionBus &transport, uid_t uid, Completion callback, Clock::time_point limit) noexcept
        : bus(transport), expected_uid(uid), completion(std::move(callback)), deadline(limit) {}

    SessionBus &bus;
    const uid_t expected_uid;
    Completion completion;
    const Clock::time_point deadline;
    std::optional<SessionBus::CallId> pending;

    ServiceReport request(const char *reply_owner = nullptr) noexcept {
        sd_bus *connection = bus.native_handle();
        if (!connection) {
            return transport_failure(bus.last_error());
        }
        const bool checking_owner = reply_owner != nullptr;
        sd_bus_message *raw = nullptr;
        int result =
            checking_owner
                ? sd_bus_message_new_method_call(connection, &raw, bus_name, bus_path, bus_name,
                                                 "GetConnectionCredentials")
                : sd_bus_message_new_method_call(connection, &raw, systemd_name, systemd_path,
                                                 "org.freedesktop.DBus.Properties", "Get");
        std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)> message(
            raw, sd_bus_message_unref);
        if (result >= 0) {
            result = sd_bus_message_set_allow_interactive_authorization(raw, 0);
        }
        if (result >= 0) {
            // The session manager is already expected to own its name. Inspect
            // the running manager without requesting service activation.
            result = sd_bus_message_set_auto_start(raw, 0);
        }
        if (result >= 0) {
            result = checking_owner ? sd_bus_message_append(raw, "s", reply_owner)
                                    : sd_bus_message_append(raw, "ss", systemd_manager, "Version");
        }
        if (result < 0) {
            return {result == -ENOTCONN ? ServiceFailure::disconnected
                                        : ServiceFailure::internal_error,
                    result};
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            return expired;
        }
        try {
            auto call = bus.call(raw, remaining,
                                 [weak = weak_from_this(), checking_owner](const BusReply &reply) {
                                     if (auto self = weak.lock()) {
                                         self->receive(reply, checking_owner);
                                     }
                                 });
            if (!call) {
                return transport_failure(call.error());
            }
            pending = *call;
            return {};
        } catch (...) {
            return {ServiceFailure::internal_error, -ENOMEM};
        }
    }

    void finish(ServiceReport report) {
        if (auto callback = std::exchange(completion, {})) {
            callback(report);
        }
    }

    void receive(const BusReply &reply, bool checking_owner) {
        pending.reset();
        if (Clock::now() >= deadline) {
            finish(expired);
            return;
        }
        if (reply.error != BusError::none) {
            finish(reply_failure(reply, checking_owner));
            return;
        }
        if (!reply.message) {
            finish(malformed);
            return;
        }
        if (checking_owner) {
            finish(read_credentials(reply.message, expected_uid));
            return;
        }
        const char *version = nullptr;
        const auto *sender = sd_bus_message_get_sender(reply.message);
        if (!sender || sender[0] != ':' || sd_bus_service_name_is_valid(sender) <= 0 ||
            sd_bus_message_has_signature(reply.message, "v") <= 0 ||
            sd_bus_message_read(reply.message, "v", "s", &version) <= 0 || !*version) {
            finish(malformed);
            return;
        }
        // A well-known name can change hands. Ask the bus about the unique
        // connection that actually returned the manager property.
        const auto next = request(sender);
        if (next.failure != ServiceFailure::none) {
            finish(next);
        }
    }

    void abandon() noexcept {
        auto id = std::exchange(pending, std::nullopt);
        completion = {};
        if (id) {
            (void)bus.cancel(*id);
        }
    }
};

SystemdUserProbe::SystemdUserProbe(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<SystemdUserProbe>, ServiceReport>
SystemdUserProbe::start(SessionBus &bus, uid_t expected_uid, Completion completion,
                        std::chrono::milliseconds timeout) noexcept {
    if (!completion || expected_uid == static_cast<uid_t>(-1) ||
        timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(10)) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -EINVAL});
    }
    try {
        auto impl = std::make_shared<Impl>(bus, expected_uid, std::move(completion),
                                           Clock::now() + timeout);
        auto probe = std::unique_ptr<SystemdUserProbe>(new SystemdUserProbe(impl));
        const auto result = impl->request();
        if (result.failure != ServiceFailure::none) {
            return std::unexpected(result);
        }
        return probe;
    } catch (...) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -ENOMEM});
    }
}

SystemdUserProbe::~SystemdUserProbe() noexcept {
    auto impl = impl_;
    impl->abandon();
}

} // namespace seaside

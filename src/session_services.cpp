#include "session_services.hpp"

#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace seaside {
const char *service_result_name(ServiceFailure failure) noexcept {
    switch (failure) {
    case ServiceFailure::none:
        return "PASS";
    case ServiceFailure::missing:
        return "missing";
    case ServiceFailure::activation_failed:
        return "activation-failed";
    case ServiceFailure::wrong_environment:
        return "wrong-environment";
    case ServiceFailure::incompatible:
        return "incompatible";
    case ServiceFailure::timeout:
        return "timeout";
    case ServiceFailure::disconnected:
        return "disconnected";
    case ServiceFailure::internal_error:
        return "internal-error";
    }
    return "internal-error";
}

static ServiceReport pipewire_failure(int error) noexcept {
    switch (error) {
    case -ENOENT:
    case -EHOSTDOWN:
        return {ServiceFailure::missing, error};
    case -EACCES:
    case -EPERM:
    case -EINVAL:
        return {ServiceFailure::wrong_environment, error};
    case -ENOTCONN:
    case -ECONNREFUSED:
    case -ECONNRESET:
    case -EPIPE:
        return {ServiceFailure::disconnected, error};
    case -ETIMEDOUT:
        return {ServiceFailure::timeout, error};
    default:
        return {ServiceFailure::activation_failed, error};
    }
}

std::expected<std::unique_ptr<SessionBus>, ServiceReport> open_login_bus(wl_event_loop *loop,
                                                                         uid_t uid) noexcept try {
    const auto path = "/run/user/" + std::to_string(uid) + "/bus";
    struct stat socket{};
    if (lstat(path.c_str(), &socket) < 0) {
        const int error = errno;
        return std::unexpected(ServiceReport{
            error == ENOENT ? ServiceFailure::missing : ServiceFailure::wrong_environment, -error});
    }
    if (!S_ISSOCK(socket.st_mode) || socket.st_uid != uid) {
        return std::unexpected(ServiceReport{ServiceFailure::wrong_environment, 0});
    }
    auto bus = SessionBus::connect(loop, "unix:path=" + path);
    if (!bus) {
        return std::unexpected(ServiceReport{ServiceFailure::disconnected, 0});
    }
    return std::move(*bus);
} catch (...) {
    return std::unexpected(ServiceReport{ServiceFailure::internal_error, -ENOMEM});
}

const char *startup_service_name(StartupService service) noexcept {
    switch (service) {
    case StartupService::systemd_user:
        return "systemd";
    case StartupService::pipewire:
        return "pipewire";
    case StartupService::portal_frontend:
        return "portal-frontend";
    case StartupService::capture_backend:
        return "capture-backend";
    }
    return "unknown-service";
}

ServiceReport pipewire_service_report(PipeWireResult result) noexcept {
    switch (result.status) {
    case PipeWireStatus::ready:
        return {};
    case PipeWireStatus::timed_out:
        return {ServiceFailure::timeout, result.system_error};
    default:
        return pipewire_failure(result.system_error);
    }
}

ServiceReport pipewire_service_report(PipeWireError error) noexcept {
    return error.code == PipeWireFailure::connection
               ? pipewire_failure(error.system_error)
               : ServiceReport{ServiceFailure::internal_error, error.system_error};
}
} // namespace seaside

#include "session_bus.hpp"
#include "session_pipewire.hpp"
#include "session_systemd.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using seaside::ServiceFailure;
using seaside::ServiceReport;

const char *result_name(ServiceFailure failure) {
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

ServiceReport pipewire_failure(int error) {
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

std::expected<std::unique_ptr<seaside::SessionBus>, ServiceReport>
open_user_bus(wl_event_loop *loop, uid_t uid) {
    // Arch's login bus endpoint keeps this developer check independent of
    // ambient bus variables and prevents accidentally querying the system bus.
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
    auto bus = seaside::SessionBus::connect(loop, "unix:path=" + path);
    if (!bus) {
        return std::unexpected(ServiceReport{ServiceFailure::disconnected, 0});
    }
    return std::move(*bus);
}
} // namespace

int main() {
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> loop(wl_event_loop_create(),
                                                                          wl_event_loop_destroy);
    if (!loop) {
        std::fputs("session core FAIL: event loop allocation\n", stderr);
        return EXIT_FAILURE;
    }
    constexpr auto budget = 10s;
    const auto deadline = Clock::now() + budget;
    const auto before_deadline = [deadline] { return Clock::now() < deadline; };
    const uid_t uid = getuid();
    std::optional<ServiceReport> systemd_result, pipewire_result;
    auto bus = open_user_bus(loop.get(), uid);
    std::unique_ptr<seaside::SystemdUserProbe> systemd;
    if (!bus) {
        systemd_result = bus.error();
    } else {
        auto probe = seaside::SystemdUserProbe::start(
            **bus, uid,
            [&systemd_result, before_deadline](ServiceReport report) {
                if (before_deadline()) {
                    systemd_result = report;
                }
            },
            budget);
        if (probe) {
            systemd = std::move(*probe);
        } else {
            systemd_result = probe.error();
        }
    }
    auto pipewire = seaside::PipeWireProbe::create(
        loop.get(), {.timeout = budget},
        [&pipewire_result, before_deadline](seaside::PipeWireResult result) {
            if (!before_deadline()) {
                return;
            }
            if (result.status == seaside::PipeWireStatus::ready) {
                pipewire_result = ServiceReport{};
            } else if (result.status == seaside::PipeWireStatus::timed_out) {
                pipewire_result = ServiceReport{ServiceFailure::timeout, result.system_error};
            } else {
                pipewire_result = pipewire_failure(result.system_error);
            }
        });
    if (!pipewire) {
        const auto error = pipewire.error();
        pipewire_result = error.code == seaside::PipeWireFailure::connection
                              ? pipewire_failure(error.system_error)
                              : ServiceReport{ServiceFailure::internal_error, error.system_error};
    }

    while (!systemd_result || !pipewire_result) {
        const auto now = Clock::now();
        if (now >= deadline) {
            break;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        if (wl_event_loop_dispatch(loop.get(), static_cast<int>(remaining.count())) < 0) {
            const int error = errno;
            const ServiceReport broken{ServiceFailure::internal_error, -error};
            systemd_result = systemd_result.value_or(broken);
            pipewire_result = pipewire_result.value_or(broken);
        }
    }
    const ServiceReport timeout{ServiceFailure::timeout, -ETIMEDOUT};
    const auto sd = systemd_result.value_or(timeout);
    const auto pw = pipewire_result.value_or(timeout);
    std::printf("session core systemd=%s(%d) pipewire=%s(%d)\n", result_name(sd.failure),
                sd.native_error, result_name(pw.failure), pw.native_error);
    return sd.failure == ServiceFailure::none && pw.failure == ServiceFailure::none ? EXIT_SUCCESS
                                                                                    : EXIT_FAILURE;
}

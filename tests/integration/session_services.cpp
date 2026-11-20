#include "session_services.hpp"
#include "session_systemd.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using seaside::ServiceFailure;
using seaside::ServiceReport;

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
    auto bus = seaside::open_login_bus(loop.get(), uid);
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
            pipewire_result = seaside::pipewire_service_report(result);
        });
    if (!pipewire) {
        pipewire_result = seaside::pipewire_service_report(pipewire.error());
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
    std::printf("session core systemd=%s(%d) pipewire=%s(%d)\n",
                seaside::service_result_name(sd.failure), sd.native_error,
                seaside::service_result_name(pw.failure), pw.native_error);
    return sd.failure == ServiceFailure::none && pw.failure == ServiceFailure::none ? EXIT_SUCCESS
                                                                                    : EXIT_FAILURE;
}

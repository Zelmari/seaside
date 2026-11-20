#ifndef SEASIDE_STARTUP_GATE_HPP
#define SEASIDE_STARTUP_GATE_HPP

#include <array>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

struct wl_event_loop;

namespace seaside {

enum class StartupService { systemd_user, pipewire, portal_frontend, capture_backend };
enum class ServiceFailure {
    none,
    missing,
    activation_failed,
    wrong_environment,
    incompatible,
    timeout,
    disconnected,
    internal_error,
};
struct ServiceReport {
    ServiceFailure failure = ServiceFailure::none;
    int native_error = 0;
};
enum class StartupPhase { checking, ready, failed, cancelled };
struct StartupResult {
    StartupPhase phase = StartupPhase::checking;
    std::optional<StartupService> service;
    ServiceReport detail;
};
enum class StartupError { invalid_arguments, allocation, system, wrong_thread, stopped, duplicate };

// This coordinates results; only the actual service probes can establish them.
// The owning loop must outlive the gate. All operations and probe reports belong
// to that loop's thread; destroying the gate suppresses outstanding delivery.
class StartupGate final {
  public:
    using Completion = std::function<void(StartupResult)>;
    static std::expected<std::unique_ptr<StartupGate>, StartupError>
    create(wl_event_loop *loop, Completion completion,
           std::chrono::milliseconds timeout = std::chrono::seconds(5)) noexcept;
    ~StartupGate() noexcept;
    StartupGate(const StartupGate &) = delete;
    StartupGate &operator=(const StartupGate &) = delete;

    // A success means the named mandatory probe really passed, not that its
    // package exists. Duplicate, terminal and invalid reports cannot change it.
    // Completion is deferred, including when every probe answers synchronously.
    // An expired deadline rejects the report and queues failure. A system error
    // means delivery could not be queued: the caller must abort directly rather
    // than wait for a callback that cannot be dispatched.
    std::expected<void, StartupError> report(StartupService service,
                                             ServiceReport result = {}) noexcept;
    // Cancellation is silent and suppresses an already queued completion too.
    std::expected<void, StartupError> cancel() noexcept;
    StartupResult result() const noexcept;
    std::array<std::optional<ServiceReport>, 4> reports() const noexcept;

  private:
    struct Impl;
    explicit StartupGate(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};
} // namespace seaside
#endif

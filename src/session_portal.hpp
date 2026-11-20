#ifndef SEASIDE_SESSION_PORTAL_HPP
#define SEASIDE_SESSION_PORTAL_HPP

#include "startup_gate.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <sys/types.h>

namespace seaside {

class SessionBus;

class PortalProbe final {
  public:
    using BindingCheck = std::function<bool(pid_t, uid_t)>;
    using Completion = std::function<void(StartupService, ServiceReport)>;

    // The bus must outlive the probe. The binding check must inspect live
    // resources on Seaside's Wayland loop for the supplied bus credentials.
    // Completion reports both passes or the first failing service. The probe
    // creates and closes an unstarted session, without selecting or capturing.
    // With runtime_fault installed, retain the probe to report loss of either
    // checked owner once. Reporting a loss leaves the session running.
    static std::expected<std::unique_ptr<PortalProbe>, ServiceReport>
    start(SessionBus &bus, uid_t expected_uid, BindingCheck bound, Completion completion,
          std::chrono::milliseconds timeout, Completion runtime_fault = {}) noexcept;
    ~PortalProbe() noexcept;
    PortalProbe(const PortalProbe &) = delete;
    PortalProbe &operator=(const PortalProbe &) = delete;

  private:
    struct Impl;
    explicit PortalProbe(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace seaside
#endif

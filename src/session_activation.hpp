#ifndef SEASIDE_SESSION_ACTIVATION_HPP
#define SEASIDE_SESSION_ACTIVATION_HPP

#include "startup_gate.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>

namespace seaside {
class SessionBus;

// The bus and Wayland loop outlive the activation and its orderly stop.
class PortalActivation final {
  public:
    using Completion = std::function<void(StartupService, ServiceReport)>;

    static std::expected<std::unique_ptr<PortalActivation>, ServiceReport>
    start(SessionBus &bus, std::string_view socket, Completion completion,
          std::chrono::milliseconds timeout) noexcept;

    ~PortalActivation() noexcept;
    PortalActivation(const PortalActivation &) = delete;
    PortalActivation &operator=(const PortalActivation &) = delete;

    // Recheck ownership and asynchronously restart missing owned units.
    // A replacement bus must outlive this object even if admission fails.
    std::expected<void, ServiceReport> recover(SessionBus &bus, Completion completion,
                                               std::chrono::milliseconds timeout) noexcept;

    // Abandon work on a disconnected bus, preserving ownership of the private units.
    void replace_bus(SessionBus &replacement) noexcept;

    // Call outside dispatch, after cancelling other portal requests.
    bool stop() noexcept;

  private:
    struct Impl;
    explicit PortalActivation(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};
} // namespace seaside
#endif

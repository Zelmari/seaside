#ifndef SEASIDE_SESSION_SYSTEMD_HPP
#define SEASIDE_SESSION_SYSTEMD_HPP

#include "startup_gate.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <sys/types.h>

namespace seaside {

class SessionBus;

// The bus must outlive the probe. Start, completion and destruction belong to
// its owner loop. Destroying the probe silently cancels its outstanding call.
class SystemdUserProbe final {
  public:
    using Completion = std::function<void(ServiceReport)>;

    static std::expected<std::unique_ptr<SystemdUserProbe>, ServiceReport>
    start(SessionBus &bus, uid_t expected_uid, Completion completion,
          std::chrono::milliseconds timeout = std::chrono::seconds(10)) noexcept;
    ~SystemdUserProbe() noexcept;
    SystemdUserProbe(const SystemdUserProbe &) = delete;
    SystemdUserProbe &operator=(const SystemdUserProbe &) = delete;

  private:
    struct Impl;
    explicit SystemdUserProbe(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace seaside
#endif

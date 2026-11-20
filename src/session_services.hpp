#ifndef SEASIDE_SESSION_SERVICES_HPP
#define SEASIDE_SESSION_SERVICES_HPP

#include "session_bus.hpp"
#include "session_pipewire.hpp"
#include "startup_gate.hpp"

#include <sys/types.h>

namespace seaside {
const char *service_result_name(ServiceFailure failure) noexcept;
const char *startup_service_name(StartupService service) noexcept;
ServiceReport pipewire_service_report(PipeWireResult result) noexcept;
ServiceReport pipewire_service_report(PipeWireError error) noexcept;

// Check the standard login socket, independent of ambient session-bus overrides.
std::expected<std::unique_ptr<SessionBus>, ServiceReport> open_login_bus(wl_event_loop *loop,
                                                                         uid_t uid) noexcept;
} // namespace seaside

#endif

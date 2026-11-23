#ifndef SEASIDE_SESSION_BUS_HPP
#define SEASIDE_SESSION_BUS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string_view>

struct sd_bus;
struct sd_bus_message;
struct wl_event_loop;

namespace seaside {

enum class BusError {
    none,
    invalid_arguments,
    allocation,
    system,
    wrong_thread,
    full,
    stopped,
    not_found,
    remote_error,
    timeout,
    disconnected,
};

// The message is borrowed only for the callback. A remote error may still carry
// a message whose sd_bus_message_get_error() identifies the service failure.
struct BusReply {
    sd_bus_message *message = nullptr;
    BusError error = BusError::none;
    int native_error = 0;
};

// All operations and destruction belong to the borrowed loop's owning thread;
// the loop must outlive this adapter. An address is supplied explicitly: this
// transport does not activate services or modify a user's session environment.
class SessionBus final {
  public:
    struct Limits {
        std::size_t max_pending_calls = 16;
        std::size_t dispatch_budget = 32;
    };
    using CallId = std::uint64_t;
    using Callback = std::function<void(const BusReply &)>;

    static std::expected<std::unique_ptr<SessionBus>, BusError>
    connect(wl_event_loop *loop, std::string_view address, Limits limits) noexcept;
    static std::expected<std::unique_ptr<SessionBus>, BusError>
    connect(wl_event_loop *loop, std::string_view address) noexcept {
        return connect(loop, address, Limits{});
    }
    ~SessionBus() noexcept;
    SessionBus(const SessionBus &) = delete;
    SessionBus &operator=(const SessionBus &) = delete;

    // Construction initiates authentication without waiting for the daemon.
    // This borrowed handle supports typed message creation, not synchronous I/O.
    sd_bus *native_handle() const noexcept;
    // The borrowed event loop is available on its owning thread.
    wl_event_loop *event_loop() const noexcept;
    bool ready() const noexcept;
    BusError last_error() const noexcept;
    std::size_t pending() const noexcept;
    std::size_t callback_failures() const noexcept;
    // Rearm write events after emitting a signal through the borrowed handle.
    bool refresh_events() noexcept;

    // The caller retains its message reference; sd-bus takes any reference it
    // needs after successful admission. Timeout is finite and must be positive.
    // Normal delivery is deferred and budgeted. If both the wake FD and timer
    // become unusable, the transport closes and already admitted failures drain
    // synchronously (at most max_pending_calls) outside sd_bus_process. A call
    // rejected during that fatal path gets no callback. Such callbacks may
    // cancel other calls or destroy this owner before the operation returns.
    std::expected<CallId, BusError> call(sd_bus_message *message, std::chrono::microseconds timeout,
                                         Callback callback) noexcept;
    // Cancellation and shutdown suppress callbacks, including already received
    // replies not yet delivered. Callbacks may cancel, submit or destroy owner.
    std::expected<void, BusError> cancel(CallId id) noexcept;
    std::expected<void, BusError> finish() noexcept;

  private:
    struct Impl;
    explicit SessionBus(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace seaside
#endif

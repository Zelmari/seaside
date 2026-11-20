#ifndef SEASIDE_SESSION_PIPEWIRE_HPP
#define SEASIDE_SESSION_PIPEWIRE_HPP

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>

struct wl_event_loop;

namespace seaside {

enum class PipeWireStatus { pending, ready, failed, timed_out, cancelled };

struct PipeWireResult {
    PipeWireStatus status = PipeWireStatus::pending;
    int system_error = 0;
    bool callback_failed = false;
};

enum class PipeWireFailure {
    invalid_arguments,
    allocation,
    loop_creation,
    context_creation,
    connection,
    event_source,
    wrong_thread,
};

struct PipeWireError {
    PipeWireFailure code;
    int system_error = 0;
};

// A single core roundtrip, not a playback/capture or whole-session health check.
// Create/use/destroy on the owning Wayland thread; the borrowed event loop must
// outlive the probe. Resource construction may load the system client modules;
// server communication itself is dispatched without waiting by the Wayland loop.
class PipeWireProbe final {
  public:
    struct Options {
        std::chrono::milliseconds timeout{3000};
        // Empty selects PipeWire's normal remote and socket activation. Explicit
        // paths let isolated tests avoid connecting to the user's actual server.
        std::string remote{};
    };
    using Completion = std::function<void(PipeWireResult)>;

    static std::expected<std::unique_ptr<PipeWireProbe>, PipeWireError>
    create(wl_event_loop *loop, Options options, Completion completion) noexcept;
    ~PipeWireProbe() noexcept;
    PipeWireProbe(const PipeWireProbe &) = delete;
    PipeWireProbe &operator=(const PipeWireProbe &) = delete;

    // Callback delivery is deferred outside PipeWire dispatch, at most once.
    // If deferred scheduling fails, explicit cancel may deliver its failure
    // before returning. A throwing completion is recorded in result().
    // Destroying the probe from its completion is supported; destroying a
    // pending probe silently cancels delivery. Explicit cancel delivers cancelled.
    std::expected<void, PipeWireError> cancel() noexcept;
    PipeWireResult result() const noexcept;

  private:
    struct Impl;
    explicit PipeWireProbe(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace seaside
#endif

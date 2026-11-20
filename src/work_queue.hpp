#ifndef SEASIDE_WORK_QUEUE_HPP
#define SEASIDE_WORK_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>

struct wl_event_loop;

namespace seaside {

enum class WorkError {
    none,
    invalid_limits,
    invalid_job,
    allocation,
    system,
    full,
    stopped,
    wrong_thread,
    cancelled,
    job_failed,
    result_too_large
};

class WorkLifetime {
  public:
    void invalidate() noexcept { active_.store(false); }
    bool active() const noexcept { return active_.load(); }

  private:
    std::atomic<bool> active_{true};
};

struct WorkResult {
    std::shared_ptr<const void> value;
    std::size_t retained_bytes = 0;
    WorkError error = WorkError::none;
};

// The borrowed event loop must outlive the queue. Create, submit, change
// generation and destroy on the event-loop thread. Jobs receive owned inputs and may only return
// data; completions alone touch UI state.
class WorkQueue final {
  public:
    struct Limits {
        std::size_t workers = 2;
        std::size_t max_tasks = 8;
        // Two conservative 64 MiB parse reservations budget amplification of a
        // 1 MiB TOML file; task count alone would not bound retained results.
        std::size_t max_retained_bytes = 128 * 1024 * 1024;
        std::size_t dispatch_budget = 4;
    };
    struct Stats {
        std::size_t outstanding = 0;
        std::size_t reserved_bytes = 0;
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;
        std::uint64_t delivered = 0;
        std::uint64_t discarded = 0;
        std::uint64_t job_failures = 0;
        std::uint64_t callback_failures = 0;
    };
    using Job = std::function<WorkResult(std::stop_token)>;
    using Completion = std::function<void(WorkResult)>;

    static std::expected<std::unique_ptr<WorkQueue>, WorkError> create(wl_event_loop *loop,
                                                                       Limits limits) noexcept;
    static std::expected<std::unique_ptr<WorkQueue>, WorkError>
    create(wl_event_loop *loop) noexcept {
        return create(loop, Limits{});
    }
    ~WorkQueue() noexcept;
    WorkQueue(const WorkQueue &) = delete;
    WorkQueue &operator=(const WorkQueue &) = delete;

    // Reservation covers captured input plus the maximum retained result. The
    // caller bounds job allocations accordingly; arbitrary C++ closures cannot
    // be measured automatically. Charge persists through queued completion, not
    // merely until the worker finishes. Callback-owned results leave this budget.
    std::expected<void, WorkError> submit(std::uint64_t generation,
                                          std::weak_ptr<const WorkLifetime> lifetime,
                                          std::size_t reservation_bytes, Job job,
                                          Completion completion) noexcept;
    // Equal generations are harmless; regression is rejected so stale work
    // cannot become current again after a newer generation has been selected.
    std::expected<void, WorkError> set_generation(std::uint64_t generation) noexcept;
    std::expected<void, WorkError> shutdown() noexcept;
    Stats stats() const noexcept;

  private:
    struct Impl;
    explicit WorkQueue(std::shared_ptr<Impl> impl) noexcept;
    std::shared_ptr<Impl> impl_;
};

} // namespace seaside
#endif

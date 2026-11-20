#include "work_queue.hpp"

#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <new>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace seaside {

struct WorkQueue::Impl : std::enable_shared_from_this<WorkQueue::Impl> {
    struct Task {
        std::uint64_t generation;
        std::weak_ptr<const WorkLifetime> lifetime;
        std::size_t reservation;
        Job job;
        Completion completion;
    };
    struct Finished {
        Task task;
        WorkResult result;
    };

    explicit Impl(Limits selected) : limits(selected), owner(std::this_thread::get_id()) {}
    Limits limits;
    std::thread::id owner;
    std::mutex mutex;
    std::condition_variable_any available;
    std::vector<Task> pending;
    std::vector<Finished> finished;
    std::vector<std::jthread> workers;
    std::uint64_t generation = 0;
    Stats counters;
    bool stopping = false;
    int fd = -1;
    wl_event_source *source = nullptr;

    bool current(const Task &task) const noexcept {
        auto recipient = task.lifetime.lock();
        return task.generation == generation && recipient && recipient->active();
    }
    void release(const Task &task) noexcept {
        --counters.outstanding;
        counters.reserved_bytes -= task.reservation;
    }
    void notify() const noexcept {
        const std::uint64_t one = 1;
        while (write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
        // EAGAIN means the counter already has a pending wakeup.
    }
    void run(std::stop_token stop) noexcept {
        for (;;) {
            Task task;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, stop, [&] { return stopping || !pending.empty(); });
                if (stopping || stop.stop_requested()) {
                    return;
                }
                task = std::move(pending.front());
                pending.erase(pending.begin());
                if (!current(task)) {
                    release(task);
                    ++counters.discarded;
                    continue;
                }
            }
            WorkResult result;
            try {
                result = task.job(stop);
                if (result.retained_bytes > task.reservation) {
                    result = {
                        .value = {}, .retained_bytes = 0, .error = WorkError::result_too_large};
                }
            } catch (...) {
                result = {.value = {}, .retained_bytes = 0, .error = WorkError::job_failed};
            }
            {
                std::lock_guard lock(mutex);
                ++counters.completed;
                if (result.error != WorkError::none) {
                    ++counters.job_failures;
                }
                if (stopping || !current(task)) {
                    release(task);
                    ++counters.discarded;
                    continue;
                }
                // Both vectors reserve max_tasks before any worker starts.
                // A task consumes its slot until dispatch, so this cannot grow.
                finished.push_back({std::move(task), std::move(result)});
            }
            notify();
        }
    }
    static int dispatch(int event_fd, std::uint32_t, void *data) noexcept {
        std::uint64_t count = 0;
        while (read(event_fd, &count, sizeof(count)) < 0 && errno == EINTR) {
        }
        // A completion may destroy its queue. Keep dispatch state alive without
        // accessing the destroyed facade after returning from user code.
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        for (std::size_t i = 0; i < self->limits.dispatch_budget; ++i) {
            Finished completion;
            bool deliver = false;
            {
                std::lock_guard lock(self->mutex);
                if (self->stopping || self->finished.empty()) {
                    return 0;
                }
                completion = std::move(self->finished.front());
                self->finished.erase(self->finished.begin());
                deliver = self->current(completion.task);
                self->release(completion.task);
                if (deliver) {
                    ++self->counters.delivered;
                } else {
                    ++self->counters.discarded;
                }
            }
            if (deliver) {
                try {
                    completion.task.completion(std::move(completion.result));
                } catch (...) {
                    std::lock_guard lock(self->mutex);
                    ++self->counters.callback_failures;
                }
            }
        }
        {
            std::lock_guard lock(self->mutex);
            if (!self->stopping && !self->finished.empty()) {
                self->notify();
            }
        }
        return 0;
    }
    void stop() noexcept {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (const auto &task : pending) {
                release(task);
                ++counters.discarded;
            }
            for (const auto &completion : finished) {
                release(completion.task);
                ++counters.discarded;
            }
            pending.clear();
            finished.clear();
        }
        for (auto &worker : workers) {
            worker.request_stop();
        }
        available.notify_all();
        // Jobs must cooperate with cancellation; no detached worker can outlive
        // the notification descriptor or the state its lambda owns.
        workers.clear();
        if (source) {
            wl_event_source_remove(source);
            source = nullptr;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
};

WorkQueue::WorkQueue(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<WorkQueue>, WorkError> WorkQueue::create(wl_event_loop *loop,
                                                                       Limits limits) noexcept {
    if (!loop || limits.workers == 0 || limits.workers > 16 || limits.max_tasks == 0 ||
        limits.max_tasks > 1024 || limits.max_retained_bytes == 0 || limits.dispatch_budget == 0 ||
        limits.dispatch_budget > 1024) {
        return std::unexpected(WorkError::invalid_limits);
    }
    try {
        auto impl = std::make_shared<Impl>(limits);
        std::unique_ptr<WorkQueue> queue(new WorkQueue(impl));
        impl->pending.reserve(limits.max_tasks);
        impl->finished.reserve(limits.max_tasks);
        impl->workers.reserve(limits.workers);
        impl->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl->fd < 0) {
            return std::unexpected(WorkError::system);
        }
        impl->source =
            wl_event_loop_add_fd(loop, impl->fd, WL_EVENT_READABLE, Impl::dispatch, impl.get());
        if (!impl->source) {
            return std::unexpected(WorkError::system);
        }
        for (std::size_t i = 0; i < limits.workers; ++i) {
            impl->workers.emplace_back([impl](std::stop_token stop) { impl->run(stop); });
        }
        return queue;
    } catch (const std::bad_alloc &) {
        return std::unexpected(WorkError::allocation);
    } catch (...) {
        return std::unexpected(WorkError::system);
    }
}

WorkQueue::~WorkQueue() noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        std::terminate();
    }
    impl_->stop();
}

std::expected<void, WorkError> WorkQueue::submit(std::uint64_t generation,
                                                 std::weak_ptr<const WorkLifetime> lifetime,
                                                 std::size_t reservation_bytes, Job job,
                                                 Completion completion) noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return std::unexpected(WorkError::wrong_thread);
    }
    if (!job || !completion || reservation_bytes == 0) {
        return std::unexpected(WorkError::invalid_job);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
        return std::unexpected(WorkError::stopped);
    }
    if (impl_->counters.outstanding >= impl_->limits.max_tasks ||
        reservation_bytes > impl_->limits.max_retained_bytes - impl_->counters.reserved_bytes) {
        return std::unexpected(WorkError::full);
    }
    impl_->pending.push_back({generation, std::move(lifetime), reservation_bytes, std::move(job),
                              std::move(completion)});
    ++impl_->counters.outstanding;
    impl_->counters.reserved_bytes += reservation_bytes;
    ++impl_->counters.submitted;
    impl_->available.notify_one();
    return {};
}

std::expected<void, WorkError> WorkQueue::set_generation(std::uint64_t generation) noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return std::unexpected(WorkError::wrong_thread);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) {
        return std::unexpected(WorkError::stopped);
    }
    if (generation < impl_->generation) {
        return std::unexpected(WorkError::invalid_job);
    }
    impl_->generation = generation;
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();) {
        if (!impl_->current(*it)) {
            impl_->release(*it);
            ++impl_->counters.discarded;
            it = impl_->pending.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = impl_->finished.begin(); it != impl_->finished.end();) {
        if (!impl_->current(it->task)) {
            impl_->release(it->task);
            ++impl_->counters.discarded;
            it = impl_->finished.erase(it);
        } else {
            ++it;
        }
    }
    return {};
}

std::expected<void, WorkError> WorkQueue::shutdown() noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return std::unexpected(WorkError::wrong_thread);
    }
    impl_->stop();
    return {};
}

WorkQueue::Stats WorkQueue::stats() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

} // namespace seaside

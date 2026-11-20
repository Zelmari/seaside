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
        std::shared_ptr<const std::atomic<bool>> active;
        std::size_t reservation;
        std::size_t slot;
        Job job;
        WorkResult result;
    };

    explicit Impl(Limits selected) : limits(selected), owner(std::this_thread::get_id()) {}
    Limits limits;
    std::thread::id owner;
    std::mutex mutex;
    std::condition_variable_any available;
    std::vector<std::unique_ptr<Task>> pending;
    std::vector<std::unique_ptr<Task>> finished;
    // Only the owner thread accesses these slots. Workers carry an index, never
    // a completion whose captures may own event-loop-affine objects.
    std::vector<Completion> callbacks;
    std::vector<std::jthread> workers;
    std::uint64_t generation = 0;
    Stats counters;
    bool stopping = false;
    int fd = -1;
    wl_event_source *source = nullptr;

    bool current(const Task &task) const noexcept {
        return task.generation == generation && !task.lifetime.expired() && task.active &&
               task.active->load();
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
    static std::unique_ptr<Task> take(std::vector<std::unique_ptr<Task>> &tasks,
                                      std::size_t index) noexcept {
        auto task = std::move(tasks[index]);
        // Erasing moved-from unique_ptrs cannot destroy a task under the mutex.
        tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(index));
        return task;
    }
    void retire(std::unique_ptr<Task> task, bool invoke) noexcept {
        bool deliver = false;
        {
            std::lock_guard lock(mutex);
            deliver = invoke && !stopping && current(*task);
            release(*task);
            if (deliver) {
                ++counters.delivered;
            } else {
                ++counters.discarded;
            }
        }
        Completion callback;
        callback.swap(callbacks[task->slot]);
        if (deliver) {
            try {
                callback(std::move(task->result));
            } catch (...) {
                std::lock_guard lock(mutex);
                ++counters.callback_failures;
            }
        }
        // Both the task and completion captures are destroyed on the owner,
        // outside the mutex. Their destructors may reenter stats()/submission.
    }
    void run(std::stop_token stop) noexcept {
        for (;;) {
            std::unique_ptr<Task> task;
            bool execute = false;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, stop, [&] { return stopping || !pending.empty(); });
                if (stopping || stop.stop_requested()) {
                    return;
                }
                task = take(pending, 0);
                execute = current(*task);
            }
            if (execute) {
                try {
                    task->result = task->job(stop);
                    if (task->result.retained_bytes > task->reservation) {
                        task->result = {
                            .value = {}, .retained_bytes = 0, .error = WorkError::result_too_large};
                    }
                } catch (...) {
                    task->result = {
                        .value = {}, .retained_bytes = 0, .error = WorkError::job_failed};
                }
            }
            {
                std::lock_guard lock(mutex);
                if (execute) {
                    ++counters.completed;
                    if (task->result.error != WorkError::none) {
                        ++counters.job_failures;
                    }
                }
                // Reservations include completed/discarded work until the owner
                // retires it. Capacity was reserved before any worker started.
                finished.push_back(std::move(task));
            }
            notify();
        }
    }
    static int dispatch(int event_fd, std::uint32_t, void *data) noexcept {
        std::uint64_t count = 0;
        while (read(event_fd, &count, sizeof(count)) < 0 && errno == EINTR) {
        }
        // A completion may destroy its queue. Hold state independently until
        // this callback returns, without touching the destroyed facade.
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        for (std::size_t i = 0; i < self->limits.dispatch_budget; ++i) {
            std::unique_ptr<Task> task;
            {
                std::lock_guard lock(self->mutex);
                if (self->stopping || self->finished.empty()) {
                    return 0;
                }
                task = take(self->finished, 0);
            }
            self->retire(std::move(task), true);
        }
        {
            std::lock_guard lock(self->mutex);
            if (!self->stopping && !self->finished.empty()) {
                self->notify();
            }
        }
        return 0;
    }
    void purge_stale() noexcept {
        for (;;) {
            std::unique_ptr<Task> task;
            {
                std::lock_guard lock(mutex);
                for (auto *tasks : {&pending, &finished}) {
                    for (std::size_t i = 0; i < tasks->size(); ++i) {
                        if (!current(*(*tasks)[i])) {
                            task = take(*tasks, i);
                            break;
                        }
                    }
                    if (task) {
                        break;
                    }
                }
            }
            if (!task) {
                return;
            }
            retire(std::move(task), false);
        }
    }
    void stop() noexcept {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            stopping = true;
        }
        for (auto &worker : workers) {
            worker.request_stop();
        }
        available.notify_all();
        // No detached job can outlive its descriptor. Stale/inflight work still
        // publishes its task record; only the owner destroys completion slots.
        workers.clear();
        for (;;) {
            std::unique_ptr<Task> task;
            {
                std::lock_guard lock(mutex);
                if (!pending.empty()) {
                    task = take(pending, 0);
                } else if (!finished.empty()) {
                    task = take(finished, 0);
                }
            }
            if (!task) {
                break;
            }
            retire(std::move(task), false);
        }
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
        impl->callbacks.resize(limits.max_tasks);
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
    auto impl = impl_;
    auto recipient = lifetime.lock();
    auto active = recipient ? recipient->active_ : nullptr;
    std::unique_ptr<Impl::Task> task;
    try {
        task = std::make_unique<Impl::Task>(Impl::Task{generation,
                                                       std::move(lifetime),
                                                       std::move(active),
                                                       reservation_bytes,
                                                       0,
                                                       std::move(job),
                                                       {}});
    } catch (const std::bad_alloc &) {
        return std::unexpected(WorkError::allocation);
    }
    {
        std::lock_guard lock(impl->mutex);
        if (impl->stopping) {
            return std::unexpected(WorkError::stopped);
        }
        if (impl->counters.outstanding >= impl->limits.max_tasks ||
            reservation_bytes > impl->limits.max_retained_bytes - impl->counters.reserved_bytes) {
            return std::unexpected(WorkError::full);
        }
        for (std::size_t i = 0; i < impl->callbacks.size(); ++i) {
            if (!impl->callbacks[i]) {
                task->slot = i;
                impl->callbacks[i].swap(completion);
                break;
            }
        }
        impl->pending.push_back(std::move(task));
        ++impl->counters.outstanding;
        impl->counters.reserved_bytes += reservation_bytes;
        ++impl->counters.submitted;
    }
    impl->available.notify_one();
    return {};
}

std::expected<void, WorkError> WorkQueue::set_generation(std::uint64_t generation) noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return std::unexpected(WorkError::wrong_thread);
    }
    auto impl = impl_;
    {
        std::lock_guard lock(impl->mutex);
        if (impl->stopping) {
            return std::unexpected(WorkError::stopped);
        }
        if (generation < impl->generation) {
            return std::unexpected(WorkError::invalid_job);
        }
        impl->generation = generation;
    }
    impl->purge_stale();
    return {};
}

std::expected<void, WorkError> WorkQueue::shutdown() noexcept {
    if (std::this_thread::get_id() != impl_->owner) {
        return std::unexpected(WorkError::wrong_thread);
    }
    auto impl = impl_;
    impl->stop();
    return {};
}

WorkQueue::Stats WorkQueue::stats() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return impl_->counters;
}

} // namespace seaside

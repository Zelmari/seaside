#include "session_bus.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
std::uint64_t monotonic_usec() noexcept {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000U +
           static_cast<std::uint64_t>(now.tv_nsec) / 1000U;
}

BusError transport_error(int code) noexcept {
    if (code == -ENOMEM) {
        return BusError::allocation;
    }
    if (code == -EINVAL) {
        return BusError::invalid_arguments;
    }
    if (code == -ENOTCONN || code == -ECONNRESET || code == -EPIPE) {
        return BusError::disconnected;
    }
    return BusError::system;
}
} // namespace

struct SessionBus::Impl : std::enable_shared_from_this<SessionBus::Impl> {
    struct Pending {
        CallId id = 0;
        sd_bus_slot *slot = nullptr;
        sd_bus_message *reply = nullptr;
        Callback callback;
        std::uint64_t deadline = 0;
        BusError error = BusError::none;
        int native_error = 0;
        bool completed = false;
        ~Pending() noexcept {
            sd_bus_slot_unref(slot);
            sd_bus_message_unref(reply);
        }
    };

    wl_event_loop *loop;
    Limits limits;
    const std::thread::id owner = std::this_thread::get_id();
    sd_bus *bus = nullptr;
    wl_event_source *fd_source = nullptr;
    wl_event_source *timer_source = nullptr;
    wl_event_source *wake_source = nullptr;
    int wake_fd = -1;
    int watched_fd = -1;
    std::map<CallId, std::unique_ptr<Pending>> calls;
    CallId next_id = 1;
    BusError error = BusError::none;
    std::size_t callback_errors = 0;
    bool stopped = false;
    bool dispatching = false;

    Impl(wl_event_loop *event_loop, Limits bounds) noexcept : loop(event_loop), limits(bounds) {}
    ~Impl() noexcept { stop(); }

    bool owns_thread() const noexcept { return owner == std::this_thread::get_id(); }

    void remove_sources() noexcept {
        for (auto **source : {&wake_source, &fd_source, &timer_source}) {
            if (*source) {
                wl_event_source_remove(*source);
                *source = nullptr;
            }
        }
        watched_fd = -1;
        if (wake_fd >= 0) {
            close(wake_fd);
            wake_fd = -1;
        }
    }

    void stop() noexcept {
        if (stopped) {
            return;
        }
        stopped = true;
        remove_sources();
        // Captured deleters may reenter the adapter. Publish the empty/stopped
        // state before retiring them, and never flush a bus during shutdown.
        std::map<CallId, std::unique_ptr<Pending>> retired;
        retired.swap(calls);
        bus = sd_bus_close_unref(bus);
    }

    void fail(BusError failure, int native) noexcept {
        if (stopped || error != BusError::none) {
            return;
        }
        error = failure;
        if (fd_source) {
            wl_event_source_remove(fd_source);
            fd_source = nullptr;
        }
        watched_fd = -1;
        for (auto &[id, pending] : calls) {
            (void)id;
            if (!pending->completed) {
                pending->completed = true;
                pending->error = failure;
                pending->native_error = native;
            }
        }
        sd_bus_close(bus);
    }

    static int receive(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &pending = *static_cast<Pending *>(data);
        // User code runs only after sd_bus_process has unwound. A completion can
        // destroy SessionBus without invalidating sd-bus's callback context.
        if (!pending.completed) {
            pending.completed = true;
            if (monotonic_usec() >= pending.deadline) {
                pending.error = BusError::timeout;
                pending.native_error = -ETIMEDOUT;
                return 1;
            }
            pending.reply = sd_bus_message_ref(message);
            if (sd_bus_message_is_method_error(message, nullptr) > 0) {
                const auto *remote = sd_bus_message_get_error(message);
                pending.native_error = -sd_bus_message_get_errno(message);
                if (sd_bus_error_has_name(remote, SD_BUS_ERROR_NO_REPLY) > 0 ||
                    sd_bus_error_has_name(remote, SD_BUS_ERROR_TIMEOUT) > 0) {
                    pending.error = BusError::timeout;
                } else if (sd_bus_error_has_name(remote, SD_BUS_ERROR_DISCONNECTED) > 0) {
                    pending.error = BusError::disconnected;
                } else {
                    pending.error = BusError::remote_error;
                }
            }
        }
        return 1;
    }

    static int on_fd(int, std::uint32_t, void *data) noexcept {
        if (auto keep_alive = static_cast<Impl *>(data)->weak_from_this().lock()) {
            keep_alive->dispatch();
        }
        return 0;
    }
    static int on_timer(void *data) noexcept {
        if (auto keep_alive = static_cast<Impl *>(data)->weak_from_this().lock()) {
            keep_alive->dispatch();
        }
        return 0;
    }
    static int on_wake(int fd, std::uint32_t, void *data) noexcept {
        std::uint64_t counter = 0;
        while (read(fd, &counter, sizeof(counter)) < 0 && errno == EINTR) {
        }
        if (auto keep_alive = static_cast<Impl *>(data)->weak_from_this().lock()) {
            keep_alive->dispatch();
        }
        return 0;
    }

    bool wake() noexcept {
        if (stopped) {
            return true;
        }
        const std::uint64_t counter = 1;
        ssize_t result = 0;
        do {
            result = write(wake_fd, &counter, sizeof(counter));
        } while (result < 0 && errno == EINTR);
        return result == sizeof(counter) || (result < 0 && errno == EAGAIN);
    }

    bool defer() noexcept {
        if (wake()) {
            return true;
        }
        const int native = -errno;
        fail(BusError::system, native);
        // The wake channel is no longer trustworthy. Existing timer storage
        // can still deliver failures in bounded turns without allocating a new
        // source or waiting for an unrelated long method deadline.
        return timer_source && wl_event_source_timer_update(timer_source, 1) >= 0;
    }

    bool rearm() noexcept {
        if (stopped || error != BusError::none) {
            return true;
        }
        const int fd = sd_bus_get_fd(bus);
        const int events = sd_bus_get_events(bus);
        std::uint64_t timeout = std::numeric_limits<std::uint64_t>::max();
        const int timeout_result = sd_bus_get_timeout(bus, &timeout);
        if (fd < 0 || events < 0 || timeout_result < 0) {
            const int native = std::min({fd, events, timeout_result});
            fail(transport_error(native), native);
            return false;
        }
        std::uint32_t mask = 0;
        if ((events & POLLIN) != 0) {
            mask |= WL_EVENT_READABLE;
        }
        if ((events & POLLOUT) != 0) {
            mask |= WL_EVENT_WRITABLE;
        }
        if (watched_fd != fd) {
            if (fd_source) {
                wl_event_source_remove(fd_source);
            }
            fd_source = wl_event_loop_add_fd(loop, fd, mask, on_fd, this);
            watched_fd = fd;
            if (!fd_source) {
                fail(BusError::system, -errno);
                return false;
            }
        } else if (wl_event_source_fd_update(fd_source, mask) < 0) {
            fail(BusError::system, -errno);
            return false;
        }
        for (const auto &[id, pending] : calls) {
            (void)id;
            if (!pending->completed) {
                timeout = std::min(timeout, pending->deadline);
            }
        }
        int delay = 0;
        if (timeout != std::numeric_limits<std::uint64_t>::max()) {
            const auto now = monotonic_usec();
            if (now == 0) {
                fail(BusError::system, -errno);
                return false;
            }
            const auto remaining = timeout > now ? timeout - now : 0;
            // Wayland uses relative milliseconds and zero disarms the timer.
            const auto milliseconds = remaining / 1000U + (remaining % 1000U != 0 ? 1U : 0U);
            delay = static_cast<int>(std::clamp<std::uint64_t>(milliseconds, 1U, INT_MAX));
        }
        if (wl_event_source_timer_update(timer_source, delay) < 0) {
            fail(BusError::system, -errno);
            return false;
        }
        return true;
    }

    void deliver_ready(std::size_t budget) noexcept {
        std::size_t delivered = 0;
        while (!stopped && delivered < budget) {
            const auto found = std::find_if(calls.begin(), calls.end(), [](const auto &entry) {
                return entry.second->completed;
            });
            if (found == calls.end()) {
                break;
            }
            auto completion = std::move(found->second);
            calls.erase(found);
            // Retire the asynchronous slot before calling arbitrary code. The
            // reply reference remains owned by completion through callback exit.
            completion->slot = sd_bus_slot_unref(completion->slot);
            const BusReply reply{completion->reply, completion->error, completion->native_error};
            try {
                completion->callback(reply);
            } catch (...) {
                ++callback_errors;
            }
            ++delivered;
        }
    }

    void dispatch() noexcept {
        if (stopped || dispatching) {
            return;
        }
        dispatching = true;
        bool continuation = false;
        if (error == BusError::none) {
            std::size_t processed = 0;
            while (processed < limits.dispatch_budget) {
                const int result = sd_bus_process(bus, nullptr);
                if (result < 0) {
                    fail(transport_error(result), result);
                    break;
                }
                if (result == 0) {
                    break;
                }
                ++processed;
            }
            continuation = processed == limits.dispatch_budget;
            const auto now = monotonic_usec();
            for (auto &[id, pending] : calls) {
                (void)id;
                if (!pending->completed && pending->deadline <= now) {
                    pending->completed = true;
                    pending->error = BusError::timeout;
                    pending->native_error = -ETIMEDOUT;
                }
            }
        }
        deliver_ready(limits.dispatch_budget);
        if (!stopped) {
            const bool completed = std::any_of(calls.begin(), calls.end(), [](const auto &entry) {
                return entry.second->completed;
            });
            if (!rearm() && !calls.empty()) {
                continuation = true;
            }
            // New Wayland idle sources can run in the same idle drain. A wake
            // FD yields to other ready sources instead of recursively draining
            // every bus reply in one event-loop turn.
            if ((completed || continuation) && !defer()) {
                // Neither scheduling mechanism can deliver another turn. This
                // fatal-only drain is bounded by admission capacity and occurs
                // after sd-bus has unwound, preserving callback lifetime safety.
                deliver_ready(limits.max_pending_calls);
            }
        }
        dispatching = false;
    }
};

SessionBus::SessionBus(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<SessionBus>, BusError>
SessionBus::connect(wl_event_loop *loop, std::string_view address, Limits limits) noexcept {
    if (!loop || address.empty() || address.size() > 4096 || !address.starts_with("unix:") ||
        address.find('\0') != std::string_view::npos ||
        address.find(';') != std::string_view::npos || limits.max_pending_calls == 0 ||
        limits.max_pending_calls > 1024 || limits.dispatch_budget == 0 ||
        limits.dispatch_budget > 1024) {
        return std::unexpected(BusError::invalid_arguments);
    }
    try {
        auto impl = std::make_shared<Impl>(loop, limits);
        const std::string owned_address(address);
        int result = sd_bus_new(&impl->bus);
        if (result >= 0) {
            result = sd_bus_set_address(impl->bus, owned_address.c_str());
        }
        if (result >= 0) {
            result = sd_bus_set_bus_client(impl->bus, 1);
        }
        if (result >= 0) {
            result = sd_bus_set_exit_on_disconnect(impl->bus, 0);
        }
        if (result >= 0) {
            result = sd_bus_set_allow_interactive_authorization(impl->bus, 0);
        }
        if (result >= 0) {
            result = sd_bus_start(impl->bus);
        }
        if (result < 0) {
            return std::unexpected(transport_error(result));
        }
        impl->timer_source = wl_event_loop_add_timer(loop, Impl::on_timer, impl.get());
        impl->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl->wake_fd >= 0) {
            impl->wake_source = wl_event_loop_add_fd(loop, impl->wake_fd, WL_EVENT_READABLE,
                                                     Impl::on_wake, impl.get());
        }
        if (!impl->timer_source || !impl->wake_source || !impl->rearm() || !impl->wake()) {
            return std::unexpected(impl->error == BusError::none ? BusError::allocation
                                                                 : impl->error);
        }
        return std::unique_ptr<SessionBus>(new SessionBus(std::move(impl)));
    } catch (const std::bad_alloc &) {
        return std::unexpected(BusError::allocation);
    } catch (...) {
        return std::unexpected(BusError::system);
    }
}

SessionBus::~SessionBus() noexcept {
    impl_->stop();
}

sd_bus *SessionBus::native_handle() const noexcept {
    return impl_->owns_thread() && !impl_->stopped && impl_->error == BusError::none ? impl_->bus
                                                                                     : nullptr;
}

wl_event_loop *SessionBus::event_loop() const noexcept {
    return impl_->owns_thread() ? impl_->loop : nullptr;
}

bool SessionBus::ready() const noexcept {
    return native_handle() && sd_bus_is_ready(impl_->bus) > 0;
}

BusError SessionBus::last_error() const noexcept {
    return !impl_->owns_thread() ? BusError::wrong_thread
           : impl_->stopped      ? BusError::stopped
                                 : impl_->error;
}

std::size_t SessionBus::pending() const noexcept {
    return impl_->owns_thread() ? impl_->calls.size() : 0;
}

std::size_t SessionBus::callback_failures() const noexcept {
    return impl_->owns_thread() ? impl_->callback_errors : 0;
}

std::expected<SessionBus::CallId, BusError> SessionBus::call(sd_bus_message *message,
                                                             std::chrono::microseconds timeout,
                                                             Callback callback) noexcept {
    auto impl = impl_;
    if (!impl->owns_thread()) {
        return std::unexpected(BusError::wrong_thread);
    }
    if (impl->stopped) {
        return std::unexpected(BusError::stopped);
    }
    if (impl->error != BusError::none) {
        return std::unexpected(impl->error);
    }
    if (!message || !callback || timeout.count() <= 0 ||
        sd_bus_message_get_bus(message) != impl->bus) {
        return std::unexpected(BusError::invalid_arguments);
    }
    if (impl->calls.size() >= impl->limits.max_pending_calls ||
        impl->next_id == std::numeric_limits<CallId>::max()) {
        return std::unexpected(BusError::full);
    }
    const auto now = monotonic_usec();
    if (now == 0) {
        return std::unexpected(BusError::system);
    }
    const auto duration = static_cast<std::uint64_t>(timeout.count());
    const auto deadline = now > std::numeric_limits<std::uint64_t>::max() - duration
                              ? std::numeric_limits<std::uint64_t>::max()
                              : now + duration;
    try {
        auto pending = std::make_unique<Impl::Pending>();
        pending->id = impl->next_id++;
        pending->callback = std::move(callback);
        pending->deadline = deadline;
        const auto id = pending->id;
        auto *context = pending.get();
        impl->calls.emplace(id, std::move(pending));
        const int result =
            sd_bus_call_async(impl->bus, &context->slot, message, Impl::receive, context, duration);
        if (result < 0) {
            auto rejected = std::move(impl->calls.find(id)->second);
            impl->calls.erase(id);
            return std::unexpected(transport_error(result));
        }
        // A queued call may change write interests and its earliest deadline.
        // If rearming fails after admission, fail it asynchronously with peers.
        if (!impl->rearm() && !impl->defer()) {
            // Admission has not been acknowledged yet: reject this new call,
            // then notify older callers whose accepted work cannot be queued.
            auto rejected = std::move(impl->calls.find(id)->second);
            impl->calls.erase(id);
            impl->deliver_ready(impl->limits.max_pending_calls);
            return std::unexpected(impl->error);
        }
        return id;
    } catch (const std::bad_alloc &) {
        return std::unexpected(BusError::allocation);
    } catch (...) {
        return std::unexpected(BusError::system);
    }
}

std::expected<void, BusError> SessionBus::cancel(CallId id) noexcept {
    auto impl = impl_;
    if (!impl->owns_thread()) {
        return std::unexpected(BusError::wrong_thread);
    }
    if (impl->stopped) {
        return std::unexpected(BusError::stopped);
    }
    const auto found = impl->calls.find(id);
    if (found == impl->calls.end()) {
        return std::unexpected(BusError::not_found);
    }
    auto retired = std::move(found->second);
    impl->calls.erase(found);
    retired->slot = sd_bus_slot_unref(retired->slot);
    if (!impl->rearm() && !impl->calls.empty() && !impl->defer()) {
        impl->deliver_ready(impl->limits.max_pending_calls);
    }
    return {};
}

std::expected<void, BusError> SessionBus::finish() noexcept {
    auto impl = impl_;
    if (!impl->owns_thread()) {
        return std::unexpected(BusError::wrong_thread);
    }
    impl->stop();
    return {};
}

} // namespace seaside

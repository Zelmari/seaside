#include "startup_gate.hpp"

#include <algorithm>
#include <thread>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
struct StartupGate::Impl : std::enable_shared_from_this<Impl> {
    wl_event_loop *loop;
    const std::thread::id owner = std::this_thread::get_id();
    wl_event_source *deadline = nullptr;
    wl_event_source *delivery = nullptr;
    Completion completion;
    StartupResult outcome;
    std::array<std::optional<ServiceReport>, 4> checks{};
    bool closed = false;
    bool delivered = false;
    std::chrono::steady_clock::time_point deadline_at;

    Impl(wl_event_loop *event_loop, Completion callback, std::chrono::milliseconds timeout)
        : loop(event_loop), completion(std::move(callback)),
          deadline_at(std::chrono::steady_clock::now() + timeout) {}
    ~Impl() noexcept { close(); }

    void close() noexcept {
        closed = true;
        if (delivery) {
            wl_event_source_remove(delivery);
            delivery = nullptr;
        }
        if (deadline) {
            wl_event_source_remove(deadline);
            deadline = nullptr;
        }
        completion = {};
    }

    bool queue_delivery() noexcept {
        if (deadline) {
            wl_event_source_remove(deadline);
            deadline = nullptr;
        }
        delivery = wl_event_loop_add_idle(loop, deliver, this);
        if (!delivery) {
            outcome = {StartupPhase::failed, std::nullopt, {ServiceFailure::internal_error, 0}};
            return false;
        }
        return true;
    }

    static void deliver(void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return;
        }
        self->delivery = nullptr; // Wayland removes an idle source after dispatch.
        if (self->closed || self->delivered) {
            return;
        }
        self->delivered = true;
        auto callback = std::move(self->completion);
        try {
            // A callback may destroy its gate. The implementation stays alive
            // until this C boundary returns; no parent object is accessed here.
            callback(self->outcome);
        } catch (...) {
            self->outcome = {
                StartupPhase::failed, std::nullopt, {ServiceFailure::internal_error, 0}};
        }
    }

    static int expire(void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self || self->closed || self->outcome.phase != StartupPhase::checking) {
            return 0;
        }
        const auto pending = std::find_if(self->checks.begin(), self->checks.end(),
                                          [](const auto &value) { return !value.has_value(); });
        const auto service = static_cast<StartupService>(pending - self->checks.begin());
        self->outcome = {StartupPhase::failed, service, {ServiceFailure::timeout, 0}};
        if (!self->queue_delivery()) {
            // The deadline callback is already asynchronous. If allocating an
            // idle source fails, deliver the internal failure here rather than
            // leave a startup caller waiting forever for a removed deadline.
            deliver(self.get());
        }
        return 0;
    }
};

StartupGate::StartupGate(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
StartupGate::~StartupGate() noexcept {
    impl_->close();
}

std::expected<std::unique_ptr<StartupGate>, StartupError>
StartupGate::create(wl_event_loop *loop, Completion completion,
                    std::chrono::milliseconds timeout) noexcept {
    if (!loop || !completion || timeout.count() <= 0 || timeout > std::chrono::seconds(60)) {
        return std::unexpected(StartupError::invalid_arguments);
    }
    try {
        auto impl = std::make_shared<Impl>(loop, std::move(completion), timeout);
        impl->deadline = wl_event_loop_add_timer(loop, Impl::expire, impl.get());
        if (!impl->deadline ||
            wl_event_source_timer_update(impl->deadline, static_cast<int>(timeout.count())) < 0) {
            return std::unexpected(StartupError::system);
        }
        return std::unique_ptr<StartupGate>(new StartupGate(std::move(impl)));
    } catch (...) {
        return std::unexpected(StartupError::allocation);
    }
}

std::expected<void, StartupError> StartupGate::report(StartupService service,
                                                      ServiceReport result) noexcept {
    const auto self = impl_;
    if (self->owner != std::this_thread::get_id()) {
        return std::unexpected(StartupError::wrong_thread);
    }
    const auto index = static_cast<unsigned>(service);
    if (index >= self->checks.size() || static_cast<unsigned>(result.failure) >
                                            static_cast<unsigned>(ServiceFailure::internal_error)) {
        return std::unexpected(StartupError::invalid_arguments);
    }
    if (self->closed || self->outcome.phase != StartupPhase::checking) {
        return std::unexpected(StartupError::stopped);
    }
    if (std::chrono::steady_clock::now() >= self->deadline_at) {
        // An overdue Wayland timer may not have dispatched yet. A late service
        // reply must not remove it and retroactively turn timeout into success.
        const auto pending = std::find_if(self->checks.begin(), self->checks.end(),
                                          [](const auto &value) { return !value.has_value(); });
        self->outcome = {StartupPhase::failed,
                         static_cast<StartupService>(pending - self->checks.begin()),
                         {ServiceFailure::timeout, 0}};
        if (!self->queue_delivery()) {
            return std::unexpected(StartupError::system);
        }
        return std::unexpected(StartupError::stopped);
    }
    if (self->checks[index]) {
        return std::unexpected(StartupError::duplicate);
    }
    self->checks[index] = result;
    if (result.failure != ServiceFailure::none) {
        self->outcome = {StartupPhase::failed, service, result};
    } else if (std::all_of(self->checks.begin(), self->checks.end(),
                           [](const auto &value) { return value.has_value(); })) {
        self->outcome = {StartupPhase::ready, std::nullopt, {}};
    }
    if (self->outcome.phase != StartupPhase::checking && !self->queue_delivery()) {
        return std::unexpected(StartupError::system);
    }
    return {};
}

std::expected<void, StartupError> StartupGate::cancel() noexcept {
    const auto self = impl_;
    if (self->owner != std::this_thread::get_id()) {
        return std::unexpected(StartupError::wrong_thread);
    }
    if (self->closed || self->delivered) {
        return std::unexpected(StartupError::stopped);
    }
    self->outcome = {StartupPhase::cancelled, std::nullopt, {}};
    self->close();
    return {};
}

StartupResult StartupGate::result() const noexcept {
    return impl_->outcome;
}
std::array<std::optional<ServiceReport>, 4> StartupGate::reports() const noexcept {
    return impl_->checks;
}
} // namespace seaside

#include "idle_power.hpp"

#include "idle_inhibit.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
using namespace std::chrono_literals;
}

IdlePower::IdlePower(Power power, VisibleOutput visible_output, Fullscreen fullscreen, Now now)
    : power_(std::move(power)), visible_output_(std::move(visible_output)),
      fullscreen_(std::move(fullscreen)), now_(std::move(now)) {}

std::unique_ptr<IdlePower> IdlePower::create(wl_event_loop *loop, wl_display *display, Power power,
                                             VisibleOutput visible_output, Fullscreen fullscreen,
                                             Now now) {
    auto idle = std::unique_ptr<IdlePower>(new IdlePower(
        std::move(power), std::move(visible_output), std::move(fullscreen), std::move(now)));
    idle->timer_ = wl_event_loop_add_timer(loop, on_timer, idle.get());
    if (!idle->timer_) {
        return nullptr;
    }
    idle->inhibit_ = seaside_idle_inhibit_create(display, inhibitor_changed, idle.get());
    return idle->inhibit_ ? std::move(idle) : nullptr;
}

IdlePower::~IdlePower() {
    seaside_idle_inhibit_destroy(inhibit_);
    if (timer_) {
        wl_event_source_remove(timer_);
    }
}

bool IdlePower::add(wlr_output *output) noexcept try {
    const bool inserted = outputs_.try_emplace(output).second;
    visibility_changed();
    return inserted;
} catch (...) {
    return false;
}

void IdlePower::remove(wlr_output *output) noexcept {
    outputs_.erase(output);
    visibility_changed();
}

void IdlePower::set_timeout(std::chrono::minutes timeout) noexcept {
    if (timeout_ != timeout) {
        timeout_ = timeout;
        for (auto &[output, state] : outputs_) {
            state.retry.reset();
        }
        visibility_changed();
    }
}

void IdlePower::set_active(bool active) noexcept {
    if (active_ == active) {
        return;
    }
    active_ = active;
    if (!active_) {
        check_queued_ = false;
        arm(0);
        return;
    }
    last_activity_ = now_();
    for (auto &[output, state] : outputs_) {
        state.retry.reset();
    }
    visibility_changed();
}

void IdlePower::visibility_changed() noexcept {
    if (active_ && !check_queued_) {
        check_queued_ = true;
        arm(1);
    }
}

void IdlePower::activity() noexcept {
    if (!active_) {
        return;
    }
    const auto now = last_activity_ = now_();
    bool recheck = sleeping_;
    for (auto &[output, state] : outputs_) {
        if (state.retry &&
            (state.retry->sleep || (state.retry->exhausted && now >= state.retry->after))) {
            recheck |= state.retry->exhausted;
            state.retry.reset();
        }
        recheck |= state.sleeping;
    }
    if (recheck)
        visibility_changed();
}

void IdlePower::inhibitor_changed(void *data) noexcept {
    static_cast<IdlePower *>(data)->visibility_changed();
}

void IdlePower::visit_inhibitor(void *data, wlr_surface *surface) noexcept {
    auto &self = *static_cast<IdlePower *>(data);
    self.visible_inhibitor_ |= self.outputs_.contains(self.visible_output_(surface));
}

int IdlePower::on_timer(void *data) noexcept try {
    auto &self = *static_cast<IdlePower *>(data);
    self.check_queued_ = false;
    if (self.active_) {
        self.update();
    }
    return 0;
} catch (...) {
    std::fputs("seaside: could not recheck idle output power\n", stderr);
    return 0;
}

void IdlePower::update() {
    const auto now = now_();
    visible_inhibitor_ =
        std::ranges::any_of(outputs_, [&](const auto &entry) { return fullscreen_(entry.first); });
    // An inhibitor arriving after the seat sleeps takes effect once the seat wakes.
    if (!sleeping_ || timeout_ == 0min || now < last_activity_ + timeout_) {
        if (!visible_inhibitor_)
            seaside_idle_inhibit_each(inhibit_, visit_inhibitor, this);
        if (inhibited_ && !visible_inhibitor_)
            last_activity_ = now;
        if (inhibited_ != visible_inhibitor_) {
            inhibited_ = visible_inhibitor_;
            for (auto &[output, state] : outputs_)
                state.retry.reset();
        }
    }

    const auto expires = last_activity_ + timeout_;
    const bool counts_idle = timeout_ > 0min && !inhibited_;
    sleeping_ = counts_idle && now >= expires;
    auto next = counts_idle && !sleeping_ ? expires : Clock::time_point::max();
    for (auto &[output, state] : outputs_) {
        if (state.sleeping == sleeping_) {
            state.retry.reset();
            continue;
        }
        if (state.retry && state.retry->sleep != sleeping_) {
            state.retry.reset();
        }
        if (!state.retry || (!state.retry->exhausted && now >= state.retry->after)) {
            if (power_(output, !sleeping_)) {
                state.sleeping = sleeping_;
                state.retry.reset();
            } else {
                // Retry once in case the power commit raced a driver transition.
                // After that, fresh local input or policy can request another attempt.
                state.retry =
                    Retry{now + (state.retry ? 1s : 250ms), sleeping_, state.retry.has_value()};
            }
        }
        if (state.retry && !state.retry->exhausted) {
            next = std::min(next, state.retry->after);
        }
    }

    if (!check_queued_) {
        if (next == Clock::time_point::max()) {
            arm(0);
        } else {
            const auto delay = std::chrono::ceil<std::chrono::milliseconds>(next - now).count();
            arm(static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(
                delay, 1, std::numeric_limits<int>::max())));
        }
    }
}

void IdlePower::arm(int milliseconds) noexcept {
    if (wl_event_source_timer_update(timer_, milliseconds) < 0) {
        check_queued_ = false;
        std::fputs("seaside: could not schedule idle output power\n", stderr);
    }
}

} // namespace seaside

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
    const bool inserted = outputs_.try_emplace(output, Output{now_(), {}}).second;
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
    const auto now = now_();
    for (auto &[output, state] : outputs_) {
        state.last_activity = now;
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

void IdlePower::activity(wlr_output *output) noexcept {
    const auto found = outputs_.find(output);
    if (!active_ || found == outputs_.end()) {
        return;
    }
    auto &state = found->second;
    const auto now = now_();
    state.last_activity = now;
    if (state.retry &&
        (state.retry->sleep || (state.retry->exhausted && now >= state.retry->after))) {
        state.retry.reset();
    }
    if (state.sleeping) {
        visibility_changed();
    }
}

void IdlePower::inhibitor_changed(void *data) noexcept {
    static_cast<IdlePower *>(data)->visibility_changed();
}

void IdlePower::visit_inhibitor(void *data, wlr_surface *surface) noexcept {
    auto &self = *static_cast<IdlePower *>(data);
    const auto output = self.outputs_.find(self.visible_output_(surface));
    // The protocol prevents entering idle. A request made while a display is
    // already idle takes effect when that display wakes through another path.
    if (output != self.outputs_.end() && !output->second.sleeping) {
        output->second.visible_protection = true;
    }
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
    for (auto &[output, state] : outputs_) {
        state.visible_protection = fullscreen_(output);
    }
    seaside_idle_inhibit_each(inhibit_, visit_inhibitor, this);

    const auto now = now_();
    auto next = Clock::time_point::max();
    for (auto &[output, state] : outputs_) {
        if (state.inhibited != state.visible_protection) {
            state.inhibited = state.visible_protection;
            state.retry.reset();
            if (!state.inhibited) {
                // A finished film or a newly hidden inhibitor starts a new idle period.
                state.last_activity = now;
            }
        }
        const auto expires = state.last_activity + timeout_;
        const bool counts_idle = timeout_ > 0min && !state.inhibited;
        const bool sleep = counts_idle && now >= expires;
        if (counts_idle && expires > now) {
            next = std::min(next, expires);
        }
        if (state.sleeping == sleep) {
            state.retry.reset();
            continue;
        }
        if (state.retry && state.retry->sleep != sleep) {
            state.retry.reset();
        }
        if (!state.retry || (!state.retry->exhausted && now >= state.retry->after)) {
            if (power_(output, !sleep)) {
                state.sleeping = sleep;
                state.retry.reset();
            } else {
                // Retry once in case the power commit raced a driver transition.
                // After that, fresh local input or policy can request another attempt.
                state.retry =
                    Retry{now + (state.retry ? 1s : 250ms), sleep, state.retry.has_value()};
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

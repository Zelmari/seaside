#include "idle_power.hpp"

#include "wlr_compat.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
}

namespace seaside {
namespace {
using namespace std::chrono_literals;
}

struct IdlePower::Inhibitors {
    struct Watch {
        IdlePower *owner;
        wl_listener destroy, map, unmap;
        wl_list link;
    };

    IdlePower *owner;
    wlr_idle_inhibit_manager_v1 *manager = nullptr;
    wl_listener manager_destroy{}, new_inhibitor{};
    wl_list watches{};

    explicit Inhibitors(IdlePower &idle) noexcept : owner(&idle) { wl_list_init(&watches); }

    ~Inhibitors() { detach(); }

    static std::unique_ptr<Inhibitors> create(IdlePower &idle, wl_display *display) noexcept {
        static_assert(std::is_standard_layout_v<Inhibitors>);
        static_assert(std::is_standard_layout_v<Watch>);
        auto result = std::unique_ptr<Inhibitors>(new (std::nothrow) Inhibitors(idle));
        if (!result) {
            return nullptr;
        }
        result->manager = wlr_idle_inhibit_v1_create(display);
        if (!result->manager) {
            return nullptr;
        }
        result->new_inhibitor.notify = on_new_inhibitor;
        result->manager_destroy.notify = on_manager_destroy;
        wl_signal_add(&result->manager->events.new_inhibitor, &result->new_inhibitor);
        wl_signal_add(&result->manager->events.destroy, &result->manager_destroy);
        return result;
    }

    bool has_visible_surface() const {
        bool visible = false;
        if (manager) {
            const wlr_idle_inhibitor_v1 *item;
            wl_list_for_each(item, &manager->inhibitors, link) {
                visible |= owner->outputs_.contains(owner->visible_output_(item->surface));
            }
        }
        return visible;
    }

    void detach() noexcept {
        if (manager) {
            wl_list_remove(&new_inhibitor.link);
            wl_list_remove(&manager_destroy.link);
            manager = nullptr;
        }
        Watch *watch, *next;
        wl_list_for_each_safe(watch, next, &watches, link) {
            remove_watch(watch);
        }
    }

    static void remove_watch(Watch *watch) noexcept {
        wl_list_remove(&watch->destroy.link);
        wl_list_remove(&watch->map.link);
        wl_list_remove(&watch->unmap.link);
        wl_list_remove(&watch->link);
        delete watch;
    }

    static void on_inhibitor_destroy(wl_listener *listener, void *) noexcept {
        Watch *watch = wl_container_of(listener, watch, destroy);
        auto *idle = watch->owner;
        remove_watch(watch);
        // wlroots unlinks the inhibitor after delivering its destroy signal.
        // The timer walks the list after the current dispatch returns.
        idle->visibility_changed();
    }

    static void on_surface_map(wl_listener *listener, void *) noexcept {
        Watch *watch = wl_container_of(listener, watch, map);
        watch->owner->visibility_changed();
    }

    static void on_surface_unmap(wl_listener *listener, void *) noexcept {
        Watch *watch = wl_container_of(listener, watch, unmap);
        watch->owner->visibility_changed();
    }

    static void on_new_inhibitor(wl_listener *listener, void *data) noexcept {
        Inhibitors *inhibitors = wl_container_of(listener, inhibitors, new_inhibitor);
        auto *inhibitor = static_cast<wlr_idle_inhibitor_v1 *>(data);
        auto *watch = new (std::nothrow) Watch{};
        if (!watch) {
            wl_resource_post_no_memory(inhibitor->resource);
            return;
        }
        watch->owner = inhibitors->owner;
        watch->destroy.notify = on_inhibitor_destroy;
        watch->map.notify = on_surface_map;
        watch->unmap.notify = on_surface_unmap;
        wl_signal_add(&inhibitor->events.destroy, &watch->destroy);
        // A subsurface can change its mapped state without new desktop projection.
        wl_signal_add(&inhibitor->surface->events.map, &watch->map);
        wl_signal_add(&inhibitor->surface->events.unmap, &watch->unmap);
        wl_list_insert(&inhibitors->watches, &watch->link);
        watch->owner->visibility_changed();
    }

    static void on_manager_destroy(wl_listener *listener, void *) noexcept {
        Inhibitors *inhibitors = wl_container_of(listener, inhibitors, manager_destroy);
        inhibitors->detach();
    }
};

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
    idle->inhibitors_ = Inhibitors::create(*idle, display);
    return idle->inhibitors_ ? std::move(idle) : nullptr;
}

IdlePower::~IdlePower() {
    inhibitors_.reset();
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
    bool visible_inhibitor =
        std::ranges::any_of(outputs_, [&](const auto &entry) { return fullscreen_(entry.first); });
    // An inhibitor arriving after the seat sleeps takes effect once the seat wakes.
    if (!sleeping_ || timeout_ == 0min || now < last_activity_ + timeout_) {
        visible_inhibitor = visible_inhibitor || inhibitors_->has_visible_surface();
        if (inhibited_ && !visible_inhibitor)
            last_activity_ = now;
        if (inhibited_ != visible_inhibitor) {
            inhibited_ = visible_inhibitor;
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

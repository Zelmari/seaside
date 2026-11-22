#include "idle-inhibit-client-protocol.h"
#include "idle_power.hpp"
#include "wlroots_bridge.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

// The policy only borrows these identities. The power callback is the test's
// DRM boundary, while Wayland inhibitor creation and destruction are real.
struct wlr_output {};

namespace seaside {
namespace {
using namespace std::chrono_literals;

struct PowerChange {
    wlr_output *output;
    bool on;
    bool operator==(const PowerChange &) const = default;
};

class IdleTrial : public testing::Test {
  protected:
    void SetUp() override {
        server_ = wl_display_create();
        ASSERT_NE(server_, nullptr);
        loop_ = wl_display_get_event_loop(server_);
        ASSERT_NE(seaside_wlr_compositor_create(server_, nullptr), nullptr);
        idle_ = IdlePower::create(
            loop_, server_,
            [this](wlr_output *output, bool on) {
                changes_.push_back({output, on});
                if (output == rejection_target_ && remaining_rejections_ > 0) {
                    --remaining_rejections_;
                    return false;
                }
                return true;
            },
            [this](wlr_surface *surface) {
                EXPECT_NE(surface, nullptr);
                return inhibited_;
            },
            [this](wlr_output *output) {
                ++checks_;
                return fullscreen_ == output;
            },
            [this] { return now_; });
        ASSERT_NE(idle_, nullptr);
        ASSERT_TRUE(idle_->add(&left_));
        ASSERT_TRUE(idle_->add(&right_));
        idle_->set_active(true);
        check();
    }

    void TearDown() override {
        if (manager_) {
            zwp_idle_inhibit_manager_v1_destroy(manager_);
        }
        if (compositor_) {
            wl_compositor_destroy(compositor_);
        }
        if (registry_) {
            wl_registry_destroy(registry_);
        }
        if (client_) {
            wl_display_disconnect(client_);
        }
        idle_.reset();
        if (server_) {
            wl_display_destroy_clients(server_);
            wl_display_destroy(server_);
        }
    }

    void check() {
        const auto last = checks_;
        idle_->visibility_changed();
        const auto deadline = IdlePower::Clock::now() + 1s;
        while (checks_ == last && IdlePower::Clock::now() < deadline) {
            ASSERT_GE(wl_event_loop_dispatch(loop_, 10), 0);
        }
        ASSERT_GT(checks_, last);
    }

    void advance(IdlePower::Clock::duration duration) {
        now_ += duration;
        check();
    }

    static void on_global(void *data, wl_registry *registry, std::uint32_t name,
                          const char *interface, std::uint32_t) {
        auto &self = *static_cast<IdleTrial *>(data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
            self.compositor_ = static_cast<wl_compositor *>(
                wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        } else if (std::strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
            self.manager_ = static_cast<zwp_idle_inhibit_manager_v1 *>(
                wl_registry_bind(registry, name, &zwp_idle_inhibit_manager_v1_interface, 1));
        }
    }

    void connect_client() {
        int sockets[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        if (!wl_client_create(server_, sockets[0])) {
            close(sockets[0]);
            close(sockets[1]);
            FAIL() << "Could not create Wayland test client";
        }
        client_ = wl_display_connect_to_fd(sockets[1]);
        ASSERT_NE(client_, nullptr);
        registry_ = wl_display_get_registry(client_);
        ASSERT_NE(registry_, nullptr);
        static const wl_registry_listener listener{.global = on_global, .global_remove = nullptr};
        wl_registry_add_listener(registry_, &listener, this);
        roundtrip();
        ASSERT_NE(compositor_, nullptr);
        ASSERT_NE(manager_, nullptr);
    }

    void roundtrip() {
        static const wl_callback_listener listener{
            .done = [](void *data, wl_callback *, std::uint32_t) {
                *static_cast<bool *>(data) = true;
            }};
        bool done = false;
        auto *callback = wl_display_sync(client_);
        ASSERT_NE(callback, nullptr);
        std::unique_ptr<wl_callback, decltype(&wl_callback_destroy)> sync(callback,
                                                                          wl_callback_destroy);
        wl_callback_add_listener(callback, &listener, &done);
        const auto deadline = IdlePower::Clock::now() + 1s;
        while (!done && IdlePower::Clock::now() < deadline) {
            ASSERT_GE(wl_display_dispatch_pending(client_), 0);
            ASSERT_TRUE(wl_display_flush(client_) >= 0 || errno == EAGAIN);
            ASSERT_GE(wl_event_loop_dispatch(loop_, 10), 0);
            wl_display_flush_clients(server_);
            pollfd readable{.fd = wl_display_get_fd(client_), .events = POLLIN, .revents = 0};
            if (poll(&readable, 1, 0) > 0 && (readable.revents & POLLIN)) {
                ASSERT_GE(wl_display_dispatch(client_), 0);
            }
        }
        ASSERT_TRUE(done);
    }

    wl_display *server_ = nullptr;
    wl_event_loop *loop_ = nullptr;
    wl_display *client_ = nullptr;
    wl_registry *registry_ = nullptr;
    wl_compositor *compositor_ = nullptr;
    zwp_idle_inhibit_manager_v1 *manager_ = nullptr;
    std::unique_ptr<IdlePower> idle_;
    wlr_output left_, right_;
    wlr_output *fullscreen_ = nullptr;
    wlr_output *inhibited_ = nullptr;
    wlr_output *rejection_target_ = nullptr;
    unsigned remaining_rejections_ = 0;
    unsigned checks_ = 0;
    IdlePower::Clock::time_point now_{};
    std::vector<PowerChange> changes_;
};

TEST_F(IdleTrial, DefaultDeadlineAndInputWakeAreIndependentPerOutput) {
    ASSERT_NO_FATAL_FAILURE(advance(15min - 1ms));
    EXPECT_TRUE(changes_.empty());
    idle_->activity(&right_);
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_EQ(changes_, (std::vector<PowerChange>{{&left_, false}}));
    idle_->activity(&left_);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(changes_, (std::vector<PowerChange>{{&left_, false}, {&left_, true}}));
}

TEST_F(IdleTrial, ShortenedExtendedAndDisabledTimeoutUseTheCurrentIdleTime) {
    ASSERT_NO_FATAL_FAILURE(advance(2min));
    idle_->set_timeout(1min);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 1);
    idle_->set_timeout(3min);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, true}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, true}), 1);
    ASSERT_NO_FATAL_FAILURE(advance(1min));
    EXPECT_EQ(changes_.size(), 6u);
    idle_->set_timeout(0min);
    ASSERT_NO_FATAL_FAILURE(advance(24h));
    EXPECT_EQ(changes_.size(), 8u);
    ASSERT_NO_FATAL_FAILURE(advance(24h));
    EXPECT_EQ(changes_.size(), 8u);
}

TEST_F(IdleTrial, FullscreenOnlyProtectsItsOwnOutputAndFollowsVisibility) {
    fullscreen_ = &left_;
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_, (std::vector<PowerChange>{{&right_, false}}));
    fullscreen_ = &right_;
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(changes_, (std::vector<PowerChange>{{&right_, false}, {&right_, true}}));
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.back(), (PowerChange{&left_, false}));
}

TEST_F(IdleTrial, HiddenProtocolInhibitorAndSurfaceDestructionReconcilePower) {
    ASSERT_NO_FATAL_FAILURE(connect_client());
    std::unique_ptr<wl_surface, decltype(&wl_surface_destroy)> surface(
        wl_compositor_create_surface(compositor_), wl_surface_destroy);
    ASSERT_NE(surface, nullptr);
    std::unique_ptr<zwp_idle_inhibitor_v1, decltype(&zwp_idle_inhibitor_v1_destroy)> inhibitor(
        zwp_idle_inhibit_manager_v1_create_inhibitor(manager_, surface.get()),
        zwp_idle_inhibitor_v1_destroy);
    ASSERT_NE(inhibitor, nullptr);
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.size(), 2u);
    inhibited_ = &left_;
    inhibitor.reset(zwp_idle_inhibit_manager_v1_create_inhibitor(manager_, surface.get()));
    ASSERT_NE(inhibitor, nullptr);
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(changes_.size(), 2u);
    idle_->activity(&left_);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(changes_.back(), (PowerChange{&left_, true}));
    ASSERT_NO_FATAL_FAILURE(advance(30min));
    EXPECT_EQ(changes_.size(), 3u);
    surface.reset();
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.back(), (PowerChange{&left_, false}));
    EXPECT_EQ(changes_.size(), 4u);
}

TEST_F(IdleTrial, PausedSeatDoesNotCommitPowerAndResumesIdleDeadline) {
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.size(), 2u);
    idle_->set_active(false);
    idle_->activity(&left_);
    idle_->remove(&right_);
    now_ += 1h;
    EXPECT_GE(wl_event_loop_dispatch(loop_, 0), 0);
    EXPECT_EQ(changes_.size(), 2u);
    idle_->set_active(true);
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(changes_.size(), 3u);
    EXPECT_EQ(changes_.back(), (PowerChange{&left_, true}));
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.back(), (PowerChange{&left_, false}));
}

TEST_F(IdleTrial, RejectedPowerTransitionsRetryOnceAndRetainTheAppliedState) {
    fullscreen_ = &right_;
    rejection_target_ = &left_;
    remaining_rejections_ = 2;
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.size(), 1u);
    ASSERT_NO_FATAL_FAILURE(advance(249ms));
    EXPECT_EQ(changes_.size(), 1u);
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_EQ(changes_.size(), 2u);
    ASSERT_NO_FATAL_FAILURE(advance(1h));
    EXPECT_EQ(changes_.size(), 2u);
    idle_->activity(&left_);
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 3);

    remaining_rejections_ = 1;
    idle_->activity(&left_);
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(250ms));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, true}), 2);
    ASSERT_NO_FATAL_FAILURE(advance(1s));
    EXPECT_EQ(changes_.size(), 5u);
}

} // namespace
} // namespace seaside

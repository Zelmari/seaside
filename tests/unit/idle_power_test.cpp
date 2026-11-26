#include "idle-inhibit-client-protocol.h"
#include "idle_power.hpp"
#include "wlroots_bridge.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server-core.h>

// Keep the C-only wlroots headers out of the C++ fixture.
extern "C" {
wlr_renderer *wlr_pixman_renderer_create();
bool wlr_renderer_init_wl_shm(wlr_renderer *renderer, wl_display *display);
wlr_surface *wlr_surface_from_resource(wl_resource *resource);
void wlr_surface_map(wlr_surface *surface);
void wlr_surface_for_each_surface(wlr_surface *surface,
                                  void (*visit)(wlr_surface *, int, int, void *), void *data);
}

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
        renderer_.reset(wlr_pixman_renderer_create());
        ASSERT_NE(renderer_, nullptr);
        ASSERT_TRUE(wlr_renderer_init_wl_shm(renderer_.get(), server_));
        ASSERT_NE(seaside_wlr_compositor_create(server_, renderer_.get()), nullptr);
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
                if (subsurface_root_) {
                    struct Search {
                        wlr_surface *target;
                        bool mapped = false;
                    } search{surface};
                    wlr_surface_for_each_surface(
                        subsurface_root_,
                        [](wlr_surface *part, int, int, void *data) {
                            auto &search = *static_cast<Search *>(data);
                            search.mapped |= part == search.target;
                        },
                        &search);
                    ++subsurface_checks_;
                    subsurface_visible_ = search.mapped;
                    return search.mapped ? inhibited_ : nullptr;
                }
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
        if (subcompositor_) {
            wl_subcompositor_destroy(subcompositor_);
        }
        if (shm_) {
            wl_shm_destroy(shm_);
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

    void wait_for_check() {
        const auto last = checks_;
        const auto deadline = IdlePower::Clock::now() + 1s;
        while (checks_ == last && IdlePower::Clock::now() < deadline) {
            ASSERT_GE(wl_event_loop_dispatch(loop_, 10), 0);
        }
        ASSERT_GT(checks_, last);
    }

    void check() {
        idle_->visibility_changed();
        wait_for_check();
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
        } else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0) {
            self.subcompositor_ = static_cast<wl_subcompositor *>(
                wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
        } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
            self.shm_ =
                static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        }
    }

    void connect_client() {
        int sockets[2];
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        server_client_ = wl_client_create(server_, sockets[0]);
        if (!server_client_) {
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

    void wait_for_subsurface_check(unsigned previous) {
        const auto deadline = IdlePower::Clock::now() + 1s;
        while (subsurface_checks_ == previous && IdlePower::Clock::now() < deadline) {
            ASSERT_GE(wl_event_loop_dispatch(loop_, 10), 0);
        }
        ASSERT_GT(subsurface_checks_, previous);
    }

    wl_display *server_ = nullptr;
    wl_event_loop *loop_ = nullptr;
    wl_client *server_client_ = nullptr;
    wl_display *client_ = nullptr;
    wl_registry *registry_ = nullptr;
    wl_compositor *compositor_ = nullptr;
    wl_subcompositor *subcompositor_ = nullptr;
    wl_shm *shm_ = nullptr;
    zwp_idle_inhibit_manager_v1 *manager_ = nullptr;
    std::unique_ptr<wlr_renderer, decltype(&seaside_wlr_renderer_destroy)> renderer_{
        nullptr, seaside_wlr_renderer_destroy};
    std::unique_ptr<IdlePower> idle_;
    wlr_output left_, right_;
    wlr_output *fullscreen_ = nullptr;
    wlr_output *inhibited_ = nullptr;
    wlr_surface *subsurface_root_ = nullptr;
    unsigned subsurface_checks_ = 0;
    bool subsurface_visible_ = false;
    wlr_output *rejection_target_ = nullptr;
    unsigned remaining_rejections_ = 0;
    unsigned checks_ = 0;
    IdlePower::Clock::time_point now_{};
    std::vector<PowerChange> changes_;
};

TEST_F(IdleTrial, LocalInputDelaysSleepAndWakesBothOutputs) {
    ASSERT_NO_FATAL_FAILURE(advance(15min - 1ms));
    EXPECT_TRUE(changes_.empty());
    idle_->activity();
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_TRUE(changes_.empty());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 1);
    idle_->activity();
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, true}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, true}), 1);
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

TEST_F(IdleTrial, FullscreenOnEitherDisplayProtectsTheWholeSeat) {
    fullscreen_ = &left_;
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_TRUE(changes_.empty());
    fullscreen_ = &right_;
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_TRUE(changes_.empty());
    fullscreen_ = nullptr;
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min - 1ms));
    EXPECT_TRUE(changes_.empty());
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 1);
}

TEST_F(IdleTrial, ReconnectedOutputJoinsExistingDeadline) {
    idle_->remove(&right_);
    ASSERT_NO_FATAL_FAILURE(advance(14min));
    ASSERT_TRUE(idle_->add(&right_));
    ASSERT_NO_FATAL_FAILURE(advance(1min - 1ms));
    EXPECT_TRUE(changes_.empty());
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_EQ(changes_.size(), 2u);
    idle_->remove(&right_);
    ASSERT_TRUE(idle_->add(&right_));
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 2);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 1);
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
    idle_->activity();
    ASSERT_NO_FATAL_FAILURE(check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, true}), 1);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, true}), 1);
    ASSERT_NO_FATAL_FAILURE(advance(30min));
    EXPECT_EQ(changes_.size(), 4u);
    surface.reset();
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 2);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 2);
    EXPECT_EQ(changes_.size(), 6u);
}

TEST_F(IdleTrial, InhibitingSubsurfaceMapAndUnmapScheduleTheirOwnVisibilityChecks) {
    ASSERT_NO_FATAL_FAILURE(connect_client());
    ASSERT_NE(shm_, nullptr);
    ASSERT_NE(subcompositor_, nullptr);
    using Surface = std::unique_ptr<wl_surface, decltype(&wl_surface_destroy)>;
    Surface parent(wl_compositor_create_surface(compositor_), wl_surface_destroy);
    Surface child(wl_compositor_create_surface(compositor_), wl_surface_destroy);
    ASSERT_TRUE(parent && child);
    std::unique_ptr<wl_subsurface, decltype(&wl_subsurface_destroy)> subsurface(
        wl_subcompositor_get_subsurface(subcompositor_, child.get(), parent.get()),
        wl_subsurface_destroy);
    ASSERT_NE(subsurface, nullptr);
    wl_subsurface_set_desync(subsurface.get());

    constexpr int stride = 4;
    const int fd = memfd_create("seaside-idle-subsurface", MFD_CLOEXEC);
    ASSERT_GE(fd, 0);
    if (ftruncate(fd, 2 * stride) != 0) {
        close(fd);
        FAIL() << "Could not size the subsurface pixel buffers";
    }
    auto *pool = wl_shm_create_pool(shm_, fd, 2 * stride);
    close(fd);
    ASSERT_NE(pool, nullptr);
    using Buffer = std::unique_ptr<wl_buffer, decltype(&wl_buffer_destroy)>;
    Buffer parent_buffer(wl_shm_pool_create_buffer(pool, 0, 1, 1, stride, WL_SHM_FORMAT_XRGB8888),
                         wl_buffer_destroy);
    Buffer child_buffer(
        wl_shm_pool_create_buffer(pool, stride, 1, 1, stride, WL_SHM_FORMAT_XRGB8888),
        wl_buffer_destroy);
    wl_shm_pool_destroy(pool);
    ASSERT_TRUE(parent_buffer && child_buffer);
    wl_surface_attach(parent.get(), parent_buffer.get(), 0, 0);
    wl_surface_commit(parent.get());
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    auto *resource = wl_client_get_object(
        server_client_, wl_proxy_get_id(reinterpret_cast<wl_proxy *>(parent.get())));
    ASSERT_NE(resource, nullptr);
    subsurface_root_ = wlr_surface_from_resource(resource);
    ASSERT_NE(subsurface_root_, nullptr);
    // The fixture acts as the parent role. The child maps and unmaps through
    // the real subsurface protocol without another commit to its parent.
    wlr_surface_map(subsurface_root_);
    inhibited_ = &left_;
    const auto initial = subsurface_checks_;
    std::unique_ptr<zwp_idle_inhibitor_v1, decltype(&zwp_idle_inhibitor_v1_destroy)> inhibitor(
        zwp_idle_inhibit_manager_v1_create_inhibitor(manager_, child.get()),
        zwp_idle_inhibitor_v1_destroy);
    ASSERT_NE(inhibitor, nullptr);
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(wait_for_subsurface_check(initial));
    EXPECT_FALSE(subsurface_visible_);

    const auto before_map = subsurface_checks_;
    wl_surface_attach(child.get(), child_buffer.get(), 0, 0);
    wl_surface_commit(child.get());
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(wait_for_subsurface_check(before_map));
    EXPECT_TRUE(subsurface_visible_);

    const auto before_unmap = subsurface_checks_;
    wl_surface_attach(child.get(), nullptr, 0, 0);
    wl_surface_commit(child.get());
    ASSERT_NO_FATAL_FAILURE(roundtrip());
    ASSERT_NO_FATAL_FAILURE(wait_for_subsurface_check(before_unmap));
    EXPECT_FALSE(subsurface_visible_);
}

TEST_F(IdleTrial, PausedSeatDoesNotCommitPowerAndResumesIdleDeadline) {
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.size(), 2u);
    idle_->set_active(false);
    idle_->activity();
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
    rejection_target_ = &left_;
    remaining_rejections_ = 2;
    ASSERT_NO_FATAL_FAILURE(advance(15min));
    EXPECT_EQ(changes_.size(), 2u);
    ASSERT_NO_FATAL_FAILURE(advance(249ms));
    EXPECT_EQ(changes_.size(), 2u);
    ASSERT_NO_FATAL_FAILURE(advance(1ms));
    EXPECT_EQ(changes_.size(), 3u);
    ASSERT_NO_FATAL_FAILURE(advance(1h));
    EXPECT_EQ(changes_.size(), 3u);
    // No visibility or config change follows the input. The policy must
    // schedule its own check, then arm the remaining idle deadline.
    idle_->activity();
    now_ += 15min - 10ms;
    ASSERT_NO_FATAL_FAILURE(wait_for_check());
    ASSERT_EQ(changes_.size(), 4u);
    now_ += 10ms;
    ASSERT_NO_FATAL_FAILURE(wait_for_check());
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, false}), 3);
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&right_, false}), 2);

    remaining_rejections_ = 1;
    idle_->activity();
    ASSERT_NO_FATAL_FAILURE(check());
    ASSERT_NO_FATAL_FAILURE(advance(250ms));
    EXPECT_EQ(std::ranges::count(changes_, PowerChange{&left_, true}), 2);
    ASSERT_NO_FATAL_FAILURE(advance(1s));
    EXPECT_EQ(changes_.size(), 9u);
}

} // namespace
} // namespace seaside

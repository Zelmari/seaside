#include "startup_gate.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <thread>
#include <wayland-server-core.h>

namespace {
bool fail_timer = false;
bool fail_update = false;
bool fail_idle = false;
using namespace std::chrono_literals;
using seaside::ServiceFailure;
using seaside::StartupError;
using seaside::StartupGate;
using seaside::StartupPhase;
using seaside::StartupResult;
using seaside::StartupService;
constexpr std::array services{StartupService::systemd_user, StartupService::pipewire,
                              StartupService::portal_frontend, StartupService::capture_backend};

class StartupGateTest : public testing::Test {
  protected:
    wl_event_loop *loop = nullptr;
    std::unique_ptr<StartupGate> gate;
    unsigned completions = 0;
    StartupResult observed;
    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3-strict-readiness-coordination-simulated");
        loop = wl_event_loop_create();
        ASSERT_NE(loop, nullptr);
    }
    void TearDown() override {
        gate.reset();
        if (loop) {
            wl_event_loop_destroy(loop);
        }
        fail_timer = fail_update = fail_idle = false;
    }
    bool create(std::chrono::milliseconds timeout = 5s) {
        auto made = StartupGate::create(
            loop,
            [&](StartupResult result) {
                ++completions;
                observed = result;
            },
            timeout);
        if (!made) {
            return false;
        }
        gate = std::move(*made);
        return true;
    }
    bool wait() {
        const auto limit = std::chrono::steady_clock::now() + 2s;
        while (!completions && std::chrono::steady_clock::now() < limit) {
            if (wl_event_loop_dispatch(loop, 10) < 0) {
                return false;
            }
        }
        return completions == 1;
    }
};

TEST_F(StartupGateTest, RequiresEveryMandatoryReportInAnyCompletionOrder) {
    std::array<unsigned, 4> order{0, 1, 2, 3};
    do {
        ASSERT_TRUE(create());
        completions = 0;
        for (std::size_t i = 0; i < order.size(); ++i) {
            ASSERT_TRUE(gate->report(services[order[i]]));
            EXPECT_EQ(completions, 0u);
            EXPECT_EQ(gate->result().phase, i == 3 ? StartupPhase::ready : StartupPhase::checking);
        }
        wl_event_loop_dispatch_idle(loop);
        ASSERT_EQ(completions, 1u);
        EXPECT_EQ(observed.phase, StartupPhase::ready);
        EXPECT_FALSE(observed.service);
        EXPECT_EQ(observed.detail.failure, ServiceFailure::none);
        for (const auto &report : gate->reports()) {
            ASSERT_TRUE(report);
            EXPECT_EQ(report->failure, ServiceFailure::none);
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_F(StartupGateTest, FailureClassificationAndNativeErrorArePreservedForEveryService) {
    constexpr std::array failures{
        ServiceFailure::missing,           ServiceFailure::activation_failed,
        ServiceFailure::wrong_environment, ServiceFailure::incompatible,
        ServiceFailure::timeout,           ServiceFailure::disconnected,
        ServiceFailure::internal_error,
    };
    for (const auto service : services) {
        for (const auto failure : failures) {
            ASSERT_TRUE(create());
            completions = 0;
            ASSERT_TRUE(gate->report(service, {failure, -EIO}));
            const auto late = gate->report(StartupService::pipewire);
            ASSERT_FALSE(late);
            EXPECT_EQ(late.error(), StartupError::stopped);
            wl_event_loop_dispatch_idle(loop);
            EXPECT_EQ(completions, 1u);
            EXPECT_EQ(observed.phase, StartupPhase::failed);
            EXPECT_EQ(observed.service, service);
            EXPECT_EQ(observed.detail.failure, failure);
            EXPECT_EQ(observed.detail.native_error, -EIO);
        }
    }
}

TEST_F(StartupGateTest, DuplicateAndInvalidReportsDoNotFillMissingRequirements) {
    ASSERT_TRUE(create());
    ASSERT_TRUE(gate->report(StartupService::systemd_user));
    auto duplicate = gate->report(StartupService::systemd_user);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error(), StartupError::duplicate);
    // These deliberate non-enumerator values exercise rejection at the typed
    // boundary; the analyzer otherwise treats constructing the test input as a bug.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    auto invalid_service = gate->report(static_cast<StartupService>(99));
    ASSERT_FALSE(invalid_service);
    EXPECT_EQ(invalid_service.error(), StartupError::invalid_arguments);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto invalid_kind = static_cast<ServiceFailure>(99);
    auto invalid_failure = gate->report(StartupService::pipewire, {invalid_kind, 0});
    ASSERT_FALSE(invalid_failure);
    EXPECT_EQ(invalid_failure.error(), StartupError::invalid_arguments);
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(completions, 0u);
    EXPECT_EQ(gate->result().phase, StartupPhase::checking);
    EXPECT_FALSE(gate->reports()[1]);
}

TEST_F(StartupGateTest, DeadlineNamesTheFirstStillPendingServiceWithoutBlockingOtherSources) {
    ASSERT_TRUE(create(30ms));
    ASSERT_TRUE(gate->report(StartupService::systemd_user));
    unsigned progress = 0;
    auto *heartbeat = wl_event_loop_add_timer(
        loop,
        [](void *data) -> int {
            ++*static_cast<unsigned *>(data);
            return 0;
        },
        &progress);
    ASSERT_NE(heartbeat, nullptr);
    ASSERT_EQ(wl_event_source_timer_update(heartbeat, 1), 0);
    const bool finished = wait();
    wl_event_source_remove(heartbeat);
    ASSERT_TRUE(finished);
    EXPECT_EQ(progress, 1u);
    EXPECT_EQ(observed.phase, StartupPhase::failed);
    EXPECT_EQ(observed.service, StartupService::pipewire);
    EXPECT_EQ(observed.detail.failure, ServiceFailure::timeout);
}

TEST_F(StartupGateTest, CancellationAndDestructionSuppressQueuedDelivery) {
    ASSERT_TRUE(create());
    for (auto service : services) {
        ASSERT_TRUE(gate->report(service));
    }
    ASSERT_TRUE(gate->cancel());
    EXPECT_EQ(gate->result().phase, StartupPhase::cancelled);
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(completions, 0u);
    auto late = gate->report(StartupService::pipewire);
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error(), StartupError::stopped);
    ASSERT_TRUE(create());
    ASSERT_TRUE(gate->report(StartupService::capture_backend, {ServiceFailure::incompatible, 0}));
    gate.reset();
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(completions, 0u);
}

TEST_F(StartupGateTest, RepliesAfterTheDeadlineCannotBecomeReadyBeforeTheTimerDispatches) {
    ASSERT_TRUE(create(5ms));
    const auto expired = std::chrono::steady_clock::now() + 6ms;
    // Wait for the clock deadline, deliberately without servicing Wayland. This
    // tests dispatch ordering, rather than guessing when another process acts.
    while (std::chrono::steady_clock::now() < expired) {
        std::this_thread::yield();
    }
    for (const auto service : services) {
        auto late = gate->report(service);
        ASSERT_FALSE(late);
        EXPECT_EQ(late.error(), StartupError::stopped);
    }
    wl_event_loop_dispatch_idle(loop);
    ASSERT_EQ(completions, 1u);
    EXPECT_EQ(observed.phase, StartupPhase::failed);
    EXPECT_EQ(observed.service, StartupService::systemd_user);
    EXPECT_EQ(observed.detail.failure, ServiceFailure::timeout);
}

TEST_F(StartupGateTest, CallbackCanDestroyItsGateAndExceptionsStayInsideTheCBoundary) {
    auto made = StartupGate::create(loop, [&](auto) {
        ++completions;
        gate.reset();
    });
    ASSERT_TRUE(made);
    gate = std::move(*made);
    for (auto service : services) {
        ASSERT_TRUE(gate->report(service));
    }
    wl_event_loop_dispatch_idle(loop);
    EXPECT_EQ(completions, 1u);
    EXPECT_FALSE(gate);
    auto throwing = StartupGate::create(loop, [](auto) { throw std::runtime_error("failure"); });
    ASSERT_TRUE(throwing);
    gate = std::move(*throwing);
    for (auto service : services) {
        ASSERT_TRUE(gate->report(service));
    }
    EXPECT_NO_THROW(wl_event_loop_dispatch_idle(loop));
    EXPECT_EQ(gate->result().phase, StartupPhase::failed);
    EXPECT_EQ(gate->result().detail.failure, ServiceFailure::internal_error);
}

TEST_F(StartupGateTest, InvalidConstructionAndEventSourceFailuresAreErrors) {
    EXPECT_FALSE(StartupGate::create(nullptr, [](auto) {}));
    EXPECT_FALSE(StartupGate::create(loop, {}));
    EXPECT_FALSE(StartupGate::create(loop, [](auto) {}, 0ms));
    EXPECT_FALSE(StartupGate::create(loop, [](auto) {}, 61s));
    fail_timer = true;
    EXPECT_FALSE(create());
    fail_timer = false;
    fail_update = true;
    EXPECT_FALSE(create());
    fail_update = false;
    ASSERT_TRUE(create());
    fail_idle = true;
    auto result = gate->report(StartupService::pipewire, {ServiceFailure::disconnected, -EPIPE});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), StartupError::system);
    EXPECT_EQ(gate->result().phase, StartupPhase::failed);
    EXPECT_EQ(gate->result().detail.failure, ServiceFailure::internal_error);
}

TEST_F(StartupGateTest, DeadlineDeliveryAllocationFailureStillNotifiesInsteadOfHanging) {
    ASSERT_TRUE(create(10ms));
    fail_idle = true;
    ASSERT_TRUE(wait());
    EXPECT_EQ(observed.phase, StartupPhase::failed);
    EXPECT_EQ(observed.detail.failure, ServiceFailure::internal_error);
}

TEST_F(StartupGateTest, ForeignThreadCannotReportOrCancel) {
    ASSERT_TRUE(create());
    std::optional<StartupError> report_error;
    std::optional<StartupError> cancel_error;
    std::jthread foreign([&] {
        const auto report = gate->report(StartupService::pipewire);
        if (!report) {
            report_error = report.error();
        }
        const auto cancel = gate->cancel();
        if (!cancel) {
            cancel_error = cancel.error();
        }
    });
    foreign.join();
    EXPECT_EQ(report_error, StartupError::wrong_thread);
    EXPECT_EQ(cancel_error, StartupError::wrong_thread);
    EXPECT_EQ(gate->result().phase, StartupPhase::checking);
}
} // namespace

extern "C" {
wl_event_source *__real_wl_event_loop_add_timer(wl_event_loop *, wl_event_loop_timer_func_t,
                                                void *);
wl_event_source *__wrap_wl_event_loop_add_timer(wl_event_loop *loop,
                                                wl_event_loop_timer_func_t callback, void *data) {
    return fail_timer ? nullptr : __real_wl_event_loop_add_timer(loop, callback, data);
}
int __real_wl_event_source_timer_update(wl_event_source *, int);
int __wrap_wl_event_source_timer_update(wl_event_source *source, int delay) {
    return fail_update ? -1 : __real_wl_event_source_timer_update(source, delay);
}
wl_event_source *__real_wl_event_loop_add_idle(wl_event_loop *, wl_event_loop_idle_func_t, void *);
wl_event_source *__wrap_wl_event_loop_add_idle(wl_event_loop *loop,
                                               wl_event_loop_idle_func_t callback, void *data) {
    return fail_idle ? nullptr : __real_wl_event_loop_add_idle(loop, callback, data);
}
}

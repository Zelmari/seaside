#include "work_queue.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using seaside::WorkError;
using seaside::WorkLifetime;
using seaside::WorkQueue;
using seaside::WorkResult;

struct Gate {
    std::mutex mutex;
    std::condition_variable_any condition;
    bool released = false;
    void wait(std::stop_token stop) {
        std::unique_lock lock(mutex);
        condition.wait(lock, stop, [&] { return released; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }
};

class WorkQueueTest : public testing::Test {
  protected:
    wl_event_loop *loop = nullptr;
    std::unique_ptr<WorkQueue> queue;
    std::shared_ptr<WorkLifetime> lifetime;
    void SetUp() override {
        RecordProperty("part", "2");
        RecordProperty("requirement", "spec-2.1-8.1-work-queue");
        loop = wl_event_loop_create();
        ASSERT_NE(loop, nullptr);
        lifetime = std::make_shared<WorkLifetime>();
    }
    void TearDown() override {
        queue.reset();
        wl_event_loop_destroy(loop);
    }
    void create(WorkQueue::Limits limits = {}) {
        auto created = WorkQueue::create(loop, limits);
        ASSERT_TRUE(created);
        queue = std::move(*created);
    }
    template <class Predicate> bool workers_reach(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
    template <class Predicate> bool dispatch_until(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                wl_event_loop_dispatch(loop, 20) < 0) {
                return false;
            }
        }
        return true;
    }
};

TEST_F(WorkQueueTest, RejectsInvalidLimitsAndWrongThreadOperations) {
    EXPECT_FALSE(WorkQueue::create(nullptr));
    EXPECT_FALSE(WorkQueue::create(loop, {.workers = 0}));
    EXPECT_FALSE(WorkQueue::create(loop, {.max_tasks = 0}));
    EXPECT_FALSE(WorkQueue::create(loop, {.max_retained_bytes = 0}));
    EXPECT_FALSE(WorkQueue::create(loop, {.dispatch_budget = 0}));
    EXPECT_FALSE(WorkQueue::create(loop, {.dispatch_budget = 1025}));
    EXPECT_TRUE(WorkQueue::create(loop, {.max_tasks = 1}));
    create();
    std::jthread foreign([&] {
        EXPECT_EQ(queue->submit(
                           0, lifetime, 1, [](auto) { return WorkResult{}; }, [](auto) {})
                      .error(),
                  WorkError::wrong_thread);
        EXPECT_EQ(queue->set_generation(1).error(), WorkError::wrong_thread);
        EXPECT_EQ(queue->shutdown().error(), WorkError::wrong_thread);
    });
    foreign.join();
    EXPECT_FALSE(queue->submit(0, lifetime, 0, [](auto) { return WorkResult{}; }, [](auto) {}));
}

TEST_F(WorkQueueTest, CapacityIncludesRunningTasksAndCompletionBacklog) {
    create({.workers = 1, .max_tasks = 2, .max_retained_bytes = 128, .dispatch_budget = 1});
    auto gate = std::make_shared<Gate>();
    auto started = std::make_shared<std::promise<void>>();
    std::size_t calls = 0;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 64,
        [gate, started](auto stop) {
            started->set_value();
            gate->wait(stop);
            return WorkResult{};
        },
        [&](auto) { ++calls; }));
    ASSERT_EQ(started->get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(
        queue->submit(0, lifetime, 64, [](auto) { return WorkResult{}; }, [&](auto) { ++calls; }));
    EXPECT_EQ(queue->submit(
                       0, lifetime, 1, [](auto) { return WorkResult{}; }, [](auto) {})
                  .error(),
              WorkError::full);
    gate->release();
    ASSERT_TRUE(workers_reach([&] { return queue->stats().completed == 2; }));
    EXPECT_EQ(calls, 0U);
    EXPECT_EQ(queue->stats().reserved_bytes, 128U);
    EXPECT_EQ(queue->submit(
                       0, lifetime, 1, [](auto) { return WorkResult{}; }, [](auto) {})
                  .error(),
              WorkError::full);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(calls, 1U);
    EXPECT_EQ(queue->stats().outstanding, 1U);
    ASSERT_TRUE(dispatch_until([&] { return calls == 2; }));
    EXPECT_EQ(queue->stats().reserved_bytes, 0U);
}

TEST_F(WorkQueueTest, MainLoopOwnsCallbacksAndExceptionsBecomeReportedFailures) {
    create();
    const auto owner = std::this_thread::get_id();
    WorkError observed = WorkError::none;
    bool called = false;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32,
        [owner](auto) -> WorkResult {
            EXPECT_NE(std::this_thread::get_id(), owner);
            throw std::runtime_error("job failure");
        },
        [&](auto result) {
            EXPECT_EQ(std::this_thread::get_id(), owner);
            observed = result.error;
            called = true;
            throw std::runtime_error("callback failure");
        }));
    ASSERT_TRUE(dispatch_until([&] { return called; }));
    EXPECT_EQ(observed, WorkError::job_failed);
    EXPECT_EQ(queue->stats().job_failures, 1U);
    EXPECT_EQ(queue->stats().callback_failures, 1U);
    EXPECT_EQ(queue->stats().outstanding, 0U);
}

TEST_F(WorkQueueTest, OversizedResultIsReleasedAndReported) {
    create();
    bool called = false;
    std::weak_ptr<const int> retained;
    auto value = std::make_shared<const int>(4);
    retained = value;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32,
        [value = std::move(value)](auto) { return WorkResult{value, 33, WorkError::none}; },
        [&](auto result) {
            EXPECT_EQ(result.error, WorkError::result_too_large);
            EXPECT_FALSE(result.value);
            called = true;
        }));
    ASSERT_TRUE(dispatch_until([&] { return called; }));
    EXPECT_TRUE(retained.expired());
    EXPECT_EQ(queue->stats().reserved_bytes, 0U);
}

TEST_F(WorkQueueTest, DestroyedRecipientDiscardsItsCompletedResult) {
    create();
    bool called = false;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32, [](auto) { return WorkResult{}; }, [&](auto) { called = true; }));
    ASSERT_TRUE(workers_reach([&] { return queue->stats().completed == 1; }));
    lifetime.reset();
    ASSERT_TRUE(dispatch_until([&] { return queue->stats().outstanding == 0; }));
    EXPECT_FALSE(called);
    EXPECT_EQ(queue->stats().discarded, 1U);
}

TEST_F(WorkQueueTest, NewGenerationPurgesQueuedWorkAndDiscardsOldInflightResults) {
    create({.workers = 1, .max_tasks = 3, .max_retained_bytes = 128, .dispatch_budget = 1});
    ASSERT_TRUE(queue->set_generation(1));
    auto gate = std::make_shared<Gate>();
    auto started = std::make_shared<std::promise<void>>();
    std::size_t old_calls = 0;
    bool latest = false;
    ASSERT_TRUE(queue->submit(
        1, lifetime, 32,
        [gate, started](auto stop) {
            started->set_value();
            gate->wait(stop);
            return WorkResult{};
        },
        [&](auto) { ++old_calls; }));
    ASSERT_EQ(started->get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(queue->submit(
        1, lifetime, 32, [](auto) { return WorkResult{}; }, [&](auto) { ++old_calls; }));
    ASSERT_TRUE(queue->set_generation(2));
    EXPECT_EQ(queue->set_generation(1).error(), WorkError::invalid_job);
    EXPECT_EQ(queue->stats().outstanding, 1U);
    ASSERT_TRUE(queue->submit(
        2, lifetime, 32, [](auto) { return WorkResult{}; }, [&](auto) { latest = true; }));
    gate->release();
    ASSERT_TRUE(dispatch_until([&] { return latest; }));
    EXPECT_EQ(old_calls, 0U);
    EXPECT_EQ(queue->stats().discarded, 2U);
    EXPECT_EQ(queue->stats().reserved_bytes, 0U);
}

TEST_F(WorkQueueTest, ShutdownRequestsStopAndReleasesAllReservationsWithoutCallbacks) {
    create({.workers = 1, .max_tasks = 2, .max_retained_bytes = 128, .dispatch_budget = 1});
    auto gate = std::make_shared<Gate>();
    auto started = std::make_shared<std::promise<void>>();
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    bool called = false;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32,
        [gate, started, stopped](auto stop) {
            started->set_value();
            gate->wait(stop);
            stopped->store(stop.stop_requested());
            return WorkResult{};
        },
        [&](auto) { called = true; }));
    ASSERT_EQ(started->get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32, [](auto) { return WorkResult{}; }, [&](auto) { called = true; }));
    ASSERT_TRUE(queue->shutdown());
    ASSERT_TRUE(queue->shutdown());
    EXPECT_TRUE(stopped->load());
    EXPECT_FALSE(called);
    EXPECT_EQ(queue->stats().outstanding, 0U);
    EXPECT_EQ(queue->stats().reserved_bytes, 0U);
    EXPECT_EQ(queue->submit(
                       0, lifetime, 1, [](auto) { return WorkResult{}; }, [](auto) {})
                  .error(),
              WorkError::stopped);
}

TEST_F(WorkQueueTest, CompletionMayDestroyItsQueueSafely) {
    create();
    bool called = false;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32, [](auto) { return WorkResult{}; },
        [&](auto) {
            queue.reset();
            called = true;
        }));
    ASSERT_TRUE(dispatch_until([&] { return called; }));
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
}

TEST_F(WorkQueueTest, CompletedOldGenerationAndInvalidatedRecipientsArePurged) {
    create();
    bool called = false;
    ASSERT_TRUE(queue->submit(
        0, lifetime, 32, [](auto) { return WorkResult{}; }, [&](auto) { called = true; }));
    ASSERT_TRUE(workers_reach([&] { return queue->stats().completed == 1; }));
    lifetime->invalidate();
    ASSERT_TRUE(queue->set_generation(1));
    EXPECT_EQ(queue->stats().outstanding, 0U);
    EXPECT_EQ(queue->stats().reserved_bytes, 0U);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_FALSE(called);
}
} // namespace

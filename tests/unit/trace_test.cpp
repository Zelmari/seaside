#include "config_store.hpp"
#include "test_directory.hpp"
#include "trace.hpp"

#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <semaphore>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace seaside {
namespace {
using namespace std::chrono_literals;

class ControlledStorage : public StorageIo {
  public:
    std::error_code check(StorageStep step) noexcept override {
        if (step == StorageStep::write && pause.exchange(false)) {
            entered.release();
            if (!resume.try_acquire_for(3s)) {
                return std::make_error_code(std::errc::timed_out);
            }
        }
        if (step == fail_step.load()) {
            return std::make_error_code(std::errc::io_error);
        }
        return {};
    }

    std::atomic<StorageStep> fail_step{StorageStep::open_read};
    std::atomic<bool> pause{false};
    std::binary_semaphore entered{0};
    std::binary_semaphore resume{0};
};

class TraceFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_NE(loop, nullptr);
        auto created = Trace::create(loop, state, [this] { return now; }, storage);
        ASSERT_TRUE(created) << created.error();
        trace = std::move(*created);
    }

    void TearDown() override {
        trace.reset();
        if (loop) {
            wl_event_loop_destroy(loop);
        }
    }

    auto export_start() {
        result.reset();
        return trace->export_completed([this](IpcResult value) { result = std::move(value); });
    }

    void wait_export() {
        const auto deadline = Trace::Clock::now() + 3s;
        while (!result && Trace::Clock::now() < deadline) {
            ASSERT_EQ(wl_event_loop_dispatch(loop, 10), 0);
        }
        ASSERT_TRUE(result) << "Trace export completion did not dispatch";
    }

    test::Directory directory;
    std::filesystem::path state = directory.path() / "state" / "seaside";
    wl_event_loop *loop = wl_event_loop_create();
    Trace::Clock::time_point now{1s};
    std::shared_ptr<ControlledStorage> storage = std::make_shared<ControlledStorage>();
    std::unique_ptr<Trace> trace;
    std::optional<IpcResult> result;
};

TEST_F(TraceFixture, OptInDeadlineAndTypedErrors) {
    const auto empty = IpcJson{{"state", "empty"},
                               {"entries", 0},
                               {"bytes", 0},
                               {"dropped_samples", 0},
                               {"stop_reason", nullptr}};
    trace->record({{"event", "ignored"}});
    EXPECT_EQ(trace->status(), empty);
    auto not_running = trace->stop();
    ASSERT_FALSE(not_running);
    EXPECT_EQ(not_running.error().code, "trace_not_running");
    auto no_export = export_start();
    ASSERT_TRUE(no_export && !*no_export);
    EXPECT_EQ(no_export->error().code, "trace_not_running");

    auto started = trace->start();
    ASSERT_TRUE(started);
    EXPECT_EQ((*started)["trace"]["state"], "running");
    auto already_running = trace->start();
    ASSERT_FALSE(already_running);
    EXPECT_EQ(already_running.error().code, "trace_already_running");
    auto still_running = export_start();
    ASSERT_TRUE(still_running && !*still_running);
    EXPECT_EQ(still_running->error().code, "invalid_state");
    EXPECT_FALSE(std::filesystem::exists(state));

    now += Trace::time_limit - 1ns;
    EXPECT_TRUE(trace->running());
    now += 1ns;
    trace->record({{"event", "past the deadline"}});
    EXPECT_EQ(trace->status()["stop_reason"], "time");
    EXPECT_EQ(trace->status()["entries"], 0);
    EXPECT_EQ(trace->status()["dropped_samples"], 0);

    ASSERT_TRUE(trace->start());
    trace->record({{"event", "first record of the replacement"}});
    auto stopped = trace->stop();
    ASSERT_TRUE(stopped);
    EXPECT_EQ((*stopped)["trace"]["entries"], 1);
    EXPECT_EQ((*stopped)["trace"]["stop_reason"], "requested");
}

TEST_F(TraceFixture, StopsAtEntryAndSerializedByteLimits) {
    ASSERT_TRUE(trace->start());
    const IpcJson sample = {{"event", "present"}};
    for (std::size_t index = 0; index <= Trace::entry_limit; ++index) {
        trace->record(sample);
    }
    EXPECT_EQ(trace->status()["entries"], Trace::entry_limit);
    EXPECT_EQ(trace->status()["stop_reason"], "entries");
    EXPECT_EQ(trace->status()["dropped_samples"], 0);

    ASSERT_TRUE(trace->start());
    trace->record({{"data", ""}});
    const auto overhead = trace->status()["bytes"].get<std::size_t>();
    ASSERT_GT(overhead, 0);
    ASSERT_TRUE(trace->stop());
    ASSERT_TRUE(trace->start());
    trace->record({{"data", std::string(Trace::byte_limit - overhead, 'a')}});
    EXPECT_EQ(trace->status()["stop_reason"], "bytes");
    EXPECT_EQ(trace->status()["bytes"], Trace::byte_limit);
    EXPECT_EQ(trace->status()["entries"], 1);
    EXPECT_EQ(trace->status()["dropped_samples"], 0);
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_EQ(std::filesystem::file_size((**result)["path"].get<std::string>()), Trace::byte_limit);

    ASSERT_TRUE(trace->start());
    trace->record(sample);
    const auto retained = trace->status()["bytes"];
    trace->record(IpcJson::array());
    trace->record({{"data", std::string(Trace::byte_limit - overhead, 'b')}});
    EXPECT_EQ(trace->status()["stop_reason"], "bytes");
    EXPECT_EQ(trace->status()["entries"], 1);
    EXPECT_EQ(trace->status()["bytes"], retained);
    EXPECT_EQ(trace->status()["dropped_samples"], 2);
}

TEST_F(TraceFixture, ExportsCompletePrivateJsonLinesAndKeepsThemForRetry) {
    ASSERT_TRUE(trace->start());
    trace->record({{"event", "repaint\nwith newline"}});
    now += 2ns;
    trace->record({{"event", "present"}, {"presented", true}});
    ASSERT_TRUE(trace->stop());
    const auto status = trace->status();
    storage->fail_step = StorageStep::write;
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code, "internal_error");
    EXPECT_TRUE(std::filesystem::is_empty(state / "traces"));
    EXPECT_EQ(trace->status(), status);

    storage->fail_step = StorageStep::sync_directory;
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_FALSE(*result);
    EXPECT_TRUE(std::filesystem::is_empty(state / "traces"));
    EXPECT_EQ(trace->status(), status);

    storage->fail_step = StorageStep::open_read;
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_TRUE(*result) << result->error().message;
    const auto path = std::filesystem::path((**result)["path"].get<std::string>());
    EXPECT_EQ(path.parent_path(), state / "traces");
    EXPECT_EQ(path.extension(), ".jsonl");
    EXPECT_EQ(std::filesystem::file_size(path), status["bytes"]);
    struct stat metadata{};
    ASSERT_EQ(stat(path.parent_path().c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_mode & 07777, 0700);
    ASSERT_EQ(stat(path.c_str(), &metadata), 0);
    EXPECT_EQ(metadata.st_uid, getuid());
    EXPECT_EQ(metadata.st_mode & 07777, 0600);
    std::ifstream input(path);
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    auto row = IpcJson::parse(line);
    EXPECT_EQ(row["observed_ns"], "1000000000");
    EXPECT_EQ(row["event"], "repaint\nwith newline");
    ASSERT_TRUE(std::getline(input, line));
    row = IpcJson::parse(line);
    EXPECT_EQ(row["observed_ns"], "1000000002");
    EXPECT_EQ(row["presented"], true);
    EXPECT_FALSE(std::getline(input, line));

    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_NE((**result)["path"], path.string());
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(TraceFixture, RefusesSymlinkAndNonPrivateTraceDirectory) {
    const auto target = directory.path() / "other";
    ASSERT_TRUE(std::filesystem::create_directories(target));
    ASSERT_TRUE(std::filesystem::create_directories(state));
    const auto traces = state / "traces";
    std::filesystem::create_directory_symlink(target, traces);
    ASSERT_TRUE(trace->start());
    ASSERT_TRUE(trace->stop());
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_FALSE(*result);
    EXPECT_TRUE(std::filesystem::is_empty(target));
    EXPECT_TRUE(std::filesystem::is_symlink(traces));

    ASSERT_TRUE(std::filesystem::remove(traces));
    ASSERT_TRUE(std::filesystem::create_directory(traces));
    ASSERT_EQ(chmod(traces.c_str(), 0755), 0);
    ASSERT_FALSE(export_start());
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_FALSE(*result);
    EXPECT_TRUE(std::filesystem::is_empty(traces));
}

TEST_F(TraceFixture, SlowExportKeepsDispatchResponsiveAndUsesOriginalCompletedSamples) {
    ASSERT_TRUE(trace->start());
    trace->record({{"event", "original"}});
    ASSERT_TRUE(trace->stop());
    storage->pause = true;
    ASSERT_FALSE(export_start());
    ASSERT_TRUE(storage->entered.try_acquire_for(2s));
    EXPECT_FALSE(result);
    const auto saturated =
        trace->export_completed([](IpcResult) { ADD_FAILURE() << "Queue was full"; });
    ASSERT_TRUE(saturated && !*saturated);
    EXPECT_EQ(saturated->error().code, "internal_error");

    bool dispatched = false;
    ASSERT_NE(wl_event_loop_add_idle(
                  loop, [](void *value) { *static_cast<bool *>(value) = true; }, &dispatched),
              nullptr);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_TRUE(dispatched);
    ASSERT_TRUE(trace->start());
    trace->record({{"event", "replacement"}});
    storage->resume.release();
    ASSERT_NO_FATAL_FAILURE(wait_export());
    ASSERT_TRUE(*result) << result->error().message;
    EXPECT_EQ(trace->status()["state"], "running");
    EXPECT_EQ(trace->status()["entries"], 1);
    std::ifstream old((**result)["path"].get<std::string>());
    std::string row;
    ASSERT_TRUE(std::getline(old, row));
    EXPECT_EQ(IpcJson::parse(row)["event"], "original");
}

TEST_F(TraceFixture, TeardownDiscardsPendingExportCompletionAndTemporaryFiles) {
    ASSERT_TRUE(trace->start());
    trace->record({{"event", "pending"}});
    ASSERT_TRUE(trace->stop());
    storage->pause = true;
    // Guarantee the in-progress write fails after the test releases its gate.
    storage->fail_step = StorageStep::write;
    ASSERT_FALSE(export_start());
    ASSERT_TRUE(storage->entered.try_acquire_for(2s));
    storage->resume.release();
    trace.reset();
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_FALSE(result);
    EXPECT_TRUE(std::filesystem::is_empty(state / "traces"));
}

} // namespace
} // namespace seaside

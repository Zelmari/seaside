#include "live_config.hpp"
#include "test_directory.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <wayland-server-core.h>

namespace {
using namespace seaside;
using namespace std::chrono_literals;
using Change = LiveConfig::Change;

class SnapshotFailure : public StorageIo {
  public:
    std::error_code check(StorageStep step) noexcept override {
        if (step == StorageStep::sync_file && fail.load())
            return std::make_error_code(std::errc::no_space_on_device);
        return {};
    }
    std::atomic<bool> fail = false;
};

class LiveConfigTest : public testing::Test {
  protected:
    bool pump_until(const std::function<bool()> &predicate,
                    std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            if (wl_event_loop_dispatch(loop.get(), 20) < 0)
                return false;
        }
        return predicate();
    }

    void start(LiveConfig::Prepare prepare = {}, LiveConfig::Apply apply = {}) {
        ASSERT_TRUE(loop);
        auto loaded = load_startup_config(store, paths);
        auto result = LiveConfig::create(
            loop.get(), store, paths, std::move(loaded), std::move(prepare), std::move(apply),
            [&](Change change, const LiveConfig::Prepared &) { events.push_back(change); });
        ASSERT_TRUE(result) << result.error();
        live = std::move(*result);
        live->session_ready();
    }

    void replace(std::string_view contents) {
        const auto replacement = paths.user.parent_path() / "editor.tmp";
        ASSERT_TRUE(std::ofstream(replacement) << contents);
        std::filesystem::rename(replacement, paths.user);
    }

    std::string snapshot() const {
        auto text = store.read(paths.recovery, true);
        EXPECT_TRUE(text && *text);
        return text && *text ? **text : "";
    }

    const ConfigTransaction &transaction() const { return live->transaction(); }

    test::Directory directory;
    const ConfigPaths paths{directory.path() / "config/seaside/config.toml",
                            directory.path() / "state/seaside/last-good-config.toml"};
    std::shared_ptr<SnapshotFailure> io = std::make_shared<SnapshotFailure>();
    ConfigStore store{io};
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> loop{wl_event_loop_create(),
                                                                          wl_event_loop_destroy};
    std::vector<Change> events;
    std::unique_ptr<LiveConfig> live;
};

TEST_F(LiveConfigTest, EditorReplacementAndRepairPreserveLastGoodAndRuntimeOverrides) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    EXPECT_EQ(snapshot(), default_template());
    const std::string valid = "[workspaces]\nmode='per-monitor'\n[window]\ngaps_outer=24\n"
                              "[keybinds]\n";
    ASSERT_NO_FATAL_FAILURE(replace(valid));
    live->set_runtime_night_light(true);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 24 && transaction().recovery_persisted();
    }));
    EXPECT_TRUE(transaction().active().keybinds.empty());
    EXPECT_TRUE(transaction().runtime_night_light());
    EXPECT_EQ(transaction().active().workspaces.mode, WorkspaceMode::global);
    EXPECT_EQ(transaction().requested().workspaces.mode, WorkspaceMode::per_monitor);
    EXPECT_EQ(snapshot(), valid);

    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer='broken'\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    EXPECT_EQ(events.back(), Change::rejected);
    EXPECT_EQ(transaction().active().window.gaps_outer, 24);
    EXPECT_EQ(snapshot(), valid);

    ASSERT_NO_FATAL_FAILURE(replace(valid));
    ASSERT_TRUE(pump_until(
        [&] { return !transaction().diagnostic() && transaction().recovery_persisted(); }));
    EXPECT_EQ(load_startup_config(store, paths).config.workspaces.mode, WorkspaceMode::per_monitor);
}

TEST_F(LiveConfigTest, InvalidStartupUsesRecoveryAndRepairsOnSave) {
    const auto recovered = "[window]\ngaps_outer=31\n";
    ASSERT_TRUE(store.publish(paths.user, "[window\n", PublishMode::create_user_file));
    ASSERT_TRUE(store.publish(paths.recovery, recovered, PublishMode::replace_snapshot));
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(live->source(), ConfigSource::recovery);
    EXPECT_TRUE(transaction().diagnostic());
    EXPECT_EQ(transaction().active().window.gaps_outer, 31);
    EXPECT_EQ(snapshot(), recovered);

    ASSERT_NO_FATAL_FAILURE(replace(""));
    ASSERT_TRUE(pump_until([&] {
        return live->source() == ConfigSource::user && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(transaction().active().window.gaps_outer, 10);
    EXPECT_TRUE(snapshot().empty());
}

TEST_F(LiveConfigTest, DirectoryReplacementAndAnInPlaceWriteAreObserved) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    std::filesystem::rename(paths.user.parent_path(), directory.path() / "retired");
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    std::filesystem::create_directory(paths.user.parent_path());
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=16\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().active().window.gaps_outer == 16; }));
    EXPECT_FALSE(transaction().diagnostic());

    auto open_file = std::ofstream(paths.user, std::ios::trunc);
    ASSERT_TRUE(open_file << "[win" << std::flush);
    EXPECT_FALSE(pump_until([&] { return transaction().diagnostic().has_value(); }, 300ms));
    ASSERT_TRUE(open_file << "dow]\ngaps_outer=17\n");
    open_file.close();
    ASSERT_TRUE(pump_until([&] { return transaction().active().window.gaps_outer == 17; }));
}

TEST_F(LiveConfigTest, SupersededWorkerCannotApplyWhileNewEditDebounces) {
    struct Gate {
        std::atomic<bool> started = false;
        bool release = false;
        std::mutex mutex;
        std::condition_variable condition;
    };
    auto gate = std::make_shared<Gate>();
    std::vector<int> applied;
    ASSERT_NO_FATAL_FAILURE(start(
        [gate](const Config &candidate,
               const Config &) -> std::expected<LiveConfig::Prepared, Diagnostic> {
            if (candidate.window.gaps_outer == 29) {
                std::unique_lock lock(gate->mutex);
                gate->started.store(true);
                gate->condition.wait_for(lock, 3s, [&] { return gate->release; });
            }
            return std::make_shared<const int>(candidate.window.gaps_outer);
        },
        [&](const Config &, const Config &candidate,
            const LiveConfig::Prepared &prepared) -> std::expected<void, Diagnostic> {
            EXPECT_EQ(*std::static_pointer_cast<const int>(prepared), candidate.window.gaps_outer);
            applied.push_back(candidate.window.gaps_outer);
            return {};
        }));
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=29\n"));
    ASSERT_TRUE(pump_until([&] { return gate->started.load(); }));
    const auto older = transaction().generation();
    const auto latest = "[window]\ngaps_outer=44\n";
    ASSERT_NO_FATAL_FAILURE(replace(latest));
    ASSERT_TRUE(pump_until([&] { return transaction().generation() > older; }));
    {
        std::scoped_lock lock(gate->mutex);
        gate->release = true;
    }
    gate->condition.notify_one();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 44 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(applied, std::vector{44});
    EXPECT_EQ(snapshot(), latest);
}

TEST_F(LiveConfigTest, PrepareRejectionAndSnapshotFailureLeavePriorSnapshot) {
    ASSERT_NO_FATAL_FAILURE(start(
        {},
        [](const Config &, const Config &candidate,
           const LiveConfig::Prepared &) -> std::expected<void, Diagnostic> {
            if (candidate.window.gaps_outer == 45)
                return std::unexpected(Diagnostic{"", 0, 0, "monitors", "test rejected output"});
            return {};
        }));
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=45\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    EXPECT_EQ(transaction().diagnostic()->path, paths.user.string());
    EXPECT_EQ(transaction().active().window.gaps_outer, 10);
    EXPECT_EQ(snapshot(), default_template());

    io->fail.store(true);
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=46\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().persistence_error().has_value(); }));
    EXPECT_EQ(transaction().active().window.gaps_outer, 46);
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), default_template());
}
} // namespace

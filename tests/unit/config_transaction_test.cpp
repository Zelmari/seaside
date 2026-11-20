#include "config_transaction.hpp"
#include "state.hpp"
#include "test_directory.hpp"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <wayland-server-core.h>

namespace {
using namespace seaside;
LoadedConfig startup(std::string_view text = "") {
    auto parsed = parse_config(text);
    EXPECT_TRUE(parsed);
    return {*parsed, std::string(text), ConfigSource::user, {}};
}
ConfigCandidate candidate(std::uint64_t generation, std::string text) {
    auto parsed = parse_config(text);
    EXPECT_TRUE(parsed);
    return {generation, *parsed, std::move(text)};
}
auto schema_only = [](const Config &, const Config &) -> std::expected<void, Diagnostic> {
    return {};
};

TEST(ConfigTransaction, RestartOnlyRequestedModeAndNightLightOverrides) {
    ConfigTransaction transaction(startup());
    transaction.set_runtime_night_light(true);
    auto generation = *transaction.begin_reload();
    auto result = transaction.commit(
        candidate(generation, "[workspaces]\nmode='per-monitor'\n[window]\ngaps_outer=14\n"),
        schema_only);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->restart_pending);
    EXPECT_FALSE(result->reset_tiling_overrides);
    EXPECT_EQ(transaction.active().workspaces.mode, WorkspaceMode::global);
    EXPECT_EQ(transaction.requested().workspaces.mode, WorkspaceMode::per_monitor);
    EXPECT_EQ(transaction.active().window.gaps_outer, 14);
    EXPECT_TRUE(transaction.runtime_night_light());
    // A new process activates the persisted requested mode.
    ConfigTransaction restarted(startup("[workspaces]\nmode='per-monitor'\n"));
    EXPECT_EQ(restarted.active().workspaces.mode, WorkspaceMode::per_monitor);
    EXPECT_FALSE(restarted.restart_pending());
    EXPECT_FALSE(restarted.runtime_night_light());
}
TEST(ConfigTransaction, OnlyCommittedLayoutOrTilingChangesResetOverrides) {
    ConfigTransaction transaction(startup());
    auto generation = *transaction.begin_reload();
    auto changed = candidate(generation, "[window]\nlayout='scrolling'\n");
    auto rejected = transaction.commit(
        changed, [](const Config &, const Config &) -> std::expected<void, Diagnostic> {
            return std::unexpected(
                Diagnostic{"config", 1, 1, "wallpaper.path", "simulated preparation failure"});
        });
    EXPECT_FALSE(rejected);
    EXPECT_EQ(transaction.active().window.layout, Layout::dwindle);
    ASSERT_TRUE(transaction.diagnostic());
    auto committed = transaction.commit(changed, schema_only);
    ASSERT_TRUE(committed);
    EXPECT_TRUE(committed->reset_tiling_overrides);
    EXPECT_FALSE(transaction.diagnostic());
    generation = *transaction.begin_reload();
    auto unchanged =
        transaction.commit(candidate(generation, "[window]\nlayout='scrolling'\n"), schema_only);
    ASSERT_TRUE(unchanged);
    EXPECT_FALSE(unchanged->reset_tiling_overrides);
}
TEST(ConfigTransaction, StaleParseAndPersistenceResultsCannotReplaceNewState) {
    ConfigTransaction transaction(startup());
    auto first = *transaction.begin_reload();
    auto stale = candidate(first, "[window]\ngaps_outer=1\n");
    auto second = *transaction.begin_reload();
    EXPECT_FALSE(transaction.commit(stale, schema_only));
    ASSERT_TRUE(transaction.commit(candidate(second, "[window]\ngaps_outer=2\n"), schema_only));
    transaction.reject(first, Diagnostic{"bad", 1, 1, "", "old error"});
    transaction.persisted(
        first, StorageError{StorageStep::write, "old", std::make_error_code(std::errc::io_error)});
    EXPECT_FALSE(transaction.diagnostic());
    EXPECT_FALSE(transaction.persistence_error());
    EXPECT_EQ(transaction.active().window.gaps_outer, 2);
    transaction.persisted(second, {});
    EXPECT_TRUE(transaction.recovery_persisted());
}
TEST(ConfigTransaction, SnapshotFailureKeepsCommittedStateAndSeparateDiagnostic) {
    ConfigTransaction transaction(startup());
    auto generation = *transaction.begin_reload();
    ASSERT_TRUE(
        transaction.commit(candidate(generation, "[window]\ngaps_inner=21\n"), schema_only));
    transaction.persisted(generation,
                          StorageError{StorageStep::sync_file, "recovery",
                                       std::make_error_code(std::errc::no_space_on_device)});
    EXPECT_EQ(transaction.active().window.gaps_inner, 21);
    EXPECT_FALSE(transaction.recovery_persisted());
    EXPECT_TRUE(transaction.persistence_error());
    EXPECT_FALSE(transaction.diagnostic());
    StorageError warning{StorageStep::cleanup, "recovery",
                         std::make_error_code(std::errc::io_error)};
    warning.publication_committed = true;
    warning.cleanup_failed = true;
    transaction.persisted(generation, warning);
    EXPECT_TRUE(transaction.recovery_persisted());
    EXPECT_TRUE(transaction.persistence_error());
    EXPECT_EQ(transaction.active().window.gaps_inner, 21);
}
TEST(ConfigTransaction, UnchangedConfiguredNightLightPreservesSessionOverride) {
    ConfigTransaction transaction(startup());
    transaction.set_runtime_night_light(true);
    auto g = *transaction.begin_reload();
    ASSERT_TRUE(transaction.commit(candidate(g, "[night_light]\nenabled=true\n"), schema_only));
    transaction.set_runtime_night_light(false);
    g = *transaction.begin_reload();
    ASSERT_TRUE(transaction.commit(
        candidate(g, "[night_light]\nenabled=true\ntemperature_kelvin=5000\n"), schema_only));
    EXPECT_FALSE(transaction.runtime_night_light());
    g = *transaction.begin_reload();
    auto changed = transaction.commit(candidate(g, "[night_light]\nenabled=false\n"), schema_only);
    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed->night_light_changed);
    EXPECT_FALSE(transaction.runtime_night_light());
}
TEST(ConfigTransaction, PreparationExceptionsAndReentrantGenerationDoNotCommit) {
    ConfigTransaction transaction(startup());
    auto g = *transaction.begin_reload();
    auto data = candidate(g, "[window]\ngaps_outer=99\n");
    EXPECT_FALSE(transaction.commit(
        data, [](const Config &, const Config &) -> std::expected<void, Diagnostic> {
            throw std::runtime_error("prepare");
        }));
    EXPECT_EQ(transaction.active().window.gaps_outer, 10);
    EXPECT_FALSE(transaction.commit(
        data, [&](const Config &, const Config &) -> std::expected<void, Diagnostic> {
            EXPECT_TRUE(transaction.begin_reload());
            return {};
        }));
    EXPECT_EQ(transaction.active().window.gaps_outer, 10);
}
TEST(ConfigTransaction, NonOwnerMutationsAreRejectedAndParsingDoesNotExecuteCommands) {
    ConfigTransaction transaction(startup());
    bool rejected = false;
    std::jthread worker([&] {
        rejected = !transaction.begin_reload();
        transaction.set_runtime_night_light(true);
    });
    worker.join();
    EXPECT_TRUE(rejected);
    EXPECT_FALSE(transaction.runtime_night_light());
    seaside::test::Directory directory;
    auto sentinel = directory.path() / "must-not-exist";
    auto g = *transaction.begin_reload();
    std::string text = "[autostart]\ncommands=['touch " + sentinel.string() +
                       "']\n[keybinds]\n'Super+T'='exec touch " + sentinel.string() + "'\n";
    ASSERT_TRUE(transaction.commit(candidate(g, text), schema_only));
    EXPECT_FALSE(std::filesystem::exists(sentinel));
}

TEST(ConfigTransaction, ReentrantSuccessCannotBeOverwrittenByStaleErrorsOrDuplicateCommit) {
    ConfigTransaction transaction(startup());
    auto first = *transaction.begin_reload();
    auto outer = candidate(first, "[window]\ngaps_outer=30\n");
    auto failed = transaction.commit(
        outer, [&](const Config &, const Config &) -> std::expected<void, Diagnostic> {
            auto second = *transaction.begin_reload();
            EXPECT_TRUE(
                transaction.commit(candidate(second, "[window]\ngaps_outer=40\n"), schema_only));
            return std::unexpected(Diagnostic{"stale", 1, 1, "", "old preparation error"});
        });
    EXPECT_FALSE(failed);
    EXPECT_EQ(transaction.active().window.gaps_outer, 40);
    EXPECT_FALSE(transaction.diagnostic());
    EXPECT_FALSE(transaction.commit(
        candidate(transaction.generation(), "[window]\ngaps_outer=99\n"), schema_only));
    EXPECT_EQ(transaction.active().window.gaps_outer, 40);
}

TEST(ConfigTransaction, MissingPreparationCannotBypassValidationAndIoDiagnosticsAreBounded) {
    ConfigTransaction transaction(startup());
    auto g = *transaction.begin_reload();
    EXPECT_FALSE(transaction.commit(candidate(g, "[window]\ngaps_outer=99\n"), {}));
    EXPECT_EQ(transaction.active().window.gaps_outer, 10);
    std::string path = "/tmp/";
    for (unsigned i = 0; i < 3000; ++i)
        path += "界";
    auto result = load_config_candidate(ConfigStore{}, path, g);
    ASSERT_FALSE(result);
    EXPECT_LE(result.error().path.size(), 4096U);
    EXPECT_LE(result.error().message.size(), 512U);
    EXPECT_EQ((result.error().path.size() - 5) % 3, 0U);
}

class AsyncConfigTest : public testing::Test {
  protected:
    void SetUp() override {
        loop.reset(wl_event_loop_create());
        ASSERT_TRUE(loop);
        auto created = WorkQueue::create(loop.get());
        ASSERT_TRUE(created);
        queue = std::move(*created);
    }
    void pump_until(const bool &complete) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!complete && std::chrono::steady_clock::now() < deadline) {
            ASSERT_GE(wl_event_loop_dispatch(loop.get(), 10), 0);
        }
        EXPECT_TRUE(complete);
    }
    seaside::test::Directory directory;
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> loop{nullptr,
                                                                          wl_event_loop_destroy};
    std::unique_ptr<WorkQueue> queue;
};

TEST_F(AsyncConfigTest, RealLoadCommitStateEffectAndSnapshotThroughMainLoop) {
    ConfigStore store;
    auto user = directory.path() / "config.toml";
    auto recovery = directory.path() / "last-good.toml";
    std::string text = "[workspaces]\nmode='per-monitor'\n[window]\nlayout='scrolling'\n";
    ASSERT_TRUE(store.publish(user, text, PublishMode::create_user_file));
    ConfigTransaction transaction(startup());
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    auto id = state.create_window(1);
    ASSERT_TRUE(id);
    ASSERT_TRUE(state.set_tiling_override(*id, false));
    AsyncConfigIo async(*queue, store);
    auto g = *transaction.begin_reload();
    bool complete = false;
    const auto owner = std::this_thread::get_id();
    ASSERT_TRUE(async.load(user, g, [&](auto result) {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        ASSERT_TRUE(result);
        auto effects = transaction.commit(std::move(*result), schema_only);
        ASSERT_TRUE(effects);
        if (effects->reset_tiling_overrides)
            state.clear_tiling_overrides();
        ASSERT_TRUE(async.persist(recovery, text, g, [&](auto error) {
            transaction.persisted(g, std::move(error));
            complete = true;
        }));
    }));
    pump_until(complete);
    EXPECT_TRUE(transaction.recovery_persisted());
    EXPECT_TRUE(transaction.restart_pending());
    EXPECT_FALSE(state.window(*id)->tiling_override);
    EXPECT_EQ(**store.read(recovery, true), text);
}
TEST_F(AsyncConfigTest, NewGenerationDiscardsOldLoadAndMissingInputIsDiagnostic) {
    ConfigStore store;
    AsyncConfigIo async(*queue, store);
    bool old_delivered = false, complete = false;
    ASSERT_TRUE(async.load(directory.path() / "old", 1, [&](auto) { old_delivered = true; }));
    ASSERT_TRUE(async.load(directory.path() / "new", 2, [&](auto result) {
        EXPECT_FALSE(result);
        if (!result) {
            EXPECT_EQ(result.error().path, (directory.path() / "new").string());
        }
        complete = true;
    }));
    pump_until(complete);
    EXPECT_FALSE(old_delivered);
    bool persisted = false;
    EXPECT_TRUE(
        async.persist(directory.path() / "recovery", "", 2, [&](auto) { persisted = true; }));
    pump_until(persisted);
    EXPECT_FALSE(async.persist(directory.path() / "recovery", "", 1, [](auto) {}));
}
TEST_F(AsyncConfigTest, DestroyedAsyncRecipientNeverReceivesQueuedCompletion) {
    bool delivered = false;
    {
        AsyncConfigIo async(*queue, ConfigStore{});
        ASSERT_TRUE(async.load(directory.path() / "missing", 1, [&](auto) { delivered = true; }));
    }
    EXPECT_TRUE(queue->shutdown());
    EXPECT_GE(wl_event_loop_dispatch(loop.get(), 0), 0);
    EXPECT_FALSE(delivered);
}

TEST_F(AsyncConfigTest, EmptyCallbacksAndNonOwnerRequestsCannotQueueWork) {
    ConfigStore store;
    AsyncConfigIo async(*queue, store);
    EXPECT_EQ(async.load(directory.path() / "missing", 1, {}).error(), WorkError::invalid_job);
    EXPECT_EQ(async.persist(directory.path() / "recovery", "", 1, {}).error(),
              WorkError::invalid_job);
    bool rejected = false;
    std::jthread worker([&] {
        rejected = async.load(directory.path() / "missing", 1, [](auto) {}).error() ==
                   WorkError::wrong_thread;
    });
    worker.join();
    EXPECT_TRUE(rejected);
    EXPECT_EQ(queue->stats().submitted, 0U);
    bool complete = false;
    EXPECT_TRUE(async.load(directory.path() / "missing", 1, [&](auto) { complete = true; }));
    EXPECT_EQ(async.load(directory.path() / "missing", 1, [](auto) {}).error(),
              WorkError::cancelled);
    pump_until(complete);
}

class PausedSaveIo : public StorageIo {
  public:
    std::error_code check(StorageStep step) noexcept override {
        if (step != StorageStep::sync_file)
            return {};
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return released; }))
            return std::make_error_code(std::errc::timed_out);
        return {};
    }
    bool wait_until_entered() {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

  private:
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
};

TEST_F(AsyncConfigTest, InvalidNewAttemptDoesNotCancelPriorValidSnapshot) {
    auto io = std::make_shared<PausedSaveIo>();
    ConfigStore store(io);
    AsyncConfigIo async(*queue, store);
    ConfigTransaction transaction(startup());
    auto first = *transaction.begin_reload();
    std::string valid = "[window]\ngaps_outer=17\n";
    ASSERT_TRUE(transaction.commit(candidate(first, valid), schema_only));
    bool saved = false;
    auto recovery = directory.path() / "recovery";
    ASSERT_TRUE(async.persist(recovery, valid, first, [&](auto error) {
        transaction.persisted(first, std::move(error));
        saved = true;
    }));
    ASSERT_TRUE(io->wait_until_entered());
    auto second = *transaction.begin_reload();
    bool invalid = false;
    ASSERT_TRUE(async.load(directory.path() / "missing", second, [&](auto result) {
        EXPECT_FALSE(result);
        if (!result)
            transaction.reject(second, result.error());
        invalid = true;
    }));
    pump_until(invalid);
    io->release();
    pump_until(saved);
    EXPECT_EQ(transaction.active().window.gaps_outer, 17);
    EXPECT_TRUE(transaction.diagnostic());
    EXPECT_TRUE(transaction.recovery_persisted());
    EXPECT_EQ(**store.read(recovery, true), valid);
}
} // namespace

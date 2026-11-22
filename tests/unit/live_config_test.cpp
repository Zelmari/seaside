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
        if (step == StorageStep::check_writer) {
            ++probes;
            if (leases_unavailable.load())
                return std::make_error_code(std::errc::operation_not_supported);
        }
        if (step == StorageStep::sync_file && fail.load())
            return std::make_error_code(std::errc::no_space_on_device);
        return {};
    }
    std::atomic<bool> fail = false;
    std::atomic<bool> leases_unavailable = false;
    std::atomic<unsigned> probes = 0;
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
        start_from(load_startup_config(store, paths), std::move(prepare), std::move(apply));
    }

    void start_from(LoadedConfig loaded, LiveConfig::Prepare prepare = {},
                    LiveConfig::Apply apply = {}) {
        ASSERT_TRUE(loop);
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

TEST_F(LiveConfigTest, RecreatedConfigWaitsForTheCompletedWrite) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    const std::string previous = "[window]\ngaps_outer=29\n[keybinds]\n";
    ASSERT_NO_FATAL_FAILURE(replace(previous));
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 29 && transaction().recovery_persisted();
    }));
    ASSERT_TRUE(std::filesystem::remove(paths.user));
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));

    auto replacement = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=49\n";
    ASSERT_TRUE(replacement << prefix << std::flush);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer != 29; }, 400ms));
    EXPECT_TRUE(transaction().active().keybinds.empty());
    EXPECT_TRUE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), previous);

    ASSERT_TRUE(replacement << "[keybinds]\n");
    replacement.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 49 && transaction().recovery_persisted();
    }));
    EXPECT_TRUE(transaction().active().keybinds.empty());
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

TEST_F(LiveConfigTest, AtomicReplacementSupersedesAnOpenWriter) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));

    auto writer = std::ofstream(paths.user);
    ASSERT_TRUE(writer << "[window]\ngaps_outer=37\n" << std::flush);
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=39\n"));
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 39 && transaction().recovery_persisted();
    }));
    writer.close();
    EXPECT_EQ(snapshot(), "[window]\ngaps_outer=39\n");
}

TEST_F(LiveConfigTest, SupersededWorkerCannotApplyWhileNewWriteRemainsOpen) {
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
    auto writer = std::ofstream(paths.user);
    ASSERT_TRUE(writer << latest << std::flush);
    ASSERT_TRUE(pump_until([&] { return transaction().generation() > older; }));
    {
        std::scoped_lock lock(gate->mutex);
        gate->release = true;
    }
    gate->condition.notify_one();
    EXPECT_FALSE(pump_until([&] { return !applied.empty(); }, 400ms));
    EXPECT_EQ(snapshot(), default_template());
    writer.close();
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

TEST_F(LiveConfigTest, SaveBetweenInitialLoadAndWatchRegistrationIsApplied) {
    auto startup = load_startup_config(store, paths);
    EXPECT_EQ(startup.config.window.gaps_outer, 10);
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=38\n"));
    ASSERT_NO_FATAL_FAILURE(start_from(std::move(startup)));
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 38 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), "[window]\ngaps_outer=38\n");
}

TEST_F(LiveConfigTest, ConfigSymlinkFollowsInPlaceWritesAndTargetReplacement) {
    std::filesystem::create_directories(paths.user.parent_path());
    const auto linked_config = directory.path() / "dotfile.toml";
    ASSERT_TRUE(std::ofstream(linked_config) << "[window]\ngaps_outer=19\n");
    std::filesystem::create_symlink(
        std::filesystem::relative(linked_config, paths.user.parent_path()), paths.user);
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(transaction().active().window.gaps_outer, 19);

    ASSERT_TRUE(std::ofstream(linked_config) << "[window]\ngaps_outer=37\n");
    ASSERT_TRUE(pump_until([&] { return transaction().active().window.gaps_outer == 37; }));
    const auto replacement = directory.path() / "editor.tmp";
    ASSERT_TRUE(std::ofstream(replacement) << "[window]\ngaps_outer=39\n");
    std::filesystem::rename(replacement, linked_config);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 39 && transaction().recovery_persisted();
    }));

    ASSERT_TRUE(std::filesystem::remove(linked_config));
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    auto writer = std::ofstream(linked_config);
    ASSERT_TRUE(writer << "[window]\ngaps_outer=41\n" << std::flush);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer != 39; }, 400ms));
    EXPECT_EQ(snapshot(), "[window]\ngaps_outer=39\n");
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 41 && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_TRUE(std::filesystem::is_symlink(paths.user));

    ASSERT_TRUE(std::filesystem::remove(paths.user));
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    ASSERT_TRUE(std::ofstream(linked_config) << "[window]\ngaps_outer=43\n");
    std::filesystem::create_symlink(linked_config, paths.user);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 43 && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
}

TEST_F(LiveConfigTest, IntermediateProfileSwitchTracksTheNewTargetAndItsRepairs) {
    const auto profiles = directory.path() / "profiles";
    const auto files = directory.path() / "files";
    std::filesystem::create_directories(paths.user.parent_path());
    std::filesystem::create_directory(profiles);
    std::filesystem::create_directory(files);
    const std::string work = "[window]\ngaps_outer=21\n";
    const std::string home = "[window]\ngaps_outer=34\n";
    ASSERT_TRUE(std::ofstream(files / "work") << work);
    ASSERT_TRUE(std::ofstream(files / "home") << home);
    std::filesystem::create_symlink("../../profiles/active", paths.user);
    std::filesystem::create_symlink("../files/work", profiles / "active");
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    // Let the post-startup rescan settle before switching the intermediate link.
    EXPECT_FALSE(pump_until([&] { return transaction().diagnostic().has_value(); }, 250ms));
    ASSERT_EQ(snapshot(), work);

    std::filesystem::create_symlink("../files/home", profiles / "next");
    std::filesystem::rename(profiles / "next", profiles / "active");
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 34 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), home);
    ASSERT_TRUE(std::ofstream(files / "home") << "[window]\ngaps_outer=false\n");
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    EXPECT_EQ(transaction().active().window.gaps_outer, 34);
    EXPECT_EQ(snapshot(), home);
    const std::string repaired = "[window]\ngaps_outer=35\n";
    ASSERT_TRUE(std::ofstream(files / "home") << repaired);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 35 && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), repaired);
}

TEST_F(LiveConfigTest, ProfileLinkCanRecoverFromSelectingItsOwnWatchedDirectory) {
    const auto profiles = directory.path() / "profiles";
    std::filesystem::create_directories(paths.user.parent_path());
    std::filesystem::create_directory(profiles);
    std::filesystem::create_symlink("../../profiles/active", paths.user);
    const auto selected = profiles / "active";
    const std::string valid = "[window]\ngaps_outer=23\n";
    ASSERT_TRUE(std::ofstream(profiles / "valid") << valid);
    std::filesystem::create_symlink("valid", selected);
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));

    std::filesystem::create_symlink(".", profiles / "next");
    std::filesystem::rename(profiles / "next", selected);
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    EXPECT_EQ(snapshot(), valid);
    std::filesystem::create_symlink("valid", profiles / "next");
    std::filesystem::rename(profiles / "next", selected);
    ASSERT_TRUE(pump_until(
        [&] { return !transaction().diagnostic() && transaction().recovery_persisted(); }));
    EXPECT_EQ(snapshot(), valid);
}

TEST_F(LiveConfigTest, PathTruncatePublishesTheShorterConfiguration) {
    const std::string prefix = "[window]\ngaps_outer=25\n";
    ASSERT_TRUE(
        store.publish(paths.user, prefix + "gaps_inner=17\n", PublishMode::create_user_file));
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_EQ(transaction().active().window.gaps_inner, 17);
    EXPECT_FALSE(pump_until([&] { return transaction().diagnostic().has_value(); }, 250ms));
    std::filesystem::resize_file(paths.user, prefix.size());
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_inner == default_config().window.gaps_inner &&
               transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix);
}

TEST_F(LiveConfigTest, HardLinkPublicationSurvivesRemovalOfTheStagingName) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    const auto stage = directory.path() / "stage";
    const std::string text = "[window]\ngaps_outer=42\n";
    ASSERT_TRUE(std::ofstream(stage) << text);
    std::filesystem::remove(paths.user);
    std::filesystem::create_hard_link(stage, paths.user);
    std::filesystem::remove(stage);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 42 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), text);
}

class WriterCloseTest : public LiveConfigTest, public testing::WithParamInterface<bool> {};

TEST_P(WriterCloseTest, ACloseFromOneWriterCannotPublishAnotherWritersPrefix) {
    io->leases_unavailable = GetParam();
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    const auto alias = directory.path() / "also-open";
    std::filesystem::create_hard_link(paths.user, alias);
    auto writer = std::ofstream(alias);
    const std::string prefix = "[window]\ngaps_outer=33\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    // The second writable description closes, while the prefix writer stays open.
    ASSERT_TRUE(std::ofstream(paths.user, std::ios::app).good());
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 33; }, 500ms));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), default_template());
    writer << "[keybinds]\n";
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 33 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

INSTANTIATE_TEST_SUITE_P(LeaseSupport, WriterCloseTest, testing::Bool());

TEST_F(LiveConfigTest, UnsupportedLeasesKeepUnconfirmedEditsPendingUntilAnOrdinarySave) {
    io->leases_unavailable = true;
    ASSERT_NO_FATAL_FAILURE(start());
    const std::string prefix = "[window]\ngaps_outer=31\n";
    const std::string prior = prefix + "gaps_inner=18\n";
    ASSERT_NO_FATAL_FAILURE(replace(prior));
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_inner == 18 && transaction().recovery_persisted();
    }));
    std::filesystem::resize_file(paths.user, prefix.size());
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_inner != 18; }, 500ms));
    EXPECT_EQ(snapshot(), prior);
    ASSERT_TRUE(std::ofstream(paths.user) << prefix);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_inner == default_config().window.gaps_inner &&
               transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix);
}

TEST_F(LiveConfigTest, AnOpenWriterAtStartupUsesRecoveryUntilTheFirstCompletedSave) {
    const std::string prior = "[window]\ngaps_outer=18\n";
    ASSERT_TRUE(store.publish(paths.recovery, prior, PublishMode::replace_snapshot));
    std::filesystem::create_directories(paths.user.parent_path());
    auto writer = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=47\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(live->source(), ConfigSource::recovery);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer != 18; }, 500ms));
    EXPECT_EQ(snapshot(), prior);
    writer << "[keybinds]\n";
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 47 && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

class NoLeaseLiveConfigTest : public LiveConfigTest {
  protected:
    void SetUp() override { io->leases_unavailable = true; }
};

TEST_F(NoLeaseLiveConfigTest, StartupKeepsDurableSnapshotWhenDifferentUserBytesCannotBeVerified) {
    const std::string prior = "[window]\ngaps_outer=23\n";
    ASSERT_TRUE(store.publish(paths.recovery, prior, PublishMode::replace_snapshot));
    std::filesystem::create_directories(paths.user.parent_path());
    auto writer = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=41\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(live->source(), ConfigSource::recovery);
    ASSERT_TRUE(transaction().diagnostic());
    EXPECT_NE(transaction().diagnostic()->message.find("cannot verify a completed save"),
              std::string::npos);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer != 23; }, 500ms));
    EXPECT_EQ(snapshot(), prior);
    writer << "[keybinds]\n";
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return live->source() == ConfigSource::user && transaction().recovery_persisted();
    }));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

TEST_F(NoLeaseLiveConfigTest, RetiredDirectoryCloseCannotAuthorizeItsReplacement) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    auto old = std::ofstream(paths.user);
    ASSERT_TRUE(old << "[window]\ngaps_outer=29\n" << std::flush);
    std::filesystem::rename(paths.user.parent_path(), directory.path() / "retired");
    std::filesystem::create_directory(paths.user.parent_path());
    auto writer = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=49\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    old.close();
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 49; }, 500ms));
    EXPECT_EQ(snapshot(), default_template());
    writer << "[keybinds]\n";
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 49 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

TEST_F(NoLeaseLiveConfigTest, UnlinkedInodeCloseCannotAuthorizeRecreatedName) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    auto old = std::ofstream(paths.user);
    ASSERT_TRUE(old << "[window]\ngaps_outer=36\n" << std::flush);
    std::filesystem::remove(paths.user);
    auto writer = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=48\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 48; }, 400ms));
    old.close();
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 48; }, 400ms));
    EXPECT_EQ(snapshot(), default_template());
    writer << "[keybinds]\n";
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 48 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

TEST_F(NoLeaseLiveConfigTest, AnUnobservedSaveInANewDirectoryRecoversOnTheNextWatchedRename) {
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    std::filesystem::rename(paths.user.parent_path(), directory.path() / "retired");
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    std::filesystem::create_directory(paths.user.parent_path());
    const std::string candidate = "[window]\ngaps_outer=37\n";
    const auto previous = io->probes.load();
    ASSERT_NO_FATAL_FAILURE(replace(candidate));
    ASSERT_TRUE(pump_until([&] { return io->probes.load() > previous; }));
    const auto deferred = io->probes.load();
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 37; }, 500ms));
    EXPECT_EQ(snapshot(), default_template());
    EXPECT_EQ(io->probes.load(), deferred);
    ASSERT_NO_FATAL_FAILURE(replace(candidate));
    ASSERT_TRUE(pump_until(
        [&] { return !transaction().diagnostic() && transaction().recovery_persisted(); }));
    EXPECT_EQ(snapshot(), candidate);
}

TEST_F(NoLeaseLiveConfigTest, AnUnverifiedNewSymlinkRecoversOnWatchedLinkAndTargetRenames) {
    const auto target = directory.path() / "target";
    const std::string first = "[window]\ngaps_outer=34\n";
    const std::string second = "[window]\ngaps_outer=44\n";
    std::filesystem::create_directories(paths.user.parent_path());
    ASSERT_TRUE(std::ofstream(target) << first);
    std::filesystem::create_symlink(target, paths.user);
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    ASSERT_EQ(snapshot(), first);
    std::filesystem::remove(paths.user);
    ASSERT_TRUE(pump_until([&] { return transaction().diagnostic().has_value(); }));
    ASSERT_TRUE(std::ofstream(target) << second);
    std::filesystem::create_symlink(target, paths.user);
    EXPECT_FALSE(pump_until([&] { return !transaction().diagnostic(); }, 500ms));
    EXPECT_EQ(snapshot(), first);
    auto staged = paths.user.parent_path() / "profile.next";
    std::filesystem::create_symlink(target, staged);
    std::filesystem::rename(staged, paths.user);
    ASSERT_TRUE(pump_until(
        [&] { return !transaction().diagnostic() && transaction().recovery_persisted(); }));
    EXPECT_EQ(snapshot(), second);
    staged = directory.path() / "target.next";
    ASSERT_TRUE(std::ofstream(staged) << first);
    std::filesystem::rename(staged, target);
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 34 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), first);
}

TEST_F(NoLeaseLiveConfigTest, RelinkingTheSameInodePreservesObservedWriterActivity) {
    const auto target = directory.path() / "target";
    std::filesystem::create_directories(paths.user.parent_path());
    ASSERT_TRUE(std::ofstream(target) << default_template());
    std::filesystem::create_symlink(target, paths.user);
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    auto writer = std::ofstream(target);
    const std::string prefix = "[window]\ngaps_outer=32\n";
    ASSERT_TRUE(writer << prefix << std::flush);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 32; }, 400ms));
    const auto next = paths.user.parent_path() / "next";
    std::filesystem::create_symlink(target, next);
    std::filesystem::rename(next, paths.user);
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 32; }, 400ms));
    EXPECT_EQ(snapshot(), default_template());
    writer << "[keybinds]\n" << std::flush;
    // Both events land in the same batch with the same inode still selected.
    std::filesystem::create_symlink(target, next);
    std::filesystem::rename(next, paths.user);
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 32 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}

int inotify_queue_limit() {
    int queue_limit = 0;
    std::ifstream("/proc/sys/fs/inotify/max_queued_events") >> queue_limit;
    return queue_limit <= 100'000 ? queue_limit : 0;
}

void fill_queue(const std::filesystem::path &dir, int queue_limit) {
    auto first = dir / "flood-a";
    auto second = dir / "flood-b";
    ASSERT_TRUE(std::ofstream(first) << "noise");
    // Each rename contributes two records with an independent cookie. With
    // dispatch paused, the subsequent save notification is replaced by overflow.
    for (int i = 0; i <= queue_limit / 2; ++i) {
        std::filesystem::rename(first, second);
        std::swap(first, second);
    }
}

TEST_F(LiveConfigTest, RescansCurrentPathAfterAnInotifyOverflow) {
    const int queue_limit = inotify_queue_limit();
    if (queue_limit <= 0)
        GTEST_SKIP() << "The system inotify queue limit is unavailable or impractical to fill";
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    EXPECT_FALSE(pump_until([&] { return transaction().diagnostic().has_value(); }, 250ms));
    ASSERT_NO_FATAL_FAILURE(fill_queue(paths.user.parent_path(), queue_limit));
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=53\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().active().window.gaps_outer == 53; }));
    ASSERT_NO_FATAL_FAILURE(replace("[window]\ngaps_outer=57\n"));
    ASSERT_TRUE(pump_until([&] { return transaction().active().window.gaps_outer == 57; }));
}

TEST_F(LiveConfigTest, OverflowPreservesOpenPrefixAndRecoversFromLostWriteClose) {
    const int queue_limit = inotify_queue_limit();
    if (queue_limit <= 0)
        GTEST_SKIP() << "The system inotify queue limit is unavailable or impractical to fill";
    ASSERT_NO_FATAL_FAILURE(start());
    ASSERT_TRUE(pump_until([&] { return transaction().recovery_persisted(); }));
    auto writer = std::ofstream(paths.user);
    const std::string prefix = "[window]\ngaps_outer=27\n";
    auto generation = transaction().generation();
    ASSERT_TRUE(writer << prefix << std::flush);
    ASSERT_TRUE(pump_until([&] { return transaction().generation() > generation; }));

    ASSERT_NO_FATAL_FAILURE(fill_queue(paths.user.parent_path(), queue_limit));
    EXPECT_FALSE(pump_until([&] { return transaction().active().window.gaps_outer == 27; }, 650ms));
    EXPECT_FALSE(transaction().diagnostic());
    EXPECT_EQ(snapshot(), default_template());

    // Let inotify deliver the modification, then fill its queue before closing.
    generation = transaction().generation();
    ASSERT_TRUE(writer << "[keybinds]\n" << std::flush);
    ASSERT_TRUE(pump_until([&] { return transaction().generation() > generation; }));
    ASSERT_NO_FATAL_FAILURE(fill_queue(paths.user.parent_path(), queue_limit));
    writer.close();
    ASSERT_TRUE(pump_until([&] {
        return transaction().active().window.gaps_outer == 27 && transaction().recovery_persisted();
    }));
    EXPECT_EQ(snapshot(), prefix + "[keybinds]\n");
}
} // namespace

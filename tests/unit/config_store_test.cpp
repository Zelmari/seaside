#include "config_store.hpp"
#include "config_transaction.hpp"
#include "test_directory.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
class FaultIo : public seaside::StorageIo {
  public:
    std::optional<seaside::StorageStep> fail;
    bool interrupt_read = false, interrupt_write = false, short_io = false;
    unsigned ancestor_syncs = 0;
    std::function<void(seaside::StorageStep)> observe;
    std::error_code check(seaside::StorageStep step) noexcept override {
        if (step == seaside::StorageStep::sync_ancestor)
            ++ancestor_syncs;
        if (observe)
            observe(step);
        if (fail == step)
            return std::make_error_code(std::errc::io_error);
        return {};
    }
    ssize_t read(int fd, std::span<char> bytes) noexcept override {
        if (std::exchange(interrupt_read, false)) {
            errno = EINTR;
            return -1;
        }
        return StorageIo::read(fd, short_io ? bytes.first(1) : bytes);
    }
    ssize_t write(int fd, std::span<const char> bytes) noexcept override {
        if (std::exchange(interrupt_write, false)) {
            errno = EINTR;
            return -1;
        }
        return StorageIo::write(fd, short_io ? bytes.first(1) : bytes);
    }
};
class StoreTest : public testing::Test {
  protected:
    void write(const std::filesystem::path &path, std::string_view text) {
        std::ofstream output(path);
        output << text;
        ASSERT_TRUE(output.good());
    }
    std::string read(const std::filesystem::path &path) {
        std::ifstream input(path);
        return {std::istreambuf_iterator<char>(input), {}};
    }
    seaside::test::Directory temporary;
    const std::filesystem::path root = temporary.path();
};

TEST_F(StoreTest, MissingEmptyAndBoundedRegularFiles) {
    seaside::ConfigStore store;
    auto path = root / "config.toml";
    auto missing = store.read(path);
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing->has_value());
    write(path, "");
    auto empty = store.read(path);
    ASSERT_TRUE(empty);
    ASSERT_TRUE(empty->has_value());
    EXPECT_EQ(**empty, "");
    write(path, std::string(seaside::max_config_bytes, '#'));
    auto maximum = store.read(path);
    ASSERT_TRUE(maximum);
    ASSERT_TRUE(maximum->has_value());
    EXPECT_EQ((**maximum).size(), seaside::max_config_bytes);
    write(path, std::string(seaside::max_config_bytes + 1, '#'));
    EXPECT_FALSE(store.read(path));
    EXPECT_FALSE(store.read(root));
    ASSERT_EQ(::mkfifo((root / "fifo").c_str(), 0600), 0);
    EXPECT_FALSE(store.read(root / "fifo"));
}
TEST_F(StoreTest, ReadLeaseRoutesBreakToReaderAndPreservesPendingSignal) {
    using namespace seaside;
    const auto path = root / "lease";
    write(path, "candidate");
    auto io = std::make_shared<FaultIo>();
    ConfigStore store{io};
    // Force the actual kernel to break the lease before the checked close.
    io->observe = [&](StorageStep step) {
        if (step != StorageStep::close_file)
            return;
        const int writer = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        EXPECT_EQ(writer, -1);
        if (writer >= 0)
            close(writer);
        else
            EXPECT_EQ(errno, EWOULDBLOCK);
        sigset_t pending;
        ASSERT_EQ(sigpending(&pending), 0);
        EXPECT_EQ(sigismember(&pending, SIGIO), 1);
    };
    std::jthread reader([&] {
        sigset_t signals, mask, pending;
        sigemptyset(&signals);
        sigaddset(&signals, SIGIO);
        ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &signals, nullptr), 0);
        ASSERT_TRUE(store.read(path, false, WriterCheck::required));
        ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &mask), 0);
        EXPECT_EQ(sigismember(&mask, SIGIO), 0);
        ASSERT_EQ(sigpending(&pending), 0);
        EXPECT_EQ(sigismember(&pending, SIGIO), 0);

        ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &signals, nullptr), 0);
        ASSERT_EQ(pthread_kill(pthread_self(), SIGIO), 0);
        ASSERT_TRUE(store.read(path, false, WriterCheck::required));
        ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &mask), 0);
        EXPECT_EQ(sigismember(&mask, SIGIO), 1);
        ASSERT_EQ(sigpending(&pending), 0);
        EXPECT_EQ(sigismember(&pending, SIGIO), 1);
    });
    reader.join();
    const int writer = open(path.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_GE(writer, 0);
    close(writer);
}

TEST_F(StoreTest, UserSymlinksAllowedAndRecoverySymlinksRejected) {
    seaside::ConfigStore store;
    write(root / "real", "[window]\ngaps_outer=3\n");
    std::filesystem::create_symlink(root / "real", root / "config");
    auto linked = store.read(root / "config");
    ASSERT_TRUE(linked);
    ASSERT_TRUE(linked->has_value());
    EXPECT_EQ(**linked, read(root / "real"));
    EXPECT_FALSE(store.read(root / "config", true));
    EXPECT_FALSE(store.publish(root / "config", "other", seaside::PublishMode::replace_snapshot));
    EXPECT_EQ(read(root / "real"), "[window]\ngaps_outer=3\n");
}
TEST_F(StoreTest, FirstUsePublicationNeverClobbersExistingFile) {
    seaside::ConfigStore store;
    auto path = root / "new" / "config.toml";
    auto first = store.publish(path, "first", seaside::PublishMode::create_user_file);
    ASSERT_TRUE(first);
    EXPECT_EQ(*first, seaside::PublishResult::written);
    auto second = store.publish(path, "second", seaside::PublishMode::create_user_file);
    ASSERT_TRUE(second);
    EXPECT_EQ(*second, seaside::PublishResult::already_exists);
    EXPECT_EQ(read(path), "first");
    struct stat mode{};
    ASSERT_EQ(::stat(path.c_str(), &mode), 0);
    EXPECT_EQ(mode.st_mode & 0777, 0600);
    ASSERT_EQ(::stat(path.parent_path().c_str(), &mode), 0);
    EXPECT_EQ(mode.st_mode & 0777, 0700);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(path.parent_path()), {}), 1);
}
TEST_F(StoreTest, ShortInterruptedIoAndChangingSource) {
    auto io = std::make_shared<FaultIo>();
    io->short_io = true;
    io->interrupt_read = true;
    io->interrupt_write = true;
    seaside::ConfigStore store(io);
    auto path = root / "config";
    ASSERT_TRUE(store.publish(path, "abcdef", seaside::PublishMode::replace_snapshot));
    auto interrupted = store.read(path);
    ASSERT_TRUE(interrupted);
    ASSERT_TRUE(interrupted->has_value());
    EXPECT_EQ(**interrupted, "abcdef");
    bool once = false;
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::read && !std::exchange(once, true))
            write(path, "different");
    };
    EXPECT_FALSE(store.read(path));
}
TEST_F(StoreTest, FailedPublicationRetainsOldSnapshotAndCleansTemporaryFiles) {
    using enum seaside::StorageStep;
    for (auto step : {open_directory, create_temporary, write, sync_file, close_file, publish,
                      sync_directory}) {
        SCOPED_TRACE(static_cast<int>(step));
        auto io = std::make_shared<FaultIo>();
        seaside::ConfigStore store(io);
        auto path = root / "last-good-config.toml";
        ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
        io->fail = step;
        auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().operation, step);
        EXPECT_FALSE(result.error().recovery_uncertain);
        EXPECT_EQ(this->read(path), "old");
        EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), {}), 1);
    }
}
TEST_F(StoreTest, CleanupFailurePreservesKnownDurablePublicationAndReportsRetainedBackup) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    ASSERT_TRUE(store.publish(root / "last-good", "old", seaside::PublishMode::replace_snapshot));
    io->fail = seaside::StorageStep::cleanup;
    auto result = store.publish(root / "last-good", "new", seaside::PublishMode::replace_snapshot);
    ASSERT_FALSE(result);
    EXPECT_FALSE(result.error().recovery_uncertain);
    EXPECT_TRUE(result.error().publication_committed);
    EXPECT_TRUE(result.error().cleanup_failed);
    EXPECT_EQ(read(root / "last-good"), "new");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), {}), 2);
}
TEST_F(StoreTest, StaleSnapshotWorkCannotPublish) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    auto path = root / "last-good";
    ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
    bool current = true;
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::sync_file)
            current = false;
    };
    EXPECT_FALSE(store.publish(path, "stale", seaside::PublishMode::replace_snapshot,
                               [&] { return current; }));
    EXPECT_EQ(this->read(path), "old");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), {}), 1);
}
TEST_F(StoreTest, StartupRecoveryPrecedencePreservesBadUserInput) {
    seaside::ConfigStore store;
    seaside::ConfigPaths paths{root / "user", root / "recovery"};
    auto first = seaside::load_startup_config(store, paths);
    EXPECT_EQ(first.source, seaside::ConfigSource::user);
    EXPECT_EQ(first.config, seaside::default_config());
    write(paths.user, "not = [ valid");
    write(paths.recovery, "[window]\ngaps_outer=24\n");
    auto recovered = seaside::load_startup_config(store, paths);
    EXPECT_EQ(recovered.source, seaside::ConfigSource::recovery);
    EXPECT_EQ(recovered.config.window.gaps_outer, 24);
    EXPECT_TRUE(recovered.diagnostic);
    EXPECT_EQ(read(paths.user), "not = [ valid");
    write(paths.recovery, "bad = [");
    auto defaults = seaside::load_startup_config(store, paths);
    EXPECT_EQ(defaults.source, seaside::ConfigSource::defaults);
    EXPECT_EQ(defaults.config, seaside::default_config());
    EXPECT_TRUE(defaults.diagnostic);
    write(paths.user, "[window]\ngaps_outer=7\n");
    EXPECT_EQ(seaside::load_startup_config(store, paths).config.window.gaps_outer, 7);
}
TEST_F(StoreTest, FailedGenerationUsesDefaultsWithoutCreatingUserFile) {
    auto io = std::make_shared<FaultIo>();
    io->fail = seaside::StorageStep::create_temporary;
    seaside::ConfigStore store(io);
    seaside::ConfigPaths paths{root / "user", root / "recovery"};
    auto loaded = seaside::load_startup_config(store, paths);
    EXPECT_EQ(loaded.source, seaside::ConfigSource::defaults);
    EXPECT_EQ(loaded.config, seaside::default_config());
    EXPECT_TRUE(loaded.diagnostic);
    EXPECT_FALSE(std::filesystem::exists(paths.user));
}
TEST_F(StoreTest, FirstUseRaceLoadsTheOtherWritersConfiguration) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    seaside::ConfigPaths paths{root / "user", root / "recovery"};
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::publish)
            write(paths.user, "[window]\ngaps_outer=31\n");
    };
    auto loaded = seaside::load_startup_config(store, paths);
    EXPECT_EQ(loaded.source, seaside::ConfigSource::user);
    EXPECT_EQ(loaded.config.window.gaps_outer, 31);
    EXPECT_EQ(read(paths.user), "[window]\ngaps_outer=31\n");
}
TEST_F(StoreTest, EarlyFailureAndTemporaryCleanupFailureKeepKnownOldSnapshot) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    auto path = root / "last-good";
    ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::write || step == seaside::StorageStep::cleanup)
            io->fail = step;
    };
    auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().operation, seaside::StorageStep::write);
    EXPECT_FALSE(result.error().recovery_uncertain);
    EXPECT_FALSE(result.error().publication_committed);
    EXPECT_TRUE(result.error().cleanup_failed);
    EXPECT_EQ(read(path), "old");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), {}), 2);
}

TEST_F(StoreTest, NestedDirectoryCreationIsSynchronizedBeforeFilePublication) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    auto path = root / "state" / "seaside" / "last-good";
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::write) {
            EXPECT_GE(io->ancestor_syncs, 2U);
        }
    };
    ASSERT_TRUE(store.publish(path, "new", seaside::PublishMode::replace_snapshot));
    EXPECT_EQ(read(path), "new");
    for (auto parent : {root / "state", root / "state" / "seaside"}) {
        struct stat metadata{};
        ASSERT_EQ(::stat(parent.c_str(), &metadata), 0);
        EXPECT_EQ(metadata.st_mode & 0777, 0700);
    }
}

TEST_F(StoreTest, FailedAncestorSyncDoesNotPublishConfigurationOrChangeExistingSnapshot) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    ASSERT_TRUE(store.publish(root / "old", "old", seaside::PublishMode::replace_snapshot));
    io->fail = seaside::StorageStep::sync_ancestor;
    auto path = root / "state" / "seaside" / "last-good";
    auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().operation, seaside::StorageStep::sync_ancestor);
    EXPECT_FALSE(result.error().recovery_uncertain);
    EXPECT_FALSE(result.error().publication_committed);
    EXPECT_FALSE(result.error().cleanup_failed);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(read(root / "old"), "old");
}

TEST_F(StoreTest, FailedFirstUseDirectorySyncNeverDeletesPublishedOrEditedUserFile) {
    for (bool replace : {false, true}) {
        SCOPED_TRACE(replace);
        auto io = std::make_shared<FaultIo>();
        seaside::ConfigStore store(io);
        const auto path = root / (replace ? "edited" : "unmodified");
        io->fail = seaside::StorageStep::sync_directory;
        io->observe = [&](seaside::StorageStep step) {
            if (replace && step == seaside::StorageStep::sync_directory) {
                write(root / "editor", "editor's config");
                std::filesystem::rename(root / "editor", path);
            }
        };
        auto result = store.publish(path, "generated", seaside::PublishMode::create_user_file);
        ASSERT_FALSE(result);
        EXPECT_TRUE(result.error().recovery_uncertain);
        EXPECT_FALSE(result.error().publication_committed);
        EXPECT_FALSE(result.error().cleanup_failed);
        EXPECT_EQ(read(path), replace ? "editor's config" : "generated");
    }
}

TEST_F(StoreTest, FailedSnapshotSyncPreservesEditorReplacementAndPreviousBackup) {
    for (auto phase : {seaside::StorageStep::sync_directory, seaside::StorageStep::rollback}) {
        SCOPED_TRACE(static_cast<int>(phase));
        const auto directory = root / std::to_string(static_cast<int>(phase));
        std::filesystem::create_directory(directory);
        auto io = std::make_shared<FaultIo>();
        seaside::ConfigStore store(io);
        const auto path = directory / "last-good";
        ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
        io->fail = seaside::StorageStep::sync_directory;
        io->observe = [&](seaside::StorageStep step) {
            if (step == phase) {
                write(directory / "editor", "editor's snapshot");
                std::filesystem::rename(directory / "editor", path);
            }
        };
        auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
        ASSERT_FALSE(result);
        EXPECT_TRUE(result.error().recovery_uncertain);
        EXPECT_FALSE(result.error().publication_committed);
        EXPECT_TRUE(result.error().cleanup_failed);
        EXPECT_EQ(read(path), "editor's snapshot");
        bool retained_old = false;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".previous" && read(entry.path()) == "old")
                retained_old = true;
        EXPECT_TRUE(retained_old);
    }
}

TEST_F(StoreTest, FailedRollbackRetainsTheOnlyOldSnapshotCopy) {
    auto io = std::make_shared<FaultIo>();
    seaside::ConfigStore store(io);
    auto path = root / "last-good";
    ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
    unsigned rollback_calls = 0;
    io->observe = [&](seaside::StorageStep step) {
        if (step == seaside::StorageStep::sync_directory)
            io->fail = step;
        if (step == seaside::StorageStep::rollback && ++rollback_calls == 2)
            io->fail = step;
    };
    auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
    ASSERT_FALSE(result);
    EXPECT_TRUE(result.error().recovery_uncertain);
    EXPECT_TRUE(result.error().cleanup_failed);
    bool retained_old = false;
    for (const auto &entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == ".previous" && read(entry.path()) == "old")
            retained_old = true;
    EXPECT_TRUE(retained_old);
}

TEST_F(StoreTest, SuccessfulDirectorySyncDoesNotClaimAnExternallyChangedSnapshotPersisted) {
    for (bool replace : {false, true}) {
        SCOPED_TRACE(replace);
        const auto directory = root / (replace ? "replacement" : "in-place");
        std::filesystem::create_directory(directory);
        auto io = std::make_shared<FaultIo>();
        seaside::ConfigStore store(io);
        const auto path = directory / "last-good";
        ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
        io->observe = [&](seaside::StorageStep step) {
            if (step == seaside::StorageStep::sync_directory) {
                if (replace) {
                    write(directory / "editor", "editor's snapshot");
                    std::filesystem::rename(directory / "editor", path);
                } else {
                    write(path, "editor's changed snapshot");
                }
            }
        };
        auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().operation, seaside::StorageStep::publish);
        EXPECT_TRUE(result.error().recovery_uncertain);
        EXPECT_FALSE(result.error().publication_committed);
        EXPECT_TRUE(result.error().cleanup_failed);
        EXPECT_EQ(read(path), replace ? "editor's snapshot" : "editor's changed snapshot");
        bool retained_old = false;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".previous" && read(entry.path()) == "old")
                retained_old = true;
        EXPECT_TRUE(retained_old);
    }
}

TEST_F(StoreTest, AThrowingGenerationPredicateCannotLeakTemporaryFilesOrChangeSnapshot) {
    seaside::ConfigStore store;
    auto path = root / "last-good";
    ASSERT_TRUE(store.publish(path, "old", seaside::PublishMode::replace_snapshot));
    unsigned checks = 0;
    auto result = store.publish(path, "new", seaside::PublishMode::replace_snapshot, [&] {
        if (++checks == 2)
            throw std::runtime_error("generation source disappeared");
        return true;
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().operation, seaside::StorageStep::publish);
    EXPECT_EQ(read(path), "old");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(root), {}), 1);
}
} // namespace

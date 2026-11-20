#include "server.hpp"
#include "test_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <pthread.h>
#include <string>
#include <wayland-server-core.h>

namespace {
using seaside::Server;
using seaside::ServerError;

struct ShutdownMaskProbe {
    wl_listener listener{};
    bool blocked_during_display_destroy = false;

    ShutdownMaskProbe() {
        listener.notify = [](wl_listener *listener, void *) noexcept {
            ShutdownMaskProbe *probe = wl_container_of(listener, probe, listener);
            sigset_t mask;
            probe->blocked_during_display_destroy =
                pthread_sigmask(SIG_SETMASK, nullptr, &mask) == 0 &&
                sigismember(&mask, SIGTERM) == 1 && sigismember(&mask, SIGINT) == 1;
        };
    }
};

class ServerLoopTest : public testing::Test {
  protected:
    seaside::test::Directory directory;
    std::optional<std::string> previous_runtime;
    sigset_t previous_mask{};

    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3-14-session-loop");
        if (const auto *value = std::getenv("XDG_RUNTIME_DIR")) {
            previous_runtime = value;
        }
        ASSERT_EQ(setenv("XDG_RUNTIME_DIR", directory.path().c_str(), 1), 0);
        ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &previous_mask), 0);
    }

    void TearDown() override {
        EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr), 0);
        if (previous_runtime) {
            EXPECT_EQ(setenv("XDG_RUNTIME_DIR", previous_runtime->c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv("XDG_RUNTIME_DIR"), 0);
        }
    }
};

TEST_F(ServerLoopTest, AutomaticSocketHasDisplayLifetimeAndCannotBePublishedTwice) {
    auto created = Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    auto result = server.listen();
    ASSERT_TRUE(result);
    const auto socket = directory.path() / *result;
    EXPECT_TRUE(std::filesystem::exists(socket));
    auto duplicate = server.listen();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error(), ServerError::invalid_state);
    server.finish();
    EXPECT_FALSE(std::filesystem::exists(socket));
    EXPECT_FALSE(std::filesystem::exists(socket.string() + ".lock"));
    auto after_finish = server.listen();
    ASSERT_FALSE(after_finish);
    EXPECT_EQ(after_finish.error(), ServerError::invalid_state);
}

TEST_F(ServerLoopTest, NamedSocketCopiesItsNameAndRejectsEmbeddedNull) {
    auto created = Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    const std::string invalid("name\0suffix", 11);
    auto rejected = server.listen(invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), ServerError::invalid_arguments);
    std::string name = "seaside-named-test";
    auto result = server.listen(name);
    ASSERT_TRUE(result);
    name.assign(100, 'z');
    EXPECT_EQ(*result, "seaside-named-test");
    EXPECT_TRUE(std::filesystem::exists(directory.path() / "seaside-named-test"));
}

TEST_F(ServerLoopTest, OccupiedSocketFailureDoesNotRemoveExistingOwnerAndAllowsRetry) {
    auto first = Server::create();
    auto second = Server::create();
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE((*first)->listen("seaside-conflict"));
    auto rejected = (*second)->listen("seaside-conflict");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error(), ServerError::socket_creation);
    EXPECT_TRUE(std::filesystem::exists(directory.path() / "seaside-conflict"));
    ASSERT_TRUE((*second)->listen("seaside-other"));
    second->reset();
    EXPECT_TRUE(std::filesystem::exists(directory.path() / "seaside-conflict"));
}

TEST_F(ServerLoopTest, RunRequiresLivePublishedDisplayAndTerminatesThroughLoopCallback) {
    auto created = Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    auto unready = server.run();
    ASSERT_FALSE(unready);
    EXPECT_EQ(unready.error(), ServerError::invalid_state);
    ASSERT_TRUE(server.listen());
    auto callback = [](void *data) { static_cast<Server *>(data)->terminate(); };
    ASSERT_NE(
        wl_event_loop_add_idle(wl_display_get_event_loop(server.display()), callback, &server),
        nullptr);
    ASSERT_TRUE(server.run());
    server.finish();
    auto stopped = server.run();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error(), ServerError::invalid_state);
}

TEST_F(ServerLoopTest, ShutdownSignalDispatchedDuringStartupPreventsRun) {
    auto created = Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    ASSERT_TRUE(server.listen());
    ASSERT_TRUE(server.install_shutdown_signals());
    ASSERT_EQ(raise(SIGTERM), 0);
    auto *loop = wl_display_get_event_loop(server.display());
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    ASSERT_TRUE(server.shutdown_requested());
    auto result = server.run();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ServerError::shutdown_requested);
}

TEST_F(ServerLoopTest, SignalRegistrationRestoresOriginalMaskAndRejectsDuplicates) {
    ShutdownMaskProbe probe;
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGTERM);
    sigaddset(&unblocked, SIGINT);
    ASSERT_EQ(pthread_sigmask(SIG_UNBLOCK, &unblocked, nullptr), 0);
    // Preserve a caller-blocked signal while restoring a previously unblocked
    // signal. This checks ownership rather than assuming an empty initial mask.
    sigset_t term;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &term, nullptr), 0);
    auto created = Server::create();
    ASSERT_TRUE(created);
    auto &server = **created;
    ASSERT_TRUE(server.install_shutdown_signals());
    auto duplicate = server.install_shutdown_signals();
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error(), ServerError::invalid_state);
    sigset_t during;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &during), 0);
    EXPECT_EQ(sigismember(&during, SIGTERM), 1);
    EXPECT_EQ(sigismember(&during, SIGINT), 1);
    wl_display_add_destroy_listener(server.display(), &probe.listener);
    server.finish();
    EXPECT_TRUE(probe.blocked_during_display_destroy);
    sigset_t after;
    ASSERT_EQ(pthread_sigmask(SIG_SETMASK, nullptr, &after), 0);
    EXPECT_EQ(sigismember(&after, SIGTERM), 1);
    EXPECT_EQ(sigismember(&after, SIGINT), 0);
    auto stopped = server.install_shutdown_signals();
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error(), ServerError::invalid_state);
}
} // namespace

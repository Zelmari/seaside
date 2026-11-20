#include "test_directory.hpp"
#include "tty_terminal.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using seaside::test::TerminalChild;

class SavedEnvironment final {
  public:
    explicit SavedEnvironment(const char *name) : name_(name) {
        if (const auto *value = getenv(name)) {
            value_ = value;
        }
    }
    ~SavedEnvironment() {
        if (value_) {
            setenv(name_, value_->c_str(), 1);
        } else {
            unsetenv(name_);
        }
    }
    SavedEnvironment(const SavedEnvironment &) = delete;
    SavedEnvironment &operator=(const SavedEnvironment &) = delete;

  private:
    const char *name_;
    std::optional<std::string> value_;
};

bool env_is(const char *name, std::string_view expected) {
    const auto *value = getenv(name);
    return value && value == expected;
}

int child_probe() {
    if (const auto *marker = getenv("SEASIDE_TEST_TERMINAL_MARKER")) {
        struct sigaction action{};
        action.sa_handler = SIG_IGN;
        if (sigaction(SIGTERM, &action, nullptr) < 0) {
            return 21;
        }
        int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return 22;
        }
        close(fd);
        // The marker observes that exec completed and TERM is now ignored.
        // The owner must use its bounded KILL path to end this child.
        for (;;) {
            pause();
        }
    }
    if (!env_is("WAYLAND_DISPLAY", "seaside-tty-unit") || getenv("WAYLAND_SOCKET") ||
        getenv("DISPLAY") || !env_is("XDG_SESSION_TYPE", "wayland") ||
        !env_is("XDG_CURRENT_DESKTOP", "Seaside")) {
        return 23;
    }
    struct stat stdin_stat{};
    struct stat null_stat{};
    if (getsid(0) != getpid() || fstat(STDIN_FILENO, &stdin_stat) < 0 ||
        stat("/dev/null", &null_stat) < 0 || !S_ISCHR(stdin_stat.st_mode) ||
        stdin_stat.st_rdev != null_stat.st_rdev) {
        return 24;
    }
    sigset_t mask;
    struct sigaction observed{};
    if (sigprocmask(SIG_SETMASK, nullptr, &mask) < 0 || sigismember(&mask, SIGTERM) != 0 ||
        sigismember(&mask, SIGINT) != 0 || sigaction(SIGTERM, nullptr, &observed) < 0 ||
        observed.sa_handler != SIG_DFL) {
        return 25;
    }
    return 42;
}

std::expected<int, int> wait_for_status(TerminalChild &child) {
    const auto limit = std::chrono::steady_clock::now() + 2s;
    do {
        auto result = child.poll();
        if (!result) {
            return std::unexpected(result.error());
        }
        if (result->has_value()) {
            return **result;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < limit);
    return std::unexpected(ETIMEDOUT);
}

struct TerminalChildTest : ::testing::Test {
    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3-14-terminal-child-lifetime");
    }
};

TEST_F(TerminalChildTest, ExecIsIsolatedAndOneShot) {
    const SavedEnvironment wayland_socket("WAYLAND_SOCKET");
    const SavedEnvironment wayland_display("WAYLAND_DISPLAY");
    const SavedEnvironment x_display("DISPLAY");
    const SavedEnvironment session_type("XDG_SESSION_TYPE");
    const SavedEnvironment desktop("XDG_CURRENT_DESKTOP");
    ASSERT_EQ(setenv("WAYLAND_SOCKET", "5", 1), 0);
    ASSERT_EQ(setenv("WAYLAND_DISPLAY", "outside-socket", 1), 0);
    ASSERT_EQ(setenv("DISPLAY", ":7", 1), 0);
    ASSERT_EQ(setenv("XDG_SESSION_TYPE", "tty", 1), 0);
    ASSERT_EQ(setenv("XDG_CURRENT_DESKTOP", "outside", 1), 0);

    sigset_t blocked;
    sigset_t original_mask;
    ASSERT_EQ(sigemptyset(&blocked), 0);
    ASSERT_EQ(sigaddset(&blocked, SIGINT), 0);
    ASSERT_EQ(sigaddset(&blocked, SIGTERM), 0);
    ASSERT_EQ(pthread_sigmask(SIG_BLOCK, &blocked, &original_mask), 0);
    struct sigaction ignored{};
    struct sigaction original_term{};
    ignored.sa_handler = SIG_IGN;
    const int saved_action = sigaction(SIGTERM, &ignored, &original_term);
    TerminalChild child("/proc/self/exe");
    auto started = child.launch("seaside-tty-unit");
    if (saved_action == 0) {
        EXPECT_EQ(sigaction(SIGTERM, &original_term, nullptr), 0);
    }
    EXPECT_EQ(pthread_sigmask(SIG_SETMASK, &original_mask, nullptr), 0);
    ASSERT_EQ(saved_action, 0);
    ASSERT_TRUE(started);
    EXPECT_EQ(child.pid(), *started);
    EXPECT_TRUE(env_is("WAYLAND_SOCKET", "5"));
    EXPECT_TRUE(env_is("WAYLAND_DISPLAY", "outside-socket"));

    auto status = wait_for_status(child);
    ASSERT_TRUE(status);
    ASSERT_TRUE(WIFEXITED(*status));
    EXPECT_EQ(WEXITSTATUS(*status), 42);
    EXPECT_EQ(child.pid(), 0);
    auto repeated = child.launch("seaside-tty-unit");
    ASSERT_FALSE(repeated);
    EXPECT_EQ(repeated.error(), EALREADY);
    auto cached = child.poll();
    ASSERT_TRUE(cached);
    EXPECT_EQ(*cached, *status);
    EXPECT_TRUE(child.stop());
}

TEST_F(TerminalChildTest, MissingExecutableIsReportedAndReaped) {
    const seaside::test::Directory directory;
    const auto unavailable = directory.path() / "missing-foot";
    TerminalChild child(unavailable.c_str());
    auto started = child.launch("seaside-tty-unit");
    ASSERT_TRUE(started);
    auto result = wait_for_status(child);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ENOENT);
    EXPECT_EQ(child.pid(), 0);
    EXPECT_EQ(waitpid(*started, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST_F(TerminalChildTest, ShutdownEscalatesAndReapsTheExactChild) {
    const seaside::test::Directory directory;
    const auto marker = directory.path() / "ready";
    const SavedEnvironment saved("SEASIDE_TEST_TERMINAL_MARKER");
    ASSERT_EQ(setenv("SEASIDE_TEST_TERMINAL_MARKER", marker.c_str(), 1), 0);
    TerminalChild child("/proc/self/exe");
    auto started = child.launch("seaside-tty-unit");
    ASSERT_TRUE(started);
    const auto limit = std::chrono::steady_clock::now() + 2s;
    while (!std::filesystem::exists(marker) && std::chrono::steady_clock::now() < limit) {
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(std::filesystem::exists(marker));
    ASSERT_TRUE(child.stop());
    EXPECT_EQ(child.pid(), 0);
    auto status = child.poll();
    ASSERT_TRUE(status && status->has_value());
    EXPECT_TRUE(WIFSIGNALED(**status));
    EXPECT_EQ(WTERMSIG(**status), SIGKILL);
    EXPECT_EQ(waitpid(*started, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--config=/dev/null") {
        return child_probe();
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

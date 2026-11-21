#include "satellite.hpp"
#include "test_directory.hpp"

#include <X11/Xauth.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using seaside::Satellite;

class Fd {
  public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return fd_; }

  private:
    int fd_;
};

// Stand-in for the external program. It accepts actual queued connections
// from both inherited listeners and reports which single child served them.
int child_probe(char **args) {
    if (std::string_view(args[2]) != "-listenfd" || std::string_view(args[3]) != "3" ||
        std::string_view(args[4]) != "-listenfd" || std::string_view(args[5]) != "4" ||
        std::string_view(args[6]) != "-auth" || std::string_view(args[8]) != "-nolisten" ||
        std::string_view(args[9]) != "tcp" || access(args[7], R_OK) != 0) {
        return 20;
    }
    const char *wayland = getenv("WAYLAND_DISPLAY");
    if (!wayland || std::string_view(wayland) != "seaside-satellite-test" ||
        getenv("WAYLAND_SOCKET") || getenv("DISPLAY") || getenv("XAUTHORITY") ||
        fcntl(5, F_GETFD) >= 0) {
        return 21;
    }
    const pid_t pid = getpid();
    std::array<pollfd, 2> sockets{{{3, POLLIN, 0}, {4, POLLIN, 0}}};
    while (poll(sockets.data(), sockets.size(), 5000) > 0) {
        for (const auto &ready : sockets) {
            if (!(ready.revents & POLLIN)) {
                continue;
            }
            const Fd client(accept4(ready.fd, nullptr, nullptr, SOCK_CLOEXEC));
            if (client.get() < 0 || write(client.get(), &pid, sizeof(pid)) != sizeof(pid)) {
                return 22;
            }
        }
    }
    return 0;
}

struct SatelliteTest : ::testing::Test {
    seaside::test::Directory directory;
    std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)> loop{wl_event_loop_create(),
                                                                          wl_event_loop_destroy};
    std::unique_ptr<Satellite> satellite;

    void SetUp() override {
        ASSERT_TRUE(loop);
        RecordProperty("part", "7");
        RecordProperty("requirement", "spec-3-G12-satellite-lifetime");
    }

    auto reserve(std::string_view executable = "/proc/self/exe") {
        return Satellite::reserve(loop.get(), "seaside-satellite-test", directory.path().string(),
                                  executable, directory.path().string());
    }

    std::string socket_path() const {
        return (directory.path() / ".X11-unix" /
                ("X" + std::string(satellite->display_name().substr(1))))
            .string();
    }

    int connect_to_socket(bool abstract_socket) const {
        const auto path = socket_path();
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            return -1;
        }
        path.copy(address.sun_path + abstract_socket, path.size());
        const auto size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd >= 0 && connect(fd, reinterpret_cast<const sockaddr *>(&address), size) == 0) {
            return fd;
        }
        if (fd >= 0) {
            close(fd);
        }
        return -1;
    }

    template <class Predicate> bool dispatch_until(Predicate predicate) const {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        do {
            if (predicate()) {
                return true;
            }
            if (wl_event_loop_dispatch(loop.get(), 10) < 0 && errno != EINTR) {
                return false;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    pid_t child_on(const Fd &client) const {
        pid_t pid = 0;
        const bool delivered = dispatch_until(
            [&] { return recv(client.get(), &pid, sizeof(pid), MSG_PEEK) == sizeof(pid); });
        return delivered ? pid : 0;
    }
};

TEST_F(SatelliteTest, ReservesIndependentDisplaysAndPrivateAuthority) {
    const auto occupied_path = directory.path() / ".X0-lock";
    std::ofstream(occupied_path) << "existing display";
    auto first = reserve();
    auto second = reserve();
    ASSERT_TRUE(first) << first.error_or(0);
    ASSERT_TRUE(second) << second.error_or(0);
    EXPECT_EQ((*first)->display_name(), ":1");
    EXPECT_EQ((*second)->display_name(), ":2");

    const std::string authority((*first)->authority_path());
    struct stat info{};
    ASSERT_EQ(stat(authority.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600U);
    const auto close_file = [](FILE *file) { fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> reader(fopen(authority.c_str(), "rb"), close_file);
    ASSERT_TRUE(reader);
    std::unique_ptr<Xauth, decltype(&XauDisposeAuth)> entry(XauReadAuth(reader.get()),
                                                            XauDisposeAuth);
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->family, FamilyLocal);
    EXPECT_EQ(std::string_view(entry->number, entry->number_length), "1");
    EXPECT_EQ(std::string_view(entry->name, entry->name_length), "MIT-MAGIC-COOKIE-1");
    EXPECT_EQ(entry->data_length, 16);
    first->reset();
    EXPECT_FALSE(std::filesystem::exists(authority));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / ".X1-lock"));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / ".X11-unix/X1"));
    EXPECT_TRUE(std::filesystem::exists(occupied_path));
    EXPECT_TRUE(std::filesystem::exists(directory.path() / ".X11-unix/X2"));
}

TEST_F(SatelliteTest, FirstConnectionsActivateOnceAndCanRestartAfterCrash) {
    auto created = reserve();
    ASSERT_TRUE(created) << created.error_or(0);
    satellite = std::move(*created);
    const Fd first(connect_to_socket(false));
    const Fd second(connect_to_socket(true));
    ASSERT_GE(first.get(), 0);
    ASSERT_GE(second.get(), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop.get(), 25), 0);
    char byte;
    EXPECT_EQ(recv(first.get(), &byte, 1, MSG_PEEK), -1);
    EXPECT_EQ(errno, EAGAIN);

    ASSERT_TRUE(satellite->enable());
    const auto pid = child_on(first);
    ASSERT_GT(pid, 0);
    EXPECT_EQ(child_on(second), pid);
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    ASSERT_TRUE(dispatch_until([&] { return kill(pid, 0) < 0 && errno == ESRCH; }));

    const Fd next(connect_to_socket(true));
    ASSERT_GE(next.get(), 0);
    const auto restarted = child_on(next);
    ASSERT_GT(restarted, 0);
    EXPECT_NE(restarted, pid);
    satellite.reset();
    EXPECT_EQ(waitpid(restarted, nullptr, WNOHANG), -1);
    EXPECT_EQ(errno, ECHILD);
}

TEST_F(SatelliteTest, FailedSpawnDropsTheFirstRequestAndAllowsTheNext) {
    const auto executable = directory.path() / "satellite";
    auto created = reserve(executable.string());
    ASSERT_TRUE(created) << created.error_or(0);
    satellite = std::move(*created);
    ASSERT_TRUE(satellite->enable());
    const Fd failed(connect_to_socket(false));
    ASSERT_GE(failed.get(), 0);
    ASSERT_TRUE(dispatch_until([&] {
        char byte;
        return recv(failed.get(), &byte, 1, MSG_PEEK) == 0;
    }));
    ASSERT_EQ(symlink("/proc/self/exe", executable.c_str()), 0);
    const Fd next(connect_to_socket(false));
    ASSERT_GE(next.get(), 0);
    EXPECT_GT(child_on(next), 0);
}

TEST_F(SatelliteTest, ExistingSocketWithoutLockIsPreserved) {
    const auto socket_directory = directory.path() / ".X11-unix";
    ASSERT_EQ(mkdir(socket_directory.c_str(), 01777), 0);
    const auto existing = socket_directory / "X0";
    std::ofstream(existing) << "unowned path";
    auto created = reserve();
    ASSERT_TRUE(created) << created.error_or(0);
    EXPECT_EQ((*created)->display_name(), ":1");
    EXPECT_TRUE(std::filesystem::exists(existing));
    EXPECT_FALSE(std::filesystem::exists(directory.path() / ".X0-lock"));
}

} // namespace

int main(int argc, char **argv) {
    if (argc == 10 && argv[1][0] == ':') {
        return child_probe(argv);
    }
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

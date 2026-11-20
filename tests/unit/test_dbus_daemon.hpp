#ifndef SEASIDE_TEST_DBUS_DAEMON_HPP
#define SEASIDE_TEST_DBUS_DAEMON_HPP

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace seaside::test {
using namespace std::chrono_literals;

class PrivateDaemon {
  public:
    explicit PrivateDaemon(const std::string &address) {
        std::array<int, 2> pipe{};
        if (pipe2(pipe.data(), O_CLOEXEC) < 0) {
            throw std::runtime_error("dbus startup pipe");
        }
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error != 0) {
            close(pipe[0]);
            close(pipe[1]);
            throw std::runtime_error("dbus startup file actions");
        }
        if (error == 0) {
            error = posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
        }
        if (error == 0) {
            error = posix_spawn_file_actions_addclose(&actions, pipe[0]);
        }
        if (error == 0) {
            error = posix_spawn_file_actions_addclose(&actions, pipe[1]);
        }
        std::string option = "--address=" + address;
        std::array<char *, 7> args{const_cast<char *>("dbus-daemon"),
                                   const_cast<char *>("--session"),
                                   const_cast<char *>("--nofork"),
                                   const_cast<char *>("--nopidfile"),
                                   option.data(),
                                   const_cast<char *>("--print-address=1"),
                                   nullptr};
        if (error == 0) {
            error = posix_spawnp(&pid_, args[0], &actions, nullptr, args.data(), environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        close(pipe[1]);
        if (error != 0) {
            close(pipe[0]);
            throw std::runtime_error("dbus-daemon spawn failed");
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        std::string output;
        while (std::chrono::steady_clock::now() < deadline &&
               output.find('\n') == std::string::npos) {
            pollfd fd{pipe[0], POLLIN, 0};
            if (poll(&fd, 1, 20) > 0) {
                std::array<char, 4096> bytes{};
                const auto count = read(pipe[0], bytes.data(), bytes.size());
                if (count <= 0) {
                    break;
                }
                output.append(bytes.data(), static_cast<std::size_t>(count));
                if (output.size() > 4096) {
                    break;
                }
            }
        }
        close(pipe[0]);
        if (output.find(address) != 0 || output.find('\n') == std::string::npos) {
            stop();
            throw std::runtime_error("dbus-daemon did not become ready");
        }
    }
    ~PrivateDaemon() { stop(); }
    void stop() {
        if (pid_ <= 0) {
            return;
        }
        // This non-forking child has not been reaped, so its PID cannot yet be
        // reused for an unrelated process.
        kill(pid_, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

  private:
    pid_t pid_ = -1;
};
} // namespace seaside::test
#endif

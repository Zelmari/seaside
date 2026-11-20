#include "tty_terminal.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace seaside::test {
namespace {
using namespace std::chrono_literals;

bool replaces_environment(std::string_view entry) noexcept {
    return entry.starts_with("WAYLAND_SOCKET=") || entry.starts_with("WAYLAND_DISPLAY=") ||
           entry.starts_with("DISPLAY=") || entry.starts_with("XDG_SESSION_TYPE=") ||
           entry.starts_with("XDG_CURRENT_DESKTOP=");
}

[[noreturn]] void child_failure(int error_fd, int error) noexcept {
    while (write(error_fd, &error, sizeof(error)) < 0 && errno == EINTR) {
    }
    _exit(127);
}

// The parent may already have renderer/library threads. The forked child uses
// only async-signal-safe operations until exec and reports errno by pipe.
[[noreturn]] void exec_terminal(const char *path, char **arguments, char **environment, int read_fd,
                                int error_fd) noexcept {
    close(read_fd);
    if (setsid() < 0) {
        child_failure(error_fd, errno);
    }
    const int null_fd = open("/dev/null", O_RDONLY | O_NOCTTY);
    if (null_fd < 0) {
        child_failure(error_fd, errno);
    }
    if (null_fd != STDIN_FILENO) {
        if (dup2(null_fd, STDIN_FILENO) < 0) {
            child_failure(error_fd, errno);
        }
        close(null_fd);
    }
    struct sigaction default_action{};
    default_action.sa_handler = SIG_DFL;
    const int signals[] = {SIGINT, SIGTERM, SIGHUP};
    if (sigemptyset(&default_action.sa_mask) < 0) {
        child_failure(error_fd, errno);
    }
    for (const int signal : signals) {
        if (sigaction(signal, &default_action, nullptr) < 0) {
            child_failure(error_fd, errno);
        }
    }
    if (sigprocmask(SIG_SETMASK, &default_action.sa_mask, nullptr) < 0) {
        child_failure(error_fd, errno);
    }
    execve(path, arguments, environment);
    child_failure(error_fd, errno);
}
} // namespace

TerminalChild::TerminalChild(const char *executable) noexcept : executable_(executable) {}

TerminalChild::~TerminalChild() noexcept {
    stop();
    if (exec_pipe_ >= 0) {
        close(exec_pipe_);
    }
}

std::expected<pid_t, int> TerminalChild::launch(std::string_view wayland_display) noexcept {
    if (launched_) {
        return std::unexpected(EALREADY);
    }
    if (!executable_ || executable_[0] != '/' || wayland_display.empty() ||
        wayland_display.find('\0') != std::string_view::npos) {
        return std::unexpected(EINVAL);
    }
    struct sigaction child_action{};
    if (sigaction(SIGCHLD, nullptr, &child_action) < 0) {
        return std::unexpected(errno);
    }
    // Keeping an exited PID reserved until we reap it is what makes signaling
    // the exact owned child safe. Reject dispositions that auto-reap it.
    if (child_action.sa_handler != SIG_DFL || (child_action.sa_flags & SA_NOCLDWAIT) != 0) {
        return std::unexpected(EINVAL);
    }
    try {
        std::vector<std::string> environment;
        for (auto **entry = environ; entry && *entry; ++entry) {
            if (!replaces_environment(*entry)) {
                environment.emplace_back(*entry);
            }
        }
        environment.emplace_back("WAYLAND_DISPLAY=");
        environment.back().append(wayland_display);
        environment.emplace_back("XDG_SESSION_TYPE=wayland");
        environment.emplace_back("XDG_CURRENT_DESKTOP=Seaside");
        std::vector<char *> values;
        values.reserve(environment.size() + 1);
        for (auto &entry : environment) {
            values.push_back(entry.data());
        }
        values.push_back(nullptr);
        auto **child_environment = values.data();

        char name[] = "foot";
        // The qualification trial launches one standalone window with a known
        // configuration independently of the user's terminal preferences.
        char config[] = "--config=/dev/null";
        char *arguments[] = {name, config, nullptr};
        int descriptors[2];
        if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) < 0) {
            return std::unexpected(errno);
        }
        const pid_t child = fork();
        if (child == 0) {
            exec_terminal(executable_, arguments, child_environment, descriptors[0],
                          descriptors[1]);
        }
        const int error = errno;
        close(descriptors[1]);
        if (child < 0) {
            close(descriptors[0]);
            return std::unexpected(error);
        }
        launched_ = true;
        pid_ = child;
        exec_pipe_ = descriptors[0];
        return child;
    } catch (...) {
        return std::unexpected(ENOMEM);
    }
}

pid_t TerminalChild::pid() const noexcept {
    return pid_;
}

void TerminalChild::collect_exec_result() noexcept {
    int child_error = 0;
    ssize_t result;
    do {
        result = read(exec_pipe_, &child_error, sizeof(child_error));
    } while (result < 0 && errno == EINTR);
    if (result == static_cast<ssize_t>(sizeof(child_error))) {
        failure_ = child_error;
    } else if (result != 0) {
        failure_ = result < 0 ? errno : EIO;
    }
    close(exec_pipe_);
    exec_pipe_ = -1;
}

void TerminalChild::reap() noexcept {
    if (pid_ <= 0) {
        return;
    }
    int status = 0;
    pid_t child;
    do {
        child = waitpid(pid_, &status, WNOHANG);
    } while (child < 0 && errno == EINTR);
    if (child == pid_) {
        pid_ = 0;
        status_ = status;
        collect_exec_result();
    } else if (child < 0) {
        failure_ = errno;
        if (failure_ == ECHILD) {
            // Fail closed if external code violated ownership. Do not signal
            // a PID that a different waiter might already have released.
            pid_ = 0;
        }
    }
}

std::expected<std::optional<int>, int> TerminalChild::poll() noexcept {
    reap();
    if (failure_ != 0) {
        return std::unexpected(failure_);
    }
    return status_;
}

bool TerminalChild::stop() noexcept {
    reap();
    if (pid_ <= 0) {
        return failure_ != ECHILD;
    }
    for (const int signal : {SIGTERM, SIGKILL}) {
        // No external waiter can reap this PID. Signal only this process, not
        // a process group or the shell that owns the TTY.
        kill(pid_, signal);
        const auto limit = std::chrono::steady_clock::now() + (signal == SIGTERM ? 250ms : 750ms);
        do {
            reap();
            if (pid_ <= 0) {
                return failure_ != ECHILD;
            }
            const timespec interval{.tv_sec = 0, .tv_nsec = 5'000'000};
            nanosleep(&interval, nullptr);
        } while (std::chrono::steady_clock::now() < limit);
    }
    return false;
}
} // namespace seaside::test

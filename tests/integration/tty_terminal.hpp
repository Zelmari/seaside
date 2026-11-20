#ifndef SEASIDE_TEST_TTY_TERMINAL_HPP
#define SEASIDE_TEST_TTY_TERMINAL_HPP

#include <expected>
#include <optional>
#include <string_view>
#include <sys/types.h>

namespace seaside::test {

// Owns the one terminal child used by the TTY qualification fixture. Only the
// owning thread may use it. The fixture must be the child's only waiter.
class TerminalChild final {
  public:
    // The override is for isolated subprocess tests and must outlive the owner.
    explicit TerminalChild(const char *executable = "/usr/bin/foot") noexcept;
    ~TerminalChild() noexcept;
    TerminalChild(const TerminalChild &) = delete;
    TerminalChild &operator=(const TerminalChild &) = delete;

    // Returns the owned PID as soon as fork succeeds. Exec errors are reported
    // by poll(). A second launch after a successful fork is rejected.
    std::expected<pid_t, int> launch(std::string_view wayland_display) noexcept;
    pid_t pid() const noexcept;
    // No value means running or not started. An exited child returns its raw
    // wait status, including on later polls, unless setup or exec failed.
    std::expected<std::optional<int>, int> poll() noexcept;
    // Run only after dispatch. Gives the child time to exit after SIGTERM and
    // uses SIGKILL if it does not. Returns false if it could not be reaped.
    bool stop() noexcept;

  private:
    void collect_exec_result() noexcept;
    void reap() noexcept;

    const char *executable_;
    pid_t pid_ = 0;
    int exec_pipe_ = -1;
    int failure_ = 0;
    std::optional<int> status_;
    bool launched_ = false;
};

} // namespace seaside::test

#endif

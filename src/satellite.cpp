#include "satellite.hpp"

#include <X11/Xauth.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/pidfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

extern char **environ;

namespace seaside {
namespace {

constexpr int retry_delay_ms = 250;
constexpr int backlog = 32;
constexpr int first_child_fd = STDERR_FILENO + 1;
constexpr int child_listener_count = 2;

class Fd {
  public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() { reset(); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_;
};

struct OwnedPath {
    std::string path;
    struct stat identity{};

    ~OwnedPath() { remove(); }

    void remove() noexcept {
        struct stat current{};
        if (identity.st_ino != 0 && lstat(path.c_str(), &current) == 0 &&
            current.st_dev == identity.st_dev && current.st_ino == identity.st_ino) {
            unlink(path.c_str());
        }
        identity = {};
    }
};

int prepare_socket_directory(const std::string &path) noexcept {
    const bool created = mkdir(path.c_str(), 01777) == 0;
    if (!created && errno != EEXIST) {
        return errno;
    }
    const Fd directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat info{};
    if (!directory || fstat(directory.get(), &info) < 0) {
        return errno;
    }
    if (created && fchmod(directory.get(), 01777) < 0) {
        return errno;
    }
    if ((info.st_uid != 0 && info.st_uid != getuid()) || (!created && !(info.st_mode & S_ISVTX))) {
        return EACCES;
    }
    return 0;
}

int listen_on(Fd &listener, const std::string &path, OwnedPath *owned_path) noexcept {
    const bool abstract_socket = owned_path == nullptr;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        return ENAMETOOLONG;
    }
    // Linux abstract sockets count the leading NUL, filesystem sockets the trailing NUL.
    path.copy(address.sun_path + abstract_socket, path.size());
    const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    const int opened = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (opened < 0) {
        return errno;
    }
    listener.reset(opened);
    if (bind(listener.get(), reinterpret_cast<const sockaddr *>(&address), length) < 0) {
        return errno;
    }
    if (owned_path && lstat(path.c_str(), &owned_path->identity) < 0) {
        return errno;
    }
    return listen(listener.get(), backlog) == 0 ? 0 : errno;
}

int write_authority(OwnedPath &authority, std::string_view runtime_dir,
                    const std::string &display) {
    std::array<char, 16> cookie;
    if (getrandom(cookie.data(), cookie.size(), GRND_NONBLOCK) !=
        static_cast<ssize_t>(cookie.size())) {
        return EIO;
    }
    char hostname[HOST_NAME_MAX + 1]{};
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        return errno;
    }
    authority.path = std::string(runtime_dir) + "/seaside-xauth-XXXXXX";
    Fd fd(mkostemp(authority.path.data(), O_CLOEXEC));
    if (!fd || fstat(fd.get(), &authority.identity) < 0) {
        return errno;
    }
    FILE *file = fdopen(fd.get(), "wb");
    if (!file) {
        return errno;
    }
    fd.release();
    char mechanism[] = "MIT-MAGIC-COOKIE-1";
    Xauth auth{.family = FamilyLocal,
               .address_length = static_cast<unsigned short>(strlen(hostname)),
               .address = hostname,
               .number_length = static_cast<unsigned short>(display.size() - 1),
               .number = const_cast<char *>(display.data() + 1),
               .name_length = sizeof(mechanism) - 1,
               .name = mechanism,
               .data_length = cookie.size(),
               .data = cookie.data()};
    const bool wrote = XauWriteAuth(file, &auth) != 0;
    return fclose(file) == 0 && wrote ? 0 : EIO;
}

void drop_pending(const Fd &listener) noexcept {
    const int flags = fcntl(listener.get(), F_GETFL);
    if (flags < 0 || fcntl(listener.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
        return;
    }
    for (int i = 0; i <= backlog; ++i) {
        const Fd client(accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (!client) {
            break;
        }
    }
    fcntl(listener.get(), F_SETFL, flags);
}

using EventSource = std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)>;
} // namespace

struct Satellite::Impl {
    wl_event_loop *loop;
    std::string executable;
    std::string display;
    std::vector<std::string> environment;
    std::vector<char *> envp;
    OwnedPath lock;
    OwnedPath file_socket;
    OwnedPath authority;
    Fd lock_file;
    Fd local;
    Fd abstract;
    Fd child_fd;
    EventSource local_source{nullptr, wl_event_source_remove};
    EventSource abstract_source{nullptr, wl_event_source_remove};
    EventSource child_source{nullptr, wl_event_source_remove};
    EventSource cooldown{nullptr, wl_event_source_remove};
    pid_t child = 0;
    bool watching = false;

    Impl(wl_event_loop *owner, std::string_view command) : loop(owner), executable(command) {}

    ~Impl() {
        local_source.reset();
        abstract_source.reset();
        child_source.reset();
        cooldown.reset();
        if (child > 0) {
            // The child is the leader of a new session. Its unreaped PID
            // reserves the group ID while its Xwayland subprocess is stopped.
            kill(-child, SIGTERM);
            pollfd exited{.fd = child_fd.get(), .events = POLLIN, .revents = 0};
            if (child_fd) {
                poll(&exited, 1, retry_delay_ms);
            }
            reap();
        }
    }

    int reserve_sockets(std::string_view root) {
        const auto directory = std::string(root) + "/.X11-unix";
        if (const int error = prepare_socket_directory(directory)) {
            return error;
        }
        for (unsigned int number = 0; number < 128; ++number) {
            const auto suffix = std::to_string(number);
            lock.path = std::string(root) + "/.X" + suffix + "-lock";
            const int opened =
                open(lock.path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
            if (opened < 0) {
                if (errno == EEXIST) {
                    continue;
                }
                return errno;
            }
            lock_file.reset(opened);
            if (fstat(lock_file.get(), &lock.identity) < 0) {
                return errno;
            }
            char record[32];
            const auto length = std::snprintf(record, sizeof(record), "%10d\n", getpid());
            if (write(lock_file.get(), record, static_cast<std::size_t>(length)) != length) {
                return EIO;
            }
            file_socket.path = directory + "/X" + suffix;
            int error = listen_on(abstract, file_socket.path, nullptr);
            if (!error) {
                error = listen_on(local, file_socket.path, &file_socket);
            }
            if (!error) {
                display = ":" + suffix;
                return 0;
            }
            if (error != EADDRINUSE) {
                return error;
            }
            // A display may have a live socket without a lock. Keep the
            // other display's resources and try a different number.
            file_socket.remove();
            abstract.reset();
            local.reset();
            lock.remove();
            lock_file.reset();
        }
        return EADDRINUSE;
    }

    void prepare_environment(std::string_view wayland) {
        for (auto **entry = environ; *entry; ++entry) {
            const std::string_view value(*entry);
            if (!value.starts_with("WAYLAND_SOCKET=") && !value.starts_with("WAYLAND_DISPLAY=") &&
                !value.starts_with("DISPLAY=") && !value.starts_with("XAUTHORITY=") &&
                !value.starts_with("NOTIFY_SOCKET=")) {
                environment.emplace_back(value);
            }
        }
        environment.push_back("WAYLAND_DISPLAY=" + std::string(wayland));
        for (auto &value : environment) {
            envp.push_back(value.data());
        }
        envp.push_back(nullptr);
    }

    bool set_watch(bool enabled) noexcept {
        const unsigned int mask = enabled ? WL_EVENT_READABLE : 0;
        const int local_result = wl_event_source_fd_update(local_source.get(), mask);
        const int abstract_result = wl_event_source_fd_update(abstract_source.get(), mask);
        watching = enabled && local_result == 0 && abstract_result == 0;
        return local_result == 0 && abstract_result == 0;
    }

    int spawn() noexcept {
        // Staging descriptors above both child destinations avoids a dup2
        // overwriting one of the inputs. Only the two listeners survive exec.
        const int first_unused = first_child_fd + child_listener_count;
        const Fd path_fd(fcntl(local.get(), F_DUPFD_CLOEXEC, first_unused));
        if (!path_fd) {
            return errno;
        }
        const Fd abstract_fd(fcntl(abstract.get(), F_DUPFD_CLOEXEC, first_unused));
        if (!abstract_fd) {
            return errno;
        }
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attr;
        const int action_status = posix_spawn_file_actions_init(&actions);
        const int attr_status = posix_spawnattr_init(&attr);
        int error = action_status ? action_status : attr_status;
        if (!error) {
            sigset_t reset, unblocked;
            sigemptyset(&unblocked);
            sigemptyset(&reset);
            for (const int signal : {SIGINT, SIGTERM, SIGHUP, SIGPIPE}) {
                sigaddset(&reset, signal);
            }
            error = posix_spawnattr_setsigdefault(&attr, &reset);
            if (!error) {
                error = posix_spawnattr_setsigmask(&attr, &unblocked);
            }
            if (!error) {
                error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGDEF |
                                                            POSIX_SPAWN_SETSIGMASK);
            }
            if (!error) {
                error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                                         O_RDONLY | O_NOCTTY, 0);
            }
            if (!error) {
                error = posix_spawn_file_actions_adddup2(&actions, path_fd.get(), first_child_fd);
            }
            if (!error) {
                error = posix_spawn_file_actions_adddup2(&actions, abstract_fd.get(),
                                                         first_child_fd + 1);
            }
            if (!error) {
                error = posix_spawn_file_actions_addclosefrom_np(&actions, first_unused);
            }
            if (!error) {
                char *const args[]{executable.data(),
                                   display.data(),
                                   const_cast<char *>("-listenfd"),
                                   const_cast<char *>("3"),
                                   const_cast<char *>("-listenfd"),
                                   const_cast<char *>("4"),
                                   const_cast<char *>("-auth"),
                                   authority.path.data(),
                                   const_cast<char *>("-nolisten"),
                                   const_cast<char *>("tcp"),
                                   nullptr};
                pid_t pid = 0;
                error = posix_spawnp(&pid, executable.c_str(), &actions, &attr, args, envp.data());
                if (!error) {
                    child = pid;
                }
            }
        }
        if (!action_status) {
            posix_spawn_file_actions_destroy(&actions);
        }
        if (!attr_status) {
            posix_spawnattr_destroy(&attr);
        }
        return error;
    }

    void reap() noexcept {
        kill(-child, SIGKILL);
        const pid_t pid = std::exchange(child, 0);
        child_source.reset();
        child_fd.reset();
        int status = 0;
        pid_t result;
        do {
            result = waitpid(pid, &status, 0);
        } while (result < 0 && errno == EINTR);
        if (result == pid) {
            std::fprintf(stderr, "seaside: xwayland-satellite pid=%d wait-status=%d\n", pid,
                         status);
        }
    }

    void schedule_retry() noexcept {
        // A failed launch may leave the triggering connection queued. Drop
        // it before rearming so it cannot start the same failed child again.
        drop_pending(local);
        drop_pending(abstract);
        if (wl_event_source_timer_update(cooldown.get(), retry_delay_ms) < 0) {
            std::fputs("seaside: could not schedule X11 listener retry\n", stderr);
        }
    }

    static int on_connection(int, unsigned int, void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        if (!self.watching) {
            return 0;
        }
        self.set_watch(false);
        if (const int error = self.spawn()) {
            std::fprintf(stderr, "seaside: xwayland-satellite launch: %s\n", std::strerror(error));
            self.schedule_retry();
            return 0;
        }
        self.child_fd.reset(pidfd_open(self.child, 0));
        if (self.child_fd) {
            self.child_source.reset(wl_event_loop_add_fd(self.loop, self.child_fd.get(),
                                                         WL_EVENT_READABLE, on_exit, &self));
        }
        if (!self.child_source) {
            std::fputs("seaside: could not monitor xwayland-satellite child\n", stderr);
            self.reap();
            self.schedule_retry();
            return 0;
        }
        std::fprintf(stderr, "seaside: xwayland-satellite started pid=%d DISPLAY=%s\n", self.child,
                     self.display.c_str());
        return 0;
    }

    static int on_exit(int, unsigned int, void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        self.reap();
        self.schedule_retry();
        return 0;
    }

    static int on_cooldown(void *data) noexcept {
        auto &self = *static_cast<Impl *>(data);
        if (!self.set_watch(true)) {
            std::fputs("seaside: could not rearm X11 listeners\n", stderr);
        }
        return 0;
    }
};

std::expected<std::unique_ptr<Satellite>, int>
Satellite::reserve(wl_event_loop *loop, std::string_view wayland_display,
                   std::string_view runtime_dir, std::string_view executable,
                   std::string_view socket_root) noexcept try {
    if (!loop || wayland_display.empty() || runtime_dir.empty() || executable.empty()) {
        return std::unexpected(EINVAL);
    }
    auto impl = std::make_unique<Impl>(loop, executable);
    if (const int error = impl->reserve_sockets(socket_root)) {
        return std::unexpected(error);
    }
    if (const int error = write_authority(impl->authority, runtime_dir, impl->display)) {
        return std::unexpected(error);
    }
    impl->prepare_environment(wayland_display);
    impl->local_source.reset(
        wl_event_loop_add_fd(loop, impl->local.get(), 0, Impl::on_connection, impl.get()));
    if (!impl->local_source) {
        return std::unexpected(errno);
    }
    impl->abstract_source.reset(
        wl_event_loop_add_fd(loop, impl->abstract.get(), 0, Impl::on_connection, impl.get()));
    if (!impl->abstract_source) {
        return std::unexpected(errno);
    }
    impl->cooldown.reset(wl_event_loop_add_timer(loop, Impl::on_cooldown, impl.get()));
    if (!impl->cooldown) {
        return std::unexpected(errno);
    }
    return std::unique_ptr<Satellite>(new Satellite(std::move(impl)));
} catch (...) {
    return std::unexpected(ENOMEM);
}

Satellite::Satellite(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Satellite::~Satellite() noexcept = default;

std::string_view Satellite::display_name() const noexcept {
    return impl_->display;
}

std::string_view Satellite::authority_path() const noexcept {
    return impl_->authority.path;
}

bool Satellite::enable() noexcept {
    if (!impl_->set_watch(true)) {
        return false;
    }
    std::fprintf(stderr, "seaside: listening on X11 DISPLAY=%s\n", impl_->display.c_str());
    return true;
}

} // namespace seaside

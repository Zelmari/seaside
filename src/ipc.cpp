#include "ipc.hpp"

#include "ipc_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <set>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {

constexpr std::size_t connections_limit = 64;
constexpr std::size_t pending_limit = 32;
constexpr std::size_t queue_records_limit = 256;
constexpr std::size_t queue_bytes_limit = 1024 * 1024;
constexpr std::size_t read_chunk = 16 * 1024;
constexpr std::size_t write_budget = 64 * 1024;
constexpr int exit_deadline_ms = 250;

class Fd {
  public:
    explicit Fd(int fd = -1) noexcept : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        std::swap(fd_, other.fd_);
        return *this;
    }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

  private:
    int fd_;
};

struct SourceDeleter {
    void operator()(wl_event_source *source) const noexcept { wl_event_source_remove(source); }
};

using Source = std::unique_ptr<wl_event_source, SourceDeleter>;

std::string system_error(std::string_view operation) {
    return std::string(operation) + ": " + std::strerror(errno);
}

bool private_directory(int fd) {
    struct stat status{};
    return fstat(fd, &status) == 0 && status.st_uid == getuid() && S_ISDIR(status.st_mode) &&
           (status.st_mode & 0777) == 0700;
}

bool same_entry(int directory, const std::string &name, const struct stat &known) {
    struct stat current{};
    return fstatat(directory, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0 &&
           current.st_dev == known.st_dev && current.st_ino == known.st_ino;
}

std::optional<std::uint64_t> oversized_id(std::string_view wire) {
    return ipc::parse_record(wire.substr(0, ipc::request_limit - 1)).id;
}

IpcJson reply(std::optional<std::uint64_t> id, std::uint64_t seq, IpcResult result) {
    IpcJson response{{"v", 1}, {"id", id ? IpcJson(*id) : IpcJson(nullptr)}, {"seq", seq}};
    response["ok"] = result.has_value();
    if (result) {
        response["result"] = std::move(*result);
    } else {
        response["error"] = {{"code", result.error().code}, {"message", result.error().message}};
    }
    return response;
}

} // namespace

struct IpcServer::Impl {
    struct Pending {
        std::shared_ptr<const std::string> wire;
        std::size_t sent = 0;
        std::optional<std::uint64_t> release_id;
        bool response = false;
        bool exit = false;
    };

    struct Peer {
        Peer(Impl &server, Fd fd) noexcept : server(server), fd(std::move(fd)) {}
        static int dispatch(int fd, std::uint32_t mask, void *data) noexcept;
        void ready(std::uint32_t mask);
        void consume();
        void request(std::string_view wire);
        bool queue(Pending next);
        void respond(std::optional<std::uint64_t> id, bool reservation, IpcResult result,
                     bool exit = false);
        void flush();
        void disconnect() noexcept;
        void update_mask() noexcept;

        Impl &server;
        Fd fd;
        Source source;
        std::string input;
        std::deque<Pending> output;
        std::set<std::uint64_t> ids;
        std::size_t queued_bytes = 0;
        std::size_t requests = 0;
        bool subscribed = false;
        bool input_closed = false;
        bool closed = false;
    };

    Impl(wl_event_loop *loop, Handler handler, std::function<void()> on_exit) noexcept
        : loop(loop), handler(std::move(handler)), on_exit(std::move(on_exit)) {}
    ~Impl() noexcept;
    std::expected<void, std::string> listen(std::string_view display);
    std::expected<void, std::string> bind_socket(const sockaddr_un &socket_address);
    static int listen_ready(int fd, std::uint32_t mask, void *data) noexcept;
    static int exit_ready(void *data) noexcept;
    void accept_peers();
    void publish(std::string_view name, IpcJson data) noexcept;
    void send_event(std::shared_ptr<const std::string> record) noexcept;
    void request_exit() noexcept;
    void finish_exit() noexcept;

    wl_event_loop *loop;
    Handler handler;
    std::function<void()> on_exit;
    std::string path;
    std::string name;
    std::string lock_name;
    struct stat socket_identity{};
    struct stat lock_identity{};
    Fd directory;
    Fd lock;
    Fd listener;
    Source listener_source;
    Source exit_timer;
    std::map<int, std::shared_ptr<Peer>> peers;
    std::uint64_t seq = 0;
    bool has_bound = false;
    bool has_lock = false;
    bool exit_requested = false;
    bool exit_finished = false;
};

IpcServer::Impl::~Impl() noexcept {
    if (has_bound && same_entry(directory.get(), name, socket_identity)) {
        unlinkat(directory.get(), name.c_str(), 0);
    }
    if (has_lock && same_entry(directory.get(), lock_name, lock_identity)) {
        unlinkat(directory.get(), lock_name.c_str(), 0);
    }
}

std::expected<void, std::string> IpcServer::Impl::listen(std::string_view display) {
    auto resolved = ipc::runtime_path(display);
    if (!resolved) {
        return std::unexpected(std::move(resolved.error()));
    }
    path = std::move(*resolved);
    auto socket_address = ipc::address(path);
    if (!socket_address) {
        return std::unexpected(std::move(socket_address.error()));
    }

    const auto directory_end = path.rfind('/');
    const auto runtime_name = path.substr(0, path.rfind('/', directory_end - 1));
    name = path.substr(directory_end + 1);
    const Fd runtime(open(runtime_name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!runtime || !private_directory(runtime.get())) {
        return std::unexpected("XDG_RUNTIME_DIR must be owned by the session user with mode 0700");
    }
    if (mkdirat(runtime.get(), "seaside", 0700) < 0 && errno != EEXIST) {
        return std::unexpected(system_error("Create Seaside IPC directory"));
    }
    directory =
        Fd(openat(runtime.get(), "seaside", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory || !private_directory(directory.get())) {
        return std::unexpected(
            "Seaside IPC directory must be owned by the session user with mode 0700");
    }

    // An exclusive lock makes stale-socket cleanup safe when two processes
    // attempt to publish the same instance name together.
    lock_name = name + ".lock";
    lock = Fd(openat(directory.get(), lock_name.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (!lock || fstat(lock.get(), &lock_identity) < 0 || !S_ISREG(lock_identity.st_mode) ||
        lock_identity.st_uid != getuid() || (lock_identity.st_mode & 0777) != 0600) {
        return std::unexpected("Seaside IPC instance lock must be a private regular file");
    }
    if (flock(lock.get(), LOCK_EX | LOCK_NB) < 0 ||
        !same_entry(directory.get(), lock_name, lock_identity)) {
        return std::unexpected("Seaside IPC instance name is already in use");
    }
    has_lock = true;

    listener = Fd(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener) {
        return std::unexpected(system_error("Open Seaside IPC socket"));
    }
    if (auto bound = bind_socket(*socket_address); !bound) {
        return bound;
    }
    if (fchmodat(directory.get(), name.c_str(), 0600, 0) < 0 || ::listen(listener.get(), 32) < 0) {
        return std::unexpected(system_error("Publish Seaside IPC socket"));
    }

    exit_timer.reset(wl_event_loop_add_timer(loop, exit_ready, this));
    listener_source.reset(
        wl_event_loop_add_fd(loop, listener.get(), WL_EVENT_READABLE, listen_ready, this));
    if (!exit_timer || !listener_source) {
        return std::unexpected("Register Seaside IPC with the Wayland event loop");
    }
    return {};
}

std::expected<void, std::string> IpcServer::Impl::bind_socket(const sockaddr_un &socket_address) {
    const auto *address = reinterpret_cast<const sockaddr *>(&socket_address);
    if (bind(listener.get(), address, sizeof(socket_address)) < 0) {
        if (errno != EADDRINUSE) {
            return std::unexpected(system_error("Bind Seaside IPC socket"));
        }
        struct stat existing{};
        const Fd probe(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!probe || fstatat(directory.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) < 0 ||
            !S_ISSOCK(existing.st_mode) || existing.st_uid != getuid() ||
            connect(probe.get(), address, sizeof(socket_address)) == 0 || errno != ECONNREFUSED ||
            !same_entry(directory.get(), name, existing)) {
            return std::unexpected("Seaside IPC socket path is already in use");
        }
        if (unlinkat(directory.get(), name.c_str(), 0) < 0 ||
            bind(listener.get(), address, sizeof(socket_address)) < 0) {
            return std::unexpected(system_error("Replace stale Seaside IPC socket"));
        }
    }
    if (fstatat(directory.get(), name.c_str(), &socket_identity, AT_SYMLINK_NOFOLLOW) < 0) {
        return std::unexpected(system_error("Identify Seaside IPC socket"));
    }
    has_bound = true;
    return {};
}

int IpcServer::Impl::listen_ready(int, std::uint32_t mask, void *data) noexcept {
    auto &server = *static_cast<Impl *>(data);
    try {
        if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
            server.listener_source.reset();
        } else {
            server.accept_peers();
        }
    } catch (...) {
        std::fputs("seaside: could not accept an IPC connection\n", stderr);
    }
    return 0;
}

void IpcServer::Impl::accept_peers() {
    for (int budget = 0; budget < 8; ++budget) {
        Fd fd(accept4(listener.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!fd) {
            return;
        }
        struct ucred credentials{};
        socklen_t length = sizeof(credentials);
        if (peers.size() >= connections_limit ||
            getsockopt(fd.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
            length != sizeof(credentials) || credentials.uid != getuid()) {
            continue;
        }
        const int number = fd.get();
        auto peer = std::make_shared<Peer>(*this, std::move(fd));
        peer->source.reset(
            wl_event_loop_add_fd(loop, number, WL_EVENT_READABLE, Peer::dispatch, peer.get()));
        if (peer->source) {
            peers.emplace(number, std::move(peer));
        }
    }
}

int IpcServer::Impl::Peer::dispatch(int, std::uint32_t mask, void *data) noexcept {
    auto &current = *static_cast<Peer *>(data);
    auto found = current.server.peers.find(current.fd.get());
    if (found == current.server.peers.end()) {
        return 0;
    }
    // The handler can publish events and disconnect its own client through
    // backpressure. Keep the callback's peer alive until dispatch returns.
    auto peer = found->second;
    try {
        peer->ready(mask);
    } catch (...) {
        peer->disconnect();
    }
    return 0;
}

void IpcServer::Impl::Peer::ready(std::uint32_t mask) {
    if (closed) {
        return;
    }
    if (mask & WL_EVENT_ERROR) {
        disconnect();
        return;
    }
    if (mask & (WL_EVENT_WRITABLE | WL_EVENT_HANGUP)) {
        flush();
    }
    if (closed || server.exit_finished) {
        return;
    }

    consume();
    if (!closed && !input_closed && requests < pending_limit &&
        (mask & (WL_EVENT_READABLE | WL_EVENT_HANGUP))) {
        std::array<char, read_chunk> bytes;
        const auto received = recv(fd.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (received > 0) {
            input.append(bytes.data(), static_cast<std::size_t>(received));
            consume();
        } else if (received == 0) {
            input_closed = true;
            if (!input.empty()) {
                respond(
                    ipc::parse_record(input).id, false,
                    std::unexpected(IpcError{"invalid_request", "JSONL record needs a newline"}));
                input.clear();
            }
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            disconnect();
        }
    }
    update_mask();
}

void IpcServer::Impl::Peer::consume() {
    std::size_t begin = 0;
    while (!closed && !input_closed && requests < pending_limit) {
        const auto newline = input.find('\n', begin);
        if (newline == std::string::npos) {
            if (input.size() - begin >= ipc::request_limit) {
                auto id = oversized_id(std::string_view(input).substr(begin));
                input_closed = true;
                begin = input.size();
                respond(id, false,
                        std::unexpected(IpcError{"invalid_request", "Request exceeds 64 KiB"}));
            }
            break;
        }
        if (newline - begin >= ipc::request_limit) {
            respond(oversized_id(std::string_view(input).substr(begin, newline - begin)), false,
                    std::unexpected(IpcError{"invalid_request", "Request exceeds 64 KiB"}));
        } else {
            request(std::string_view(input).substr(begin, newline - begin));
        }
        begin = newline + 1;
    }
    input.erase(0, begin);
}

void IpcServer::Impl::Peer::request(std::string_view wire) {
    auto record = ipc::parse_record(wire);
    const auto id = record.id;
    if (id && !ids.insert(*id).second) {
        respond(id, false,
                std::unexpected(IpcError{"invalid_request", "Request ID is still in flight"}));
        return;
    }
    if (auto error = ipc::validate_request(record)) {
        respond(id, id.has_value(), std::unexpected(std::move(*error)));
        return;
    }

    const auto &method = record.object["method"].get_ref<const std::string &>();
    if (method == "events.subscribe") {
        respond(id, true, IpcJson{{"subscribed", true}});
        subscribed = !closed;
        return;
    }

    IpcResult result = std::unexpected(IpcError{"internal_error", "IPC command did not complete"});
    try {
        result = server.handler(method, record.object["params"]);
    } catch (...) {
        std::fputs("seaside: IPC command failed unexpectedly\n", stderr);
    }
    const bool exit = method == "action.exit" && result.has_value();
    respond(id, true, std::move(result), exit);
    if (exit) {
        server.request_exit();
    }
}

bool IpcServer::Impl::Peer::queue(Pending next) {
    if (closed) {
        return false;
    }
    const auto size = next.wire->size();
    if (output.size() >= queue_records_limit || size > queue_bytes_limit - queued_bytes) {
        disconnect();
        return false;
    }
    output.push_back(std::move(next));
    queued_bytes += size;
    return true;
}

void IpcServer::Impl::Peer::respond(std::optional<std::uint64_t> id, bool reservation,
                                    IpcResult result, bool exit) {
    if (closed) {
        return;
    }
    std::optional<std::string> wire;
    try {
        wire = ipc::serialize(reply(id, server.seq, std::move(result)));
        if (!wire) {
            wire = ipc::serialize(
                reply(id, server.seq,
                      std::unexpected(IpcError{"response_too_large", "Response exceeds 1 MiB"})));
        }
    } catch (...) {
        wire = ipc::serialize(
            reply(id, server.seq,
                  std::unexpected(IpcError{"internal_error", "Could not serialize reply"})));
    }
    if (queue({.wire = std::make_shared<const std::string>(std::move(*wire)),
               .release_id = reservation ? id : std::nullopt,
               .response = true,
               .exit = exit})) {
        ++requests;
    }
}

void IpcServer::Impl::Peer::flush() {
    std::size_t available = write_budget;
    while (!output.empty() && available) {
        auto &record = output.front();
        const auto count = std::min(record.wire->size() - record.sent, available);
        const auto written =
            send(fd.get(), record.wire->data() + record.sent, count, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (written <= 0) {
            if (!written || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
                disconnect();
            }
            return;
        }
        record.sent += static_cast<std::size_t>(written);
        available -= static_cast<std::size_t>(written);
        if (record.sent != record.wire->size()) {
            continue;
        }
        if (record.release_id) {
            ids.erase(*record.release_id);
        }
        if (record.response) {
            --requests;
        }
        const bool exit = record.exit;
        queued_bytes -= record.wire->size();
        output.pop_front();
        if (exit) {
            server.finish_exit();
            return;
        }
    }
}

void IpcServer::Impl::Peer::disconnect() noexcept {
    if (!closed) {
        closed = true;
        source.reset();
        server.peers.erase(fd.get());
    }
}

void IpcServer::Impl::Peer::update_mask() noexcept {
    if (closed) {
        return;
    }
    std::uint32_t mask = 0;
    if (!input_closed && requests < pending_limit) {
        mask |= WL_EVENT_READABLE;
    }
    if (!output.empty()) {
        mask |= WL_EVENT_WRITABLE;
    }
    if (!mask || wl_event_source_fd_update(source.get(), mask) < 0) {
        disconnect();
    }
}

void IpcServer::Impl::publish(std::string_view name, IpcJson data) noexcept {
    if (seq == ipc::id_limit) {
        return;
    }
    ++seq;
    if (!std::ranges::any_of(peers, [](const auto &entry) { return entry.second->subscribed; })) {
        return;
    }
    std::shared_ptr<const std::string> record;
    try {
        auto serialized =
            ipc::serialize({{"v", 1}, {"event", name}, {"seq", seq}, {"data", std::move(data)}});
        if (serialized) {
            record = std::make_shared<const std::string>(std::move(*serialized));
        }
    } catch (...) {
        std::fputs("seaside: could not serialize an IPC event\n", stderr);
    }
    send_event(std::move(record));
}

void IpcServer::Impl::send_event(std::shared_ptr<const std::string> record) noexcept {
    for (auto next = peers.begin(); next != peers.end();) {
        auto peer = (next++)->second;
        if (!peer->subscribed) {
            continue;
        }
        try {
            if (record && peer->queue({.wire = record, .release_id = std::nullopt})) {
                peer->update_mask();
            } else {
                peer->disconnect();
            }
        } catch (...) {
            peer->disconnect();
        }
    }
}

void IpcServer::Impl::request_exit() noexcept {
    if (!exit_requested) {
        exit_requested = true;
        if (wl_event_source_timer_update(exit_timer.get(), exit_deadline_ms) < 0) {
            finish_exit();
        }
    }
}

int IpcServer::Impl::exit_ready(void *data) noexcept {
    static_cast<Impl *>(data)->finish_exit();
    return 0;
}

void IpcServer::Impl::finish_exit() noexcept {
    if (exit_finished) {
        return;
    }
    exit_finished = true;
    wl_event_source_timer_update(exit_timer.get(), 0);
    try {
        on_exit();
    } catch (...) {
        std::fputs("seaside: IPC exit callback failed unexpectedly\n", stderr);
    }
}

std::expected<std::unique_ptr<IpcServer>, std::string>
IpcServer::create(wl_event_loop *loop, std::string_view wayland_display, Handler handler,
                  std::function<void()> on_exit) {
    if (!loop || !handler || !on_exit) {
        return std::unexpected("Seaside IPC needs a Wayland loop and command and exit callbacks");
    }
    try {
        auto impl = std::make_unique<Impl>(loop, std::move(handler), std::move(on_exit));
        if (auto result = impl->listen(wayland_display); !result) {
            return std::unexpected(std::move(result.error()));
        }
        return std::unique_ptr<IpcServer>(new IpcServer(std::move(impl)));
    } catch (const std::exception &error) {
        return std::unexpected(std::string("Initialize Seaside IPC: ") + error.what());
    }
}

IpcServer::IpcServer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
IpcServer::~IpcServer() noexcept = default;

const std::string &IpcServer::path() const noexcept {
    return impl_->path;
}

void IpcServer::event(std::string_view name, IpcJson data) noexcept {
    impl_->publish(name, std::move(data));
}

} // namespace seaside

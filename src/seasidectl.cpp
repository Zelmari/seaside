#include "build_info.hpp"
#include "ipc_protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using seaside::IpcJson;
namespace ipc = seaside::ipc;
using Args = std::span<char *>;
using Clock = std::chrono::steady_clock;
using Deadline = std::optional<Clock::time_point>;
using Command = std::pair<std::string_view, IpcJson>;

constexpr auto connection_timeout = std::chrono::seconds(5);
constexpr std::string_view usage =
    "Usage: seasidectl status|bindings|outputs|workspaces|windows|exit|events\n"
    "       seasidectl focus [left|right|up|down]\n"
    "       seasidectl workspace ID\n"
    "       seasidectl scratchpad toggle\n"
    "       seasidectl window toggle-tiling|toggle-pin|toggle-maximize|toggle-fullscreen|close\n"
    "       seasidectl window move|swap DIR\n"
    "       seasidectl window resize DIR [DELTA]\n"
    "       seasidectl window to-workspace ID | to-scratchpad\n"
    "       seasidectl window icon ID\n"
    "       seasidectl night-light status|on|off|toggle\n"
    "       seasidectl settings [set SETTING VALUE]\n"
    "       seasidectl keybind set CHORD ACTION | unset CHORD | reset\n"
    "       seasidectl debug render\n"
    "       seasidectl trace start|stop|export\n"
    "       seasidectl --help | --version\n";

IpcJson integer(std::string_view input) {
    std::uint64_t value = 0;
    auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size()) {
        return nullptr;
    }
    return value;
}

std::optional<Command> window_command(Args args) {
    if (args.empty()) {
        return {};
    }
    constexpr std::pair<std::string_view, std::string_view> simple[]{
        {"toggle-tiling", "action.window_toggle_tiling"},
        {"toggle-pin", "action.window_toggle_pin"},
        {"toggle-maximize", "action.window_toggle_maximize"},
        {"toggle-fullscreen", "action.window_toggle_fullscreen"},
        {"close", "action.window_close"},
        {"to-scratchpad", "action.window_to_scratchpad"},
    };
    const std::string_view operation = args[0];
    if (args.size() == 1) {
        for (const auto &[name, method] : simple) {
            if (name == operation) {
                return Command{method, IpcJson::object()};
            }
        }
    }
    if (args.size() == 2 && (operation == "move" || operation == "swap")) {
        const auto method = operation == "move" ? "action.window_move" : "action.window_swap";
        return Command{method, IpcJson{{"direction", args[1]}}};
    }
    if ((args.size() == 2 || args.size() == 3) && operation == "resize") {
        IpcJson params{{"direction", args[1]}};
        if (args.size() == 3) {
            params["delta"] = integer(args[2]);
        }
        return Command{"action.window_resize", std::move(params)};
    }
    if (args.size() == 2 && operation == "to-workspace") {
        return Command{"action.window_to_workspace", IpcJson{{"workspace_id", integer(args[1])}}};
    }
    if (args.size() == 2 && operation == "icon") {
        return Command{"query.window_icon", IpcJson{{"window_id", integer(args[1])}}};
    }
    return {};
}

Command settings_update(std::string_view name, IpcJson value) {
    return {"action.settings_update", {{"setting", name}, {"value", std::move(value)}}};
}

std::optional<Command> settings_command(Args args) {
    if (args.size() != 3 || std::string_view(args[0]) != "set")
        return {};
    const std::string_view name = args[1], value = args[2];
    const auto setting = seaside::parse_setting(name);
    if (!setting || *setting == seaside::Setting::keybinds)
        return {};
    switch (*setting) {
    case seaside::Setting::idle_minutes:
    case seaside::Setting::night_light_temperature:
        return settings_update(name, integer(value));
    case seaside::Setting::power_profile:
        return settings_update(name, value == "system" ? IpcJson(nullptr) : IpcJson(value));
    default:
        if (value == "true" || value == "false")
            return settings_update(name, value == "true");
        return settings_update(name, value);
    }
}

std::optional<Command> keybind_command(Args args) {
    const std::string_view operation = args[0];
    if (args.size() == 1 && operation == "reset")
        return settings_update("keybinds", nullptr);
    if ((args.size() == 3 && operation == "set") || (args.size() == 2 && operation == "unset")) {
        auto result = settings_update("keybinds", operation == "set" ? IpcJson(args[2]) : nullptr);
        result.second["chord"] = args[1];
        return result;
    }
    return {};
}

std::optional<Command> command(Args args) {
    if (args.empty()) {
        return {};
    }
    constexpr std::pair<std::string_view, std::string_view> simple[]{
        {"status", "query.status"},     {"bindings", "query.bindings"},
        {"outputs", "query.outputs"},   {"workspaces", "query.workspaces"},
        {"windows", "query.windows"},   {"focus", "query.focus"},
        {"events", "events.subscribe"}, {"exit", "action.exit"},
        {"settings", "query.settings"},
    };
    const std::string_view group = args[0];
    if (args.size() == 1) {
        for (const auto &[name, method] : simple) {
            if (name == group) {
                return Command{method, IpcJson::object()};
            }
        }
        return {};
    }
    if (group == "window") {
        return window_command(args.subspan(1));
    }
    if (group == "settings")
        return settings_command(args.subspan(1));
    if (group == "keybind")
        return keybind_command(args.subspan(1));
    const std::string_view operation = args[1];
    if (args.size() != 2) {
        return {};
    }
    if (group == "scratchpad" && operation == "toggle") {
        return Command{"action.scratchpad_toggle", IpcJson::object()};
    }
    if (group == "workspace") {
        return Command{"action.workspace_select", IpcJson{{"workspace_id", integer(operation)}}};
    }
    if (group == "focus") {
        return Command{"action.focus", IpcJson{{"direction", operation}}};
    }
    if (group == "night-light" && operation == "status") {
        return Command{"query.night_light", IpcJson::object()};
    }
    if (group == "night-light" && (operation == "on" || operation == "off")) {
        return Command{"action.night_light_set", IpcJson{{"enabled", operation == "on"}}};
    }
    if (group == "night-light" && operation == "toggle")
        return settings_update("night_light.enabled", "toggle");
    if (group == "debug" && operation == "render") {
        return Command{"query.diagnostics", IpcJson::object()};
    }
    if (group == "trace") {
        constexpr std::pair<std::string_view, std::string_view> trace[]{
            {"start", "trace.start"}, {"stop", "trace.stop"}, {"export", "trace.export"}};
        for (const auto &[name, method] : trace) {
            if (operation == name) {
                return Command{method, IpcJson::object()};
            }
        }
    }
    return {};
}

std::expected<std::string, std::string> socket_path() {
    if (const char *value = std::getenv("SEASIDE_SOCKET")) {
        return std::string(value);
    }
    const char *display = std::getenv("WAYLAND_DISPLAY");
    return ipc::runtime_path(display ? display : "");
}

class Client {
  public:
    Client() noexcept : fd_(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) {}
    ~Client() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    std::expected<void, std::string> open(const sockaddr_un &address, Deadline deadline);
    std::expected<void, std::string> send_all(std::string_view request, Deadline deadline);
    std::expected<std::string, std::string> receive(Deadline deadline);

  private:
    std::expected<void, std::string> wait(short interest, Deadline deadline);
    int fd_;
    std::string unread_;
};

std::expected<void, std::string> Client::wait(short interest, Deadline deadline) {
    while (true) {
        int timeout = -1;
        if (deadline) {
            const auto remaining =
                std::chrono::ceil<std::chrono::milliseconds>(*deadline - Clock::now());
            if (remaining.count() <= 0) {
                return std::unexpected("Timed out waiting for Seaside");
            }
            timeout = static_cast<int>(remaining.count());
        }
        pollfd pending{.fd = fd_, .events = interest, .revents = 0};
        const int ready = poll(&pending, 1, timeout);
        if (ready > 0) {
            return {};
        }
        if (ready == 0) {
            return std::unexpected("Timed out waiting for Seaside");
        }
        if (errno != EINTR) {
            return std::unexpected(std::strerror(errno));
        }
    }
}

std::expected<void, std::string> Client::open(const sockaddr_un &address, Deadline deadline) {
    if (fd_ < 0) {
        return std::unexpected("Could not allocate a Unix socket");
    }
    if (connect(fd_, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0) {
        if (errno != EINPROGRESS && errno != EINTR) {
            return std::unexpected(std::strerror(errno));
        }
        if (auto connected = wait(POLLOUT, deadline); !connected) {
            return connected;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            return std::unexpected(std::strerror(errno));
        }
        if (error) {
            return std::unexpected(std::strerror(error));
        }
    }
    struct ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd_, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0 ||
        length != sizeof(credentials) || credentials.uid != getuid()) {
        return std::unexpected("The Seaside socket peer does not belong to this user");
    }
    return {};
}

std::expected<void, std::string> Client::send_all(std::string_view request, Deadline deadline) {
    while (!request.empty()) {
        if (auto ready = wait(POLLOUT, deadline); !ready) {
            return ready;
        }
        const auto size = send(fd_, request.data(), request.size(), MSG_NOSIGNAL);
        if (size > 0) {
            request.remove_prefix(static_cast<std::size_t>(size));
        } else if (!size) {
            return std::unexpected("Seaside closed the connection while sending a request");
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return std::unexpected(std::strerror(errno));
        }
    }
    return {};
}

std::expected<std::string, std::string> Client::receive(Deadline deadline) {
    while (true) {
        const auto newline = unread_.find('\n');
        if (newline < ipc::record_limit) {
            auto record = unread_.substr(0, newline);
            unread_.erase(0, newline + 1);
            return record;
        }
        if (unread_.size() >= ipc::record_limit) {
            return std::unexpected("Seaside returned a record over 1 MiB");
        }
        if (auto ready = wait(POLLIN, deadline); !ready) {
            return std::unexpected(std::move(ready.error()));
        }
        std::array<char, 16 * 1024> buffer;
        const auto size = recv(fd_, buffer.data(), buffer.size(), 0);
        if (size > 0) {
            unread_.append(buffer.data(), static_cast<std::size_t>(size));
        } else if (!size) {
            return std::unexpected("Seaside closed the connection");
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return std::unexpected(std::strerror(errno));
        }
    }
}

bool has_sequence(const IpcJson &record) {
    const auto seq = record.find("seq");
    return seq != record.end() && seq->is_number_unsigned() &&
           seq->get<std::uint64_t>() <= ipc::id_limit;
}

int error(std::string_view message) {
    std::cerr << "seasidectl: " << message << '\n';
    return 1;
}

int run(Args args) {
    if (args.size() == 1 && std::string_view(args.front()) == "--help") {
        std::cout << usage;
        return 0;
    }
    if (args.size() == 1 && std::string_view(args.front()) == "--version") {
        std::cout << "seasidectl " << SEASIDE_BUILD_VERSION << " (revision "
                  << SEASIDE_BUILD_REVISION << ")\n";
        return 0;
    }
    const auto selected = command(args);
    if (!selected) {
        std::cerr << usage;
        return 1;
    }
    const auto &[method, params] = *selected;
    const auto request =
        ipc::serialize({{"v", 1}, {"id", 1}, {"method", method}, {"params", params}});
    if (!request || request->size() > ipc::request_limit) {
        return error("Request exceeds 64 KiB");
    }
    if (const auto invalid = ipc::validate_request(ipc::parse_record(*request))) {
        return error(invalid->message);
    }

    const auto path = socket_path();
    if (!path) {
        return error(path.error());
    }
    const auto address = ipc::address(*path);
    if (!address) {
        return error(address.error());
    }
    Client client;
    const auto deadline = Clock::now() + connection_timeout;
    if (const auto connected = client.open(*address, deadline); !connected) {
        return error(connected.error());
    }
    if (const auto written = client.send_all(*request, deadline); !written) {
        return error(written.error());
    }
    const auto wire = client.receive(deadline);
    if (!wire) {
        return error(wire.error());
    }
    const auto response = ipc::parse_record(*wire);
    const auto &object = response.object;
    if (!response.error.empty() || response.id != 1 || !object.contains("v") ||
        !object["v"].is_number_integer() || object["v"] != 1 || !has_sequence(object) ||
        !object.contains("ok") || !object["ok"].is_boolean()) {
        return error("Invalid response from Seaside");
    }
    if (object["ok"] != true) {
        if (!object.contains("error") || !object["error"].is_object() ||
            !object["error"].contains("code") || !object["error"]["code"].is_string() ||
            !object["error"].contains("message") || !object["error"]["message"].is_string()) {
            return error("Invalid error response from Seaside");
        }
        std::cout << *wire << '\n';
        return error(object["error"]["code"].dump() + " " + object["error"]["message"].dump());
    }
    if (!object.contains("result") || !object["result"].is_object()) {
        return error("Missing result in Seaside response");
    }
    std::cout << *wire << '\n' << std::flush;
    if (method != "events.subscribe") {
        return std::cout ? 0 : error("Could not write the response");
    }
    auto seq = object["seq"].get<std::uint64_t>();
    while (std::cout) {
        const auto event_wire = client.receive(std::nullopt);
        if (!event_wire) {
            return error(event_wire.error());
        }
        const auto event = ipc::parse_record(*event_wire);
        const auto &data = event.object;
        if (!event.error.empty() || !data.contains("v") || !data["v"].is_number_integer() ||
            data["v"] != 1 || !has_sequence(data) || !data.contains("event") ||
            !data["event"].is_string() || !data.contains("data") || !data["data"].is_object() ||
            data["seq"].get<std::uint64_t>() <= seq) {
            return error("Invalid event from Seaside");
        }
        seq = data["seq"].get<std::uint64_t>();
        std::cout << *event_wire << '\n' << std::flush;
    }
    return error("Could not write an event");
}

} // namespace

int main(int argc, char *argv[]) {
    try {
        return run({argv + 1, static_cast<std::size_t>(argc - 1)});
    } catch (const std::exception &) {
        return error("Could not prepare or decode the IPC request");
    }
}

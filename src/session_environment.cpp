#include "session_environment.hpp"
#include "storage_fd.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <unistd.h>

namespace seaside {
namespace {
constexpr char manager[] = "org.freedesktop.systemd1";
constexpr char path[] = "/org/freedesktop/systemd1";
constexpr char interface[] = "org.freedesktop.systemd1.Manager";
constexpr std::array keys{"WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
                          "DISPLAY",         "XAUTHORITY",          "XCURSOR_SIZE"};
using Environment = std::array<std::optional<std::string>, keys.size()>;
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;

bool snapshot(sd_bus *bus, Environment &env) {
    sd_bus_message *raw = nullptr;
    const int r =
        sd_bus_get_property(bus, manager, path, interface, "Environment", nullptr, &raw, "as");
    Message reply(raw, sd_bus_message_unref);
    if (r < 0 || sd_bus_message_enter_container(raw, 'a', "s") <= 0) {
        return false;
    }
    const char *entry;
    int count;
    while ((count = sd_bus_message_read(raw, "s", &entry)) > 0) {
        const std::string_view line(entry);
        const auto separator = line.find('=');
        for (size_t i = 0; i < keys.size(); ++i) {
            if (separator != line.npos && line.substr(0, separator) == keys[i]) {
                env[i] = line.substr(separator + 1);
            }
        }
    }
    return count == 0;
}

bool update(sd_bus *bus, const Environment &from, const Environment &to,
            const Environment &observed) {
    sd_bus_message *raw = nullptr;
    const int r = sd_bus_message_new_method_call(bus, &raw, manager, path, interface,
                                                 "UnsetAndSetEnvironment");
    Message call(raw, sd_bus_message_unref);
    if (r < 0) {
        return false;
    }
    for (bool set : {false, true}) {
        if (sd_bus_message_open_container(raw, 'a', "s") < 0) {
            return false;
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            if (from[i] == to[i] || from[i] != observed[i] || set != to[i].has_value()) {
                continue;
            }
            // systemd supports conditional removal using the exact key=value.
            const auto &value = set ? to[i] : from[i];
            const std::string assignment = std::string(keys[i]) + "=" + *value;
            if (sd_bus_message_append(raw, "s", assignment.c_str()) < 0) {
                return false;
            }
        }
        if (sd_bus_message_close_container(raw) < 0) {
            return false;
        }
    }
    return sd_bus_call(bus, raw, 0, nullptr, nullptr) >= 0;
}
} // namespace

int run_session_environment(const char *wayland, const char *display, const char *authority,
                            const char *cursor_size) {
    struct stat pipe{};
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !std::getenv("NOTIFY_SOCKET") || fstat(STDIN_FILENO, &pipe) < 0 ||
        !S_ISFIFO(pipe.st_mode)) {
        return EXIT_FAILURE;
    }
    // Serialize publication and restoration across Seaside instances, including
    // an immediate relaunch while the old instance's pipe is reaching EOF.
    const std::string lock_path = std::string(runtime) + "/seaside/activation.lock";
    StorageFd lock(open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (lock.get() < 0 || flock(lock.get(), LOCK_EX | LOCK_NB) < 0) {
        return EXIT_FAILURE;
    }
    sd_bus *raw = nullptr;
    const int r = sd_bus_open_user(&raw);
    const std::unique_ptr<sd_bus, decltype(&sd_bus_unref)> bus(raw, sd_bus_unref);
    Environment before, after;
    Environment session{
        std::string(wayland),    std::string("Seaside"), std::string("wayland"), {}, {},
        std::string(cursor_size)};
    if (*display && *authority) {
        session[3] = display;
        session[4] = authority;
    }
    if (r < 0 || sd_bus_set_method_call_timeout(raw, 2'000'000) < 0 || !snapshot(raw, before) ||
        !update(raw, before, session, before)) {
        std::fputs("seaside: could not publish user service activation environment\n", stderr);
        return EXIT_FAILURE;
    }
    const bool ready = sd_notify(0, "READY=1") > 0;
    if (ready) {
        char byte;
        while (read(STDIN_FILENO, &byte, 1) < 0 && errno == EINTR) {
        }
    }
    if (!snapshot(raw, after) || !update(raw, session, before, after)) {
        std::fputs("seaside: could not restore user service activation environment\n", stderr);
        return EXIT_FAILURE;
    }
    return ready ? EXIT_SUCCESS : EXIT_FAILURE;
}
} // namespace seaside

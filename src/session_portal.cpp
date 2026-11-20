#include "session_portal.hpp"

#include "session_bus.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <systemd/sd-bus.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;

constexpr char frontend_name[] = "org.freedesktop.portal.Desktop";
constexpr char backend_name[] = "org.freedesktop.impl.portal.desktop.wlr";
constexpr char desktop_path[] = "/org/freedesktop/portal/desktop";
constexpr char frontend_cast[] = "org.freedesktop.portal.ScreenCast";
constexpr char backend_cast[] = "org.freedesktop.impl.portal.ScreenCast";
constexpr char frontend_shot[] = "org.freedesktop.portal.Screenshot";
constexpr char backend_shot[] = "org.freedesktop.impl.portal.Screenshot";
constexpr char bus_name[] = "org.freedesktop.DBus";
constexpr char bus_path[] = "/org/freedesktop/DBus";
constexpr char properties[] = "org.freedesktop.DBus.Properties";
constexpr char request_interface[] = "org.freedesktop.portal.Request";
constexpr char session_interface[] = "org.freedesktop.portal.Session";
constexpr char impl_session_xml[] = "<interface name=\"org.freedesktop.impl.portal.Session\">";
constexpr ServiceReport malformed{ServiceFailure::incompatible, -EBADMSG};
constexpr ServiceReport expired{ServiceFailure::timeout, -ETIMEDOUT};
constexpr ServiceReport wrong_display{ServiceFailure::wrong_environment, -EXDEV};

enum class Step {
    frontend_cast,
    frontend_shot,
    backend_cast,
    backend_shot,
    credentials,
    binding,
    create,
    response,
    prove_route,
    close,
    frontend_owner,
    backend_owner,
};

StartupService service_for(Step step) noexcept {
    return step == Step::frontend_cast || step == Step::frontend_shot || step == Step::create ||
                   step == Step::frontend_owner
               ? StartupService::portal_frontend
               : StartupService::capture_backend;
}

ServiceReport transport_error(BusError error, int native = 0) noexcept {
    switch (error) {
    case BusError::disconnected:
    case BusError::stopped:
        return {ServiceFailure::disconnected, native < 0 ? native : -ENOTCONN};
    case BusError::timeout:
        return expired;
    case BusError::allocation:
        return {ServiceFailure::internal_error, -ENOMEM};
    default:
        return {ServiceFailure::internal_error, native < 0 ? native : -EIO};
    }
}

ServiceReport protocol_error(const BusReply &reply, Step step) noexcept {
    if (reply.error != BusError::remote_error) {
        return transport_error(reply.error, reply.native_error);
    }
    const auto *error = reply.message ? sd_bus_message_get_error(reply.message) : nullptr;
    const std::string_view name = error && error->name ? error->name : "";
    if (name == SD_BUS_ERROR_SERVICE_UNKNOWN || name == SD_BUS_ERROR_NAME_HAS_NO_OWNER) {
        const auto code = step == Step::frontend_cast || step == Step::backend_cast
                              ? ServiceFailure::missing
                              : ServiceFailure::disconnected;
        return {code, reply.native_error};
    }
    if (name == SD_BUS_ERROR_UNKNOWN_METHOD || name == SD_BUS_ERROR_UNKNOWN_OBJECT ||
        name == SD_BUS_ERROR_UNKNOWN_INTERFACE || name == SD_BUS_ERROR_UNKNOWN_PROPERTY ||
        name == SD_BUS_ERROR_NOT_SUPPORTED || name == SD_BUS_ERROR_INVALID_SIGNATURE) {
        return {ServiceFailure::incompatible, reply.native_error};
    }
    if (name == SD_BUS_ERROR_TIMED_OUT) {
        return expired;
    }
    return {ServiceFailure::activation_failed, reply.native_error};
}

template <class ReadProperty>
bool read_properties(sd_bus_message *message, ReadProperty read) noexcept {
    if (sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
        return false;
    }
    int entry = 0;
    while ((entry = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
        const char *name = nullptr;
        if (sd_bus_message_read(message, "s", &name) <= 0 || !read(std::string_view(name)) ||
            sd_bus_message_exit_container(message) < 0) {
            return false;
        }
    }
    return entry == 0 && sd_bus_message_exit_container(message) >= 0;
}

bool read_unsigned(sd_bus_message *message, std::optional<std::uint32_t> &result) noexcept {
    std::uint32_t value = 0;
    if (result || sd_bus_message_read(message, "v", "u", &value) <= 0) {
        return false;
    }
    result = value;
    return true;
}

bool read_screencast(sd_bus_message *message) noexcept {
    std::optional<std::uint32_t> version, source_types, cursor_modes;
    const bool valid = sd_bus_message_has_signature(message, "a{sv}") > 0 &&
                       read_properties(message, [&](std::string_view name) {
                           if (name == "version") {
                               return read_unsigned(message, version);
                           }
                           if (name == "AvailableSourceTypes") {
                               return read_unsigned(message, source_types);
                           }
                           if (name == "AvailableCursorModes") {
                               return read_unsigned(message, cursor_modes);
                           }
                           return sd_bus_message_skip(message, "v") > 0;
                       });
    constexpr std::uint32_t monitor = 1;
    constexpr std::uint32_t hidden_and_embedded = 1 | 2;
    return valid && version.value_or(0) >= 4 && (source_types.value_or(0) & monitor) != 0 &&
           (cursor_modes.value_or(0) & hidden_and_embedded) == hidden_and_embedded;
}

bool read_screenshot_version(sd_bus_message *message) noexcept {
    std::uint32_t version = 0;
    return sd_bus_message_has_signature(message, "v") > 0 &&
           sd_bus_message_read(message, "v", "u", &version) > 0 && version >= 1;
}

int append_option(sd_bus_message *message, const char *name, const char *value) noexcept {
    return sd_bus_message_append(message, "{sv}", name, "s", value);
}

bool matches_sender(sd_bus_message *message, std::string_view name) noexcept {
    const auto *sender = sd_bus_message_get_sender(message);
    return sender && sender == name;
}

bool is_unique_name(const char *name) noexcept {
    return name && name[0] == ':' && sd_bus_service_name_is_valid(name) > 0;
}
} // namespace

struct PortalProbe::Impl final : std::enable_shared_from_this<Impl> {
    struct Watch {
        Impl *probe;
        StartupService service;
        const char *name;
        const std::string &owner;
        sd_bus_slot *slot = nullptr;
        bool installed = false;
        bool lost = false;
        bool reported = false;
    };

    Impl(SessionBus &transport, uid_t uid, BindingCheck bound, Completion complete,
         Clock::time_point limit, Completion fault) noexcept
        : bus(transport), expected_uid(uid), binding(std::move(bound)),
          completion(std::move(complete)), runtime_fault(std::move(fault)), deadline(limit) {}

    SessionBus &bus;
    const uid_t expected_uid;
    BindingCheck binding;
    Completion completion;
    Completion runtime_fault;
    const Clock::time_point deadline;
    Clock::time_point binding_deadline{};
    Step step = Step::frontend_cast;
    std::optional<SessionBus::CallId> pending;
    wl_event_source *timer = nullptr;
    sd_bus_slot *response_filter = nullptr;
    std::string frontend_owner, backend_owner, request_path, session_path, token;
    std::array<Watch, 2> watches{{
        {this, StartupService::portal_frontend, frontend_name, frontend_owner},
        {this, StartupService::capture_backend, backend_name, backend_owner},
    }};
    pid_t backend_pid = 0;
    std::optional<ServiceReport> response;
    bool create_sent = false;
    bool ticking = false;
    bool abandoned = false;
    bool monitoring = false;

    ServiceReport request(Step next) noexcept {
        step = next;
        auto *connection = bus.native_handle();
        if (!connection) {
            return transport_error(bus.last_error());
        }
        sd_bus_message *raw = nullptr;
        int result = 0;
        switch (next) {
        case Step::frontend_cast:
        case Step::backend_cast:
            result = sd_bus_message_new_method_call(
                connection, &raw, next == Step::frontend_cast ? frontend_name : backend_name,
                desktop_path, properties, "GetAll");
            break;
        case Step::frontend_shot:
        case Step::backend_shot:
            result = sd_bus_message_new_method_call(
                connection, &raw,
                next == Step::frontend_shot ? frontend_owner.c_str() : backend_owner.c_str(),
                desktop_path, properties, "Get");
            break;
        case Step::credentials:
            result = sd_bus_message_new_method_call(connection, &raw, bus_name, bus_path, bus_name,
                                                    "GetConnectionCredentials");
            break;
        case Step::frontend_owner:
        case Step::backend_owner:
            result = sd_bus_message_new_method_call(connection, &raw, bus_name, bus_path, bus_name,
                                                    "GetNameOwner");
            break;
        case Step::create:
            result = sd_bus_message_new_method_call(connection, &raw, frontend_owner.c_str(),
                                                    desktop_path, frontend_cast, "CreateSession");
            break;
        case Step::prove_route:
            result = sd_bus_message_new_method_call(
                connection, &raw, backend_owner.c_str(), session_path.c_str(),
                "org.freedesktop.DBus.Introspectable", "Introspect");
            break;
        case Step::close:
            result =
                sd_bus_message_new_method_call(connection, &raw, frontend_owner.c_str(),
                                               session_path.c_str(), session_interface, "Close");
            break;
        case Step::binding:
        case Step::response:
            return {ServiceFailure::internal_error, -EINVAL};
        }
        Message message(raw, sd_bus_message_unref);
        if (result >= 0) {
            result = sd_bus_message_set_allow_interactive_authorization(raw, 0);
        }
        if (result >= 0) {
            // The activation owner starts the session's dedicated services.
            // A lost name must not start a stock service in the login scope.
            result = sd_bus_message_set_auto_start(raw, 0);
        }
        if (result >= 0) {
            switch (next) {
            case Step::frontend_cast:
            case Step::backend_cast:
                result = sd_bus_message_append(
                    raw, "s", next == Step::frontend_cast ? frontend_cast : backend_cast);
                break;
            case Step::frontend_shot:
            case Step::backend_shot:
                result = sd_bus_message_append(
                    raw, "ss", next == Step::frontend_shot ? frontend_shot : backend_shot,
                    "version");
                break;
            case Step::credentials:
                result = sd_bus_message_append(raw, "s", backend_owner.c_str());
                break;
            case Step::frontend_owner:
            case Step::backend_owner:
                result = sd_bus_message_append(
                    raw, "s", next == Step::frontend_owner ? frontend_name : backend_name);
                break;
            case Step::create:
                result = sd_bus_message_open_container(raw, SD_BUS_TYPE_ARRAY, "{sv}");
                if (result >= 0) {
                    result = append_option(raw, "handle_token", token.c_str());
                }
                if (result >= 0) {
                    result = append_option(raw, "session_handle_token", token.c_str());
                }
                if (result >= 0) {
                    result = sd_bus_message_close_container(raw);
                }
                break;
            default:
                break;
            }
        }
        if (result < 0) {
            return {result == -ENOTCONN ? ServiceFailure::disconnected
                                        : ServiceFailure::internal_error,
                    result};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            return expired;
        }
        try {
            auto call = bus.call(raw, remaining, [weak = weak_from_this()](const BusReply &reply) {
                if (auto self = weak.lock()) {
                    self->receive(reply);
                }
            });
            if (!call) {
                return transport_error(call.error());
            }
            pending = *call;
            if (next == Step::create) {
                create_sent = true;
            }
            return {};
        } catch (...) {
            return {ServiceFailure::internal_error, -ENOMEM};
        }
    }

    void best_effort_close(const std::string &path, const char *interface) noexcept {
        auto *connection = bus.native_handle();
        if (!connection) {
            return;
        }
        sd_bus_message *raw = nullptr;
        const auto result = sd_bus_message_new_method_call(connection, &raw, frontend_owner.c_str(),
                                                           path.c_str(), interface, "Close");
        Message message(raw, sd_bus_message_unref);
        if (result < 0 || sd_bus_message_set_auto_start(raw, 0) < 0 ||
            sd_bus_message_set_allow_interactive_authorization(raw, 0) < 0) {
            return;
        }
        try {
            (void)bus.call(raw, 250ms, [](const BusReply &) {});
        } catch (...) {
            // When the bus is gone, the frontend owns peer-disconnect cleanup.
        }
    }

    void stop() noexcept {
        if (!monitoring) {
            if (!ticking) {
                if (auto *source = std::exchange(timer, nullptr)) {
                    wl_event_source_remove(source);
                }
            }
            response_filter = sd_bus_slot_unref(response_filter);
            for (auto &watch : watches) {
                watch.slot = sd_bus_slot_unref(watch.slot);
            }
        }
        if (auto call = std::exchange(pending, std::nullopt)) {
            (void)bus.cancel(*call);
        }
        if (std::exchange(create_sent, false)) {
            best_effort_close(request_path, request_interface);
            best_effort_close(session_path, session_interface);
        }
    }

    void finish(ServiceReport report) noexcept {
        auto notify = std::exchange(completion, {});
        stop();
        if (!notify || abandoned) {
            return;
        }
        try {
            if (report.failure != ServiceFailure::none) {
                notify(service_for(step), report);
                return;
            }
            notify(StartupService::portal_frontend, {});
            if (!abandoned) {
                notify(StartupService::capture_backend, {});
            }
        } catch (...) {
            // Completion runs outside sd-bus dispatch and cannot cross Wayland.
        }
    }

    void advance(Step next) noexcept {
        if (const auto result = request(next); result.failure != ServiceFailure::none) {
            finish(result);
        }
    }

    ServiceReport read_backend_credentials(sd_bus_message *message) noexcept {
        std::optional<std::uint32_t> uid, pid;
        const bool valid = matches_sender(message, bus_name) &&
                           sd_bus_message_has_signature(message, "a{sv}") > 0 &&
                           read_properties(message, [&](std::string_view name) {
                               if (name == "UnixUserID") {
                                   return read_unsigned(message, uid);
                               }
                               if (name == "ProcessID") {
                                   return read_unsigned(message, pid);
                               }
                               return sd_bus_message_skip(message, "v") > 0;
                           });
        if (!valid || !uid || !pid ||
            *pid > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) {
            return malformed;
        }
        if (*uid != expected_uid || *pid <= 1) {
            return {ServiceFailure::wrong_environment, -EPERM};
        }
        backend_pid = static_cast<pid_t>(*pid);
        return {};
    }

    ServiceReport read_response(sd_bus_message *message) const noexcept {
        std::uint32_t result = 0;
        if (sd_bus_message_has_signature(message, "ua{sv}") <= 0 ||
            sd_bus_message_read(message, "u", &result) <= 0) {
            return malformed;
        }
        if (result != 0) {
            return {ServiceFailure::activation_failed, result == 1 ? -ECANCELED : -EIO};
        }
        bool received_handle = false;
        const bool valid = read_properties(message, [&](std::string_view name) {
            if (name != "session_handle") {
                return sd_bus_message_skip(message, "v") > 0;
            }
            char type = 0;
            const char *signature = nullptr;
            const char *handle = nullptr;
            if (received_handle || sd_bus_message_peek_type(message, &type, &signature) <= 0 ||
                type != SD_BUS_TYPE_VARIANT ||
                (std::string_view(signature) != "s" && std::string_view(signature) != "o") ||
                sd_bus_message_read(message, "v", signature, &handle) <= 0 ||
                session_path != handle) {
                return false;
            }
            received_handle = true;
            return true;
        });
        return valid && received_handle ? ServiceReport{} : malformed;
    }

    void schedule_faults() noexcept {
        if (!monitoring || ticking ||
            std::none_of(watches.begin(), watches.end(),
                         [](const Watch &watch) { return watch.lost && !watch.reported; })) {
            return;
        }
        if (wl_event_source_timer_update(timer, 1) < 0) {
            // This runs inside sd-bus dispatch. Never call user code here,
            // including if the Wayland notification mechanism breaks.
            for (auto &watch : watches) {
                if (watch.lost && !std::exchange(watch.reported, true)) {
                    std::fprintf(stderr, "seaside: portal owner disappeared: %s\n", watch.name);
                }
            }
        }
    }

    void deliver_faults() noexcept {
        auto notify = std::exchange(runtime_fault, {});
        for (auto &watch : watches) {
            if (notify && !abandoned && watch.lost && !std::exchange(watch.reported, true)) {
                try {
                    notify(watch.service, {ServiceFailure::disconnected, -ENOTCONN});
                } catch (...) {
                    // A runtime observer cannot throw across the Wayland loop.
                }
            }
        }
        if (!abandoned) {
            runtime_fault = std::move(notify);
        }
    }

    static int watch_installed(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &watch = *static_cast<Watch *>(data);
        watch.installed = matches_sender(message, bus_name) &&
                          sd_bus_message_has_signature(message, "") > 0 &&
                          sd_bus_message_is_method_error(message, nullptr) == 0;
        return 1;
    }

    static int observe_owner(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &watch = *static_cast<Watch *>(data);
        const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
        if (!watch.lost && matches_sender(message, bus_name) &&
            sd_bus_message_has_signature(message, "sss") > 0 &&
            sd_bus_message_read(message, "sss", &name, &old_owner, &new_owner) > 0 &&
            std::string_view(name) == watch.name && watch.owner == old_owner &&
            watch.owner != new_owner) {
            watch.lost = true;
            watch.probe->schedule_faults();
        }
        return 0;
    }

    void confirm_owners() {
        auto *connection = bus.native_handle();
        if (!connection) {
            finish(transport_error(bus.last_error()));
            return;
        }
        for (auto &watch : watches) {
            const auto rule =
                std::string("type='signal',sender='org.freedesktop.DBus',"
                            "interface='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                            "member='NameOwnerChanged',arg0='") +
                watch.name + '\'';
            const auto result = sd_bus_add_match_async(connection, &watch.slot, rule.c_str(),
                                                       observe_owner, watch_installed, &watch);
            if (result < 0) {
                finish({ServiceFailure::internal_error, result});
                return;
            }
        }
        // This call is queued after both AddMatch requests and updates the
        // transport's event interests. Revalidate both identities to bound the
        // gap from the initial replies until the matches were installed.
        advance(Step::frontend_owner);
    }

    static int observe_bus(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &self = *static_cast<Impl *>(data);
        constexpr char local[] = "org.freedesktop.DBus.Local";
        if (self.monitoring && matches_sender(message, local) &&
            sd_bus_message_is_signal(message, local, "Disconnected") > 0) {
            for (auto &watch : self.watches) {
                watch.lost = true;
            }
            self.schedule_faults();
            return 0;
        }
        if (self.step != Step::create && self.step != Step::response) {
            return 0;
        }
        const auto *path = sd_bus_message_get_path(message);
        if (!path || self.request_path != path || !matches_sender(message, self.frontend_owner) ||
            sd_bus_message_is_signal(message, request_interface, "Response") <= 0) {
            return 0;
        }
        if (!self.response) {
            self.response = self.read_response(message);
        }
        return 1;
    }

    void start_create() {
        auto *connection = bus.native_handle();
        const char *unique = nullptr;
        const int result = connection ? sd_bus_get_unique_name(connection, &unique) : -ENOTCONN;
        if (result < 0 || !is_unique_name(unique)) {
            finish({ServiceFailure::disconnected, result < 0 ? result : -ENOTCONN});
            return;
        }
        static std::atomic<std::uint64_t> sequence{};
        token = "seaside" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        std::string sender(unique + 1);
        std::replace(sender.begin(), sender.end(), '.', '_');
        request_path = std::string(desktop_path) + "/request/" + sender + '/' + token;
        session_path = std::string(desktop_path) + "/session/" + sender + '/' + token;
        // The frontend directs Response to its caller. A local filter accepts
        // only this request path and the frontend's pinned unique owner, even
        // if the signal reaches sd-bus before the method callback is delivered.
        const auto filter = sd_bus_add_filter(connection, &response_filter, observe_bus, this);
        if (filter < 0) {
            finish({ServiceFailure::internal_error, filter});
            return;
        }
        advance(Step::create);
    }

    void wait_tick() {
        if (bus.last_error() != BusError::none) {
            finish(transport_error(bus.last_error()));
            return;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            finish(expired);
        } else if (step == Step::binding && binding(backend_pid, expected_uid)) {
            if (!abandoned) {
                start_create();
            }
        } else if (step == Step::binding && now >= binding_deadline) {
            finish(wrong_display);
        } else if (step == Step::response && response) {
            if (response->failure == ServiceFailure::none) {
                advance(Step::prove_route);
            } else {
                finish(*response);
            }
        } else if (!abandoned) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
            const auto interval = std::min(25ms, remaining).count();
            if (wl_event_source_timer_update(timer, static_cast<int>(interval)) < 0) {
                finish({ServiceFailure::internal_error, -errno});
            }
        }
    }

    static int on_timer(void *data) noexcept {
        auto self = static_cast<Impl *>(data)->weak_from_this().lock();
        if (!self) {
            return 0;
        }
        self->ticking = true;
        try {
            if (self->monitoring) {
                self->deliver_faults();
            } else {
                self->wait_tick();
            }
        } catch (...) {
            self->finish({ServiceFailure::internal_error, -ENOMEM});
        }
        self->ticking = false;
        if (!self->completion) {
            self->stop();
        }
        self->schedule_faults();
        return 0;
    }

    void wait_for(Step next) {
        step = next;
        if (!timer) {
            timer = wl_event_loop_add_timer(bus.event_loop(), on_timer, this);
        }
        if (!timer) {
            finish({ServiceFailure::internal_error, -errno});
            return;
        }
        wait_tick();
    }

    void receive(const BusReply &reply) noexcept {
        pending.reset();
        if (Clock::now() >= deadline) {
            finish(expired);
            return;
        }
        if (reply.error != BusError::none) {
            finish(protocol_error(reply, step));
            return;
        }
        if (!reply.message) {
            finish(malformed);
            return;
        }
        try {
            receive_message(reply.message);
        } catch (...) {
            finish({ServiceFailure::internal_error, -ENOMEM});
        }
    }

    void receive_message(sd_bus_message *message) {
        switch (step) {
        case Step::frontend_cast:
        case Step::backend_cast: {
            const auto *sender = sd_bus_message_get_sender(message);
            if (!is_unique_name(sender) || !read_screencast(message)) {
                break;
            }
            if (step == Step::frontend_cast) {
                frontend_owner = sender;
                advance(Step::frontend_shot);
            } else {
                backend_owner = sender;
                advance(Step::backend_shot);
            }
            return;
        }
        case Step::frontend_shot:
        case Step::backend_shot: {
            const bool frontend = step == Step::frontend_shot;
            if (!matches_sender(message, frontend ? frontend_owner : backend_owner) ||
                !read_screenshot_version(message)) {
                break;
            }
            advance(frontend ? Step::backend_cast : Step::credentials);
            return;
        }
        case Step::credentials: {
            const auto result = read_backend_credentials(message);
            if (result.failure != ServiceFailure::none) {
                finish(result);
                return;
            }
            // xdpw discovers the globals before claiming its bus name, but
            // Wayland may still be dispatching the queued bind requests.
            binding_deadline = Clock::now() + 500ms;
            wait_for(Step::binding);
            return;
        }
        case Step::create: {
            const char *handle = nullptr;
            if (!matches_sender(message, frontend_owner) ||
                sd_bus_message_has_signature(message, "o") <= 0 ||
                sd_bus_message_read(message, "o", &handle) <= 0 || request_path != handle) {
                break;
            }
            wait_for(Step::response);
            return;
        }
        case Step::prove_route: {
            const char *xml = nullptr;
            if (!matches_sender(message, backend_owner) ||
                sd_bus_message_has_signature(message, "s") <= 0 ||
                sd_bus_message_read(message, "s", &xml) <= 0 ||
                !std::string_view(xml).contains(impl_session_xml)) {
                break;
            }
            advance(Step::close);
            return;
        }
        case Step::close:
            if (matches_sender(message, frontend_owner) &&
                sd_bus_message_has_signature(message, "") > 0) {
                create_sent = false;
                const bool still_bound = binding(backend_pid, expected_uid);
                if (!still_bound) {
                    finish(wrong_display);
                } else if (!abandoned && runtime_fault) {
                    confirm_owners();
                } else {
                    finish({});
                }
                return;
            }
            break;
        case Step::frontend_owner:
        case Step::backend_owner: {
            const char *owner = nullptr;
            if (!matches_sender(message, bus_name) ||
                sd_bus_message_has_signature(message, "s") <= 0 ||
                sd_bus_message_read(message, "s", &owner) <= 0) {
                break;
            }
            const bool frontend = step == Step::frontend_owner;
            const auto &watch = watches[frontend ? 0 : 1];
            if (!watch.installed) {
                finish({ServiceFailure::internal_error, -EIO});
            } else if (watch.lost || watch.owner != owner) {
                finish({ServiceFailure::disconnected, -ENOTCONN});
            } else if (frontend) {
                advance(Step::backend_owner);
            } else if (watches[0].lost) {
                step = Step::frontend_owner;
                finish({ServiceFailure::disconnected, -ENOTCONN});
            } else {
                monitoring = true;
                finish({});
            }
            return;
        }
        case Step::binding:
        case Step::response:
            break;
        }
        finish(malformed);
    }

    void abandon() noexcept {
        abandoned = true;
        monitoring = false;
        completion = {};
        runtime_fault = {};
        stop();
    }
};

PortalProbe::PortalProbe(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

std::expected<std::unique_ptr<PortalProbe>, ServiceReport>
PortalProbe::start(SessionBus &bus, uid_t expected_uid, BindingCheck bound, Completion completion,
                   std::chrono::milliseconds timeout, Completion runtime_fault) noexcept {
    if (!bound || !completion || expected_uid == static_cast<uid_t>(-1) || timeout <= 0ms ||
        timeout > 60s) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -EINVAL});
    }
    try {
        auto impl =
            std::make_shared<Impl>(bus, expected_uid, std::move(bound), std::move(completion),
                                   Clock::now() + timeout, std::move(runtime_fault));
        auto probe = std::unique_ptr<PortalProbe>(new PortalProbe(impl));
        const auto result = impl->request(Step::frontend_cast);
        if (result.failure != ServiceFailure::none) {
            return std::unexpected(result);
        }
        return probe;
    } catch (...) {
        return std::unexpected(ServiceReport{ServiceFailure::internal_error, -ENOMEM});
    }
}

PortalProbe::~PortalProbe() noexcept {
    auto impl = impl_;
    impl->abandon();
}
} // namespace seaside

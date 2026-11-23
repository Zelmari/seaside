#include "session_preferences.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <systemd/sd-bus.h>

namespace seaside {
namespace {
using namespace std::chrono_literals;
using Message = std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)>;

constexpr char appearance_namespace[] = "org.freedesktop.appearance";
constexpr char color_key[] = "color-scheme";
constexpr char settings_path[] = "/org/freedesktop/portal/desktop";
constexpr char settings_interface[] = "org.freedesktop.impl.portal.Settings";
constexpr char settings_service[] = "org.freedesktop.impl.portal.desktop.seaside";
constexpr char gtk_settings_service[] = "org.freedesktop.impl.portal.desktop.gtk";
constexpr char power_service[] = "org.freedesktop.UPower.PowerProfiles";
constexpr char power_path[] = "/org/freedesktop/UPower/PowerProfiles";

Message method(sd_bus *bus, const char *destination, const char *path, const char *interface,
               const char *name) noexcept {
    sd_bus_message *raw = nullptr;
    if (bus) {
        sd_bus_message_new_method_call(bus, &raw, destination, path, interface, name);
    }
    return Message(raw, sd_bus_message_unref);
}

bool namespace_matches(std::string_view pattern) noexcept {
    const std::string_view name(appearance_namespace);
    if (pattern.empty() || pattern == name) {
        return true;
    }
    if (pattern.ends_with(".*")) {
        pattern.remove_suffix(1);
        return name.starts_with(pattern);
    }
    return false;
}

unsigned color_value(ColorScheme scheme) noexcept {
    return scheme == ColorScheme::dark ? 1 : 2;
}

int setting_not_found(sd_bus_message *request) noexcept {
    return sd_bus_reply_method_errorf(request, "org.freedesktop.portal.Error.NotFound",
                                      "Requested setting not found");
}

template <typename Reply>
int forward_setting(SessionBus &bus, sd_bus_message *request, Reply respond) {
    auto call = method(bus.native_handle(), gtk_settings_service, settings_path,
                       settings_interface, sd_bus_message_get_member(request));
    if (!call || sd_bus_message_rewind(request, true) < 0 ||
        sd_bus_message_copy(call.get(), request, true) < 0) {
        return respond(request, BusReply{});
    }
    auto incoming = std::shared_ptr<sd_bus_message>(sd_bus_message_ref(request),
                                                     sd_bus_message_unref);
    if (!bus.call(call.get(), 3s, [incoming, respond](const BusReply &reply) {
            respond(incoming.get(), reply);
        })) {
        return respond(request, BusReply{});
    }
    return 1;
}

int copy_setting(sd_bus_message *request, sd_bus_message *source) noexcept {
    sd_bus_message *raw = nullptr;
    if (int code = sd_bus_message_new_method_return(request, &raw); code < 0) {
        return code;
    }
    Message reply(raw, sd_bus_message_unref);
    if (int code = sd_bus_message_copy(reply.get(), source, true); code < 0) {
        return code;
    }
    return sd_bus_send(nullptr, reply.get(), nullptr);
}

bool append_settings_namespace(sd_bus_message *reply, sd_bus_message *source, const char *name,
                               std::optional<unsigned> color) noexcept {
    if (sd_bus_message_open_container(reply, 'e', "sa{sv}") < 0 ||
        sd_bus_message_append(reply, "s", name) < 0) {
        return false;
    }
    if (!color) {
        return source && sd_bus_message_copy(reply, source, false) > 0 &&
               sd_bus_message_close_container(reply) >= 0;
    }

    if (sd_bus_message_open_container(reply, 'a', "{sv}") < 0 ||
        sd_bus_message_append(reply, "{sv}", color_key, "u", *color) < 0) {
        return false;
    }
    if (source) {
        if (sd_bus_message_enter_container(source, 'a', "{sv}") <= 0) {
            return false;
        }
        int entry;
        while ((entry = sd_bus_message_enter_container(source, 'e', "sv")) > 0) {
            const char *key = nullptr;
            if (sd_bus_message_read(source, "s", &key) <= 0) {
                return false;
            }
            if (key == std::string_view(color_key)) {
                if (sd_bus_message_skip(source, "v") < 0) {
                    return false;
                }
            } else if (sd_bus_message_open_container(reply, 'e', "sv") < 0 ||
                       sd_bus_message_append(reply, "s", key) < 0 ||
                       sd_bus_message_copy(reply, source, false) <= 0 ||
                       sd_bus_message_close_container(reply) < 0) {
                return false;
            }
            if (sd_bus_message_exit_container(source) < 0) {
                return false;
            }
        }
        if (entry < 0 || sd_bus_message_exit_container(source) < 0) {
            return false;
        }
    }
    return sd_bus_message_close_container(reply) >= 0 &&
           sd_bus_message_close_container(reply) >= 0;
}

int merge_settings(sd_bus_message *request, sd_bus_message *source,
                   std::optional<unsigned> color) noexcept {
    sd_bus_message *raw = nullptr;
    if (int code = sd_bus_message_new_method_return(request, &raw); code < 0) {
        return code;
    }
    Message reply(raw, sd_bus_message_unref);
    if (int code = sd_bus_message_open_container(raw, 'a', "{sa{sv}}"); code < 0) {
        return code;
    }
    if (source && sd_bus_message_enter_container(source, 'a', "{sa{sv}}") > 0) {
        int entry;
        while ((entry = sd_bus_message_enter_container(source, 'e', "sa{sv}")) > 0) {
            const char *name = nullptr;
            if (sd_bus_message_read(source, "s", &name) <= 0) {
                return -EBADMSG;
            }
            const auto override = name == std::string_view(appearance_namespace) ? color
                                                                                   : std::nullopt;
            if (!append_settings_namespace(raw, source, name, override) ||
                sd_bus_message_exit_container(source) < 0) {
                return -EBADMSG;
            }
            if (override) {
                color.reset();
            }
        }
        if (entry < 0 || sd_bus_message_exit_container(source) < 0) {
            return -EBADMSG;
        }
    }
    if (color && !append_settings_namespace(raw, nullptr, appearance_namespace, color)) {
        return -ENOMEM;
    }
    if (int code = sd_bus_message_close_container(raw); code < 0) {
        return code;
    }
    return sd_bus_send(nullptr, raw, nullptr);
}

// Return the policy name used in Seaside configuration and IPC.
std::string_view common_profile_name(std::string_view name) noexcept {
    return name == "power-saver" ? "saver" : name;
}

const char *daemon_profile_name(PowerProfile profile) noexcept {
    return profile == PowerProfile::saver ? "power-saver" : power_profile_name(profile);
}

bool parse_profiles(sd_bus_message *message, std::vector<std::string> &profiles) {
    if (sd_bus_message_enter_container(message, 'v', "aa{sv}") <= 0 ||
        sd_bus_message_enter_container(message, 'a', "a{sv}") <= 0) {
        return false;
    }
    int row;
    while ((row = sd_bus_message_enter_container(message, 'a', "{sv}")) > 0) {
        int field;
        while ((field = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
            const char *key = nullptr;
            const char *value = nullptr;
            if (sd_bus_message_read(message, "s", &key) <= 0) {
                return false;
            }
            if (std::string_view(key) == "Profile") {
                if (sd_bus_message_read(message, "v", "s", &value) <= 0) {
                    return false;
                }
                profiles.emplace_back(common_profile_name(value));
            } else if (sd_bus_message_skip(message, "v") < 0) {
                return false;
            }
            if (sd_bus_message_exit_container(message) < 0) {
                return false;
            }
        }
        if (field < 0 || sd_bus_message_exit_container(message) < 0) {
            return false;
        }
    }
    return row == 0 && sd_bus_message_exit_container(message) >= 0 &&
           sd_bus_message_exit_container(message) >= 0;
}

bool parse_power_state(sd_bus_message *message, PowerProfiles::Status &status) {
    if (!message || sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) {
        return false;
    }
    int entry;
    while ((entry = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
        const char *key = nullptr;
        if (sd_bus_message_read(message, "s", &key) <= 0) {
            return false;
        }
        const std::string_view field(key);
        if (field == "ActiveProfile") {
            const char *value = nullptr;
            if (sd_bus_message_read(message, "v", "s", &value) <= 0) {
                return false;
            }
            status.effective = std::string(common_profile_name(value));
        } else if (field == "Profiles") {
            if (!parse_profiles(message, status.available)) {
                return false;
            }
        } else if (sd_bus_message_skip(message, "v") < 0) {
            return false;
        }
        if (sd_bus_message_exit_container(message) < 0) {
            return false;
        }
    }
    return entry == 0 && sd_bus_message_exit_container(message) >= 0 && status.effective &&
           !status.available.empty();
}
} // namespace

std::unique_ptr<SettingsPortal> SettingsPortal::start(SessionBus &bus, ColorScheme scheme) noexcept
    try {
    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0), SD_BUS_METHOD("Read", "ss", "v", read, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("ReadAll", "as", "a{sa{sv}}", read_all, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("version", "u",
                        [](sd_bus *, const char *, const char *, const char *,
                           sd_bus_message *reply, void *, sd_bus_error *) {
                            return sd_bus_message_append(reply, "u", 1U);
                        },
                        0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_SIGNAL("SettingChanged", "ssv", 0), SD_BUS_VTABLE_END};
    auto portal = std::unique_ptr<SettingsPortal>(new SettingsPortal(bus, scheme));
    if (sd_bus_add_object_vtable(bus.native_handle(), &portal->object_, settings_path,
                                 settings_interface, vtable, portal.get()) < 0 ||
        sd_bus_match_signal_async(
            bus.native_handle(), &portal->gtk_changes_, gtk_settings_service, settings_path,
            settings_interface, "SettingChanged", relay_changed,
            [](sd_bus_message *reply, void *, sd_bus_error *) {
                if (sd_bus_message_is_method_error(reply, nullptr) > 0) {
                    std::fputs("seaside: could not subscribe to GTK settings changes\n", stderr);
                }
                return 0;
            },
            portal.get()) < 0) {
        return {};
    }
    auto request = method(bus.native_handle(), "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "RequestName");
    constexpr unsigned do_not_queue = 4;
    if (!request ||
        sd_bus_message_append(request.get(), "su", settings_service, do_not_queue) < 0) {
        return {};
    }
    auto *self = portal.get();
    auto pending = bus.call(request.get(), 3s, [self](const BusReply &reply) {
        self->name_call_.reset();
        unsigned result = 0;
        self->owns_name_ = reply.error == BusError::none &&
                           sd_bus_message_read(reply.message, "u", &result) > 0 &&
                           (result == 1 || result == 4);
        if (!self->owns_name_) {
            std::fputs("seaside: could not publish the session appearance preference\n", stderr);
        }
    });
    if (!pending) {
        return {};
    }
    portal->name_call_ = *pending;
    return portal;
} catch (...) {
    return {};
}

SettingsPortal::~SettingsPortal() {
    if (name_call_) {
        (void)bus_.cancel(*name_call_);
    }
    sd_bus_slot_unref(gtk_changes_);
    sd_bus_slot_unref(object_);
    if (auto *native = bus_.native_handle(); native && (owns_name_ || name_call_)) {
        sd_bus_release_name_async(native, nullptr, settings_service, nullptr, nullptr);
        bus_.refresh_events();
    }
}

void SettingsPortal::set(ColorScheme scheme) noexcept {
    if (*scheme_ == scheme) {
        return;
    }
    *scheme_ = scheme;
    if (owns_name_ && bus_.native_handle()) {
        sd_bus_emit_signal(bus_.native_handle(), settings_path, settings_interface,
                           "SettingChanged", "ssv", appearance_namespace, color_key, "u",
                           color_value(scheme));
        bus_.refresh_events();
    }
}

int SettingsPortal::read(sd_bus_message *message, void *data, sd_bus_error *) noexcept try {
    auto &self = *static_cast<SettingsPortal *>(data);
    const char *name = nullptr;
    const char *key = nullptr;
    if (const auto result = sd_bus_message_read(message, "ss", &name, &key); result < 0) {
        return result;
    }
    if (name && key && std::string_view(name) == appearance_namespace &&
        std::string_view(key) == color_key) {
        return sd_bus_reply_method_return(message, "v", "u", color_value(*self.scheme_));
    }
    return forward_setting(self.bus_, message, [](sd_bus_message *request, const BusReply &reply) {
        if (reply.error == BusError::none && reply.message &&
            sd_bus_message_has_signature(reply.message, "v") > 0) {
            if (copy_setting(request, reply.message) >= 0) {
                return 1;
            }
        }
        return setting_not_found(request);
    });
} catch (...) {
    return -ENOMEM;
}

int SettingsPortal::read_all(sd_bus_message *message, void *data, sd_bus_error *) noexcept try {
    auto &self = *static_cast<SettingsPortal *>(data);
    auto code = sd_bus_message_enter_container(message, 'a', "s");
    if (code <= 0) {
        return code < 0 ? code : -EINVAL;
    }
    bool matches = false;
    bool have_namespace = false;
    const char *pattern = nullptr;
    while ((code = sd_bus_message_read(message, "s", &pattern)) > 0) {
        have_namespace = true;
        matches |= namespace_matches(pattern);
    }
    if (code < 0 || (code = sd_bus_message_exit_container(message)) < 0) {
        return code;
    }
    return forward_setting(
        self.bus_, message,
        [scheme = std::weak_ptr(self.scheme_), include_color = !have_namespace || matches](
            sd_bus_message *request, const BusReply &reply) {
            const auto current = scheme.lock();
            if (!current) {
                return setting_not_found(request);
            }
            const auto color = include_color ? std::optional(color_value(*current)) : std::nullopt;
            auto *source = reply.error == BusError::none ? reply.message : nullptr;
            if (source && sd_bus_message_has_signature(source, "a{sa{sv}}") > 0 &&
                merge_settings(request, source, color) >= 0) {
                return 1;
            }
            return merge_settings(request, nullptr, color);
        });
} catch (...) {
    return -ENOMEM;
}

int SettingsPortal::relay_changed(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
    const auto &self = *static_cast<SettingsPortal *>(data);
    // The broker validates the well-known sender for broadcasts. Directed
    // signals reach the connection regardless of its broker match rules.
    if (!self.owns_name_ || sd_bus_message_get_destination(message) ||
        sd_bus_message_has_signature(message, "ssv") <= 0) {
        return 0;
    }
    const char *name = nullptr;
    const char *key = nullptr;
    if (sd_bus_message_read(message, "ss", &name, &key) <= 0 ||
        (name == std::string_view(appearance_namespace) && key == std::string_view(color_key))) {
        return 0;
    }
    sd_bus_message *raw = nullptr;
    if (sd_bus_message_new_signal(self.bus_.native_handle(), &raw, settings_path,
                                  settings_interface, "SettingChanged") >= 0) {
        Message relayed(raw, sd_bus_message_unref);
        if (sd_bus_message_rewind(message, true) >= 0 &&
            sd_bus_message_copy(raw, message, true) > 0) {
            sd_bus_send(nullptr, raw, nullptr);
        }
    }
    return 0;
}

const char *power_profile_name(PowerProfile profile) noexcept {
    switch (profile) {
    case PowerProfile::saver:
        return "saver";
    case PowerProfile::balanced:
        return "balanced";
    case PowerProfile::performance:
        return "performance";
    }
    return "balanced";
}

void PowerProfiles::configure(std::optional<PowerProfile> profile) noexcept {
    if (profile == requested_) {
        return;
    }
    if (pending_) {
        (void)bus_->cancel(*pending_);
        pending_.reset();
    }
    requested_ = profile;
    profile_pending_ = profile.has_value();
    status_.state = profile_pending_ ? "checking" : "system";
    next_ = {};
}

void PowerProfiles::tick(bool active) noexcept {
    active_ = active;
    if (pending_ || Clock::now() < next_) {
        return;
    }
    if (bus_ && bus_->last_error() != BusError::none) {
        bus_.reset();
        profile_pending_ = requested_.has_value();
    }
    if (!bus_) {
        const auto *address = std::getenv("DBUS_SYSTEM_BUS_ADDRESS");
        auto bus =
            SessionBus::connect(loop_, address ? address : "unix:path=/run/dbus/system_bus_socket");
        if (!bus) {
            mark_unavailable();
            return;
        }
        bus_ = std::move(*bus);
    }
    query();
}

void PowerProfiles::stop() noexcept {
    active_ = false;
    pending_.reset();
    bus_.reset();
}

void PowerProfiles::query() noexcept try {
    auto request = method(bus_->native_handle(), power_service, power_path,
                          "org.freedesktop.DBus.Properties", "GetAll");
    if (!request || sd_bus_message_set_auto_start(request.get(), requested_.has_value()) < 0 ||
        sd_bus_message_append(request.get(), "s", power_service) < 0) {
        mark_unavailable();
        return;
    }
    const auto call = bus_->call(request.get(), 2s, [this](const BusReply &reply) {
        pending_.reset();
        read_state(reply);
    });
    if (call) {
        pending_ = *call;
    } else {
        mark_unavailable();
    }
} catch (...) {
    mark_unavailable();
}

void PowerProfiles::read_state(const BusReply &reply) noexcept try {
    Status observed;
    if (reply.error != BusError::none || !parse_power_state(reply.message, observed)) {
        mark_unavailable();
        return;
    }
    observed.state = status_.state;
    status_ = std::move(observed);
    next_ = Clock::now() + 3s;
    if (!requested_) {
        status_.state = "system";
        return;
    }
    const char *desired = power_profile_name(*requested_);
    if (std::ranges::find(status_.available, desired) == status_.available.end()) {
        status_.state = "unsupported";
        profile_pending_ = true;
    } else if (status_.effective == desired) {
        status_.state = "applied";
        profile_pending_ = false;
    } else if (profile_pending_ && active_) {
        select();
    } else if (std::string_view(status_.state) == "applied") {
        status_.state = "changed-externally";
    }
} catch (...) {
    mark_unavailable();
}

void PowerProfiles::select() noexcept try {
    auto request = method(bus_->native_handle(), power_service, power_path,
                          "org.freedesktop.DBus.Properties", "Set");
    if (!request || sd_bus_message_append(request.get(), "ssv", power_service, "ActiveProfile", "s",
                                          daemon_profile_name(*requested_)) < 0) {
        mark_unavailable();
        return;
    }
    const auto call = bus_->call(request.get(), 2s, [this](const BusReply &reply) {
        pending_.reset();
        if (reply.error == BusError::none) {
            profile_pending_ = false;
            next_ = {};
        } else if (reply.native_error == -EPERM || reply.native_error == -EACCES) {
            profile_pending_ = false;
            status_.state = "denied";
        } else {
            mark_unavailable();
        }
    });
    if (call) {
        pending_ = *call;
    } else {
        mark_unavailable();
    }
} catch (...) {
    mark_unavailable();
}

void PowerProfiles::mark_unavailable() noexcept {
    status_ = {};
    status_.state = "unavailable";
    profile_pending_ = requested_.has_value();
    next_ = Clock::now() + 3s;
}
} // namespace seaside

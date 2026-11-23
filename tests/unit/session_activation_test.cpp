#include "session_activation.hpp"
#include "session_bus.hpp"
#include "test_dbus_daemon.hpp"
#include "test_directory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using seaside::PortalActivation;
using seaside::ServiceFailure;
using seaside::ServiceReport;
using seaside::SessionBus;
using seaside::StartupService;

constexpr char manager_name[] = "org.freedesktop.systemd1";
constexpr char manager_interface[] = "org.freedesktop.systemd1.Manager";
constexpr char manager_path[] = "/org/freedesktop/systemd1";
constexpr char unit_prefix[] = "/org/freedesktop/systemd1/unit";

struct Portal {
    const char *suffix;
    const char *name;
    const char *binary;
};
constexpr std::array portals{
    Portal{"-wlr.service", "org.freedesktop.impl.portal.desktop.wlr",
           "/usr/lib/xdg-desktop-portal-wlr"},
    Portal{"-gtk.service", "org.freedesktop.impl.portal.desktop.gtk",
           "/usr/lib/xdg-desktop-portal-gtk"},
    Portal{"-frontend.service", "org.freedesktop.portal.Desktop", "/usr/lib/xdg-desktop-portal"},
};

int portal_binary_errno = 0;
unsigned portal_binary_checks = 0;
} // namespace

extern "C" int __real_access(const char *path, int mode);
extern "C" int __wrap_access(const char *path, int mode) {
    for (const auto &portal : portals) {
        if (std::string_view(path) == portal.binary && mode == X_OK) {
            ++portal_binary_checks;
            if (portal_binary_errno == 0) {
                return 0;
            }
            errno = portal_binary_errno;
            return -1;
        }
    }
    return __real_access(path, mode);
}

namespace {
class PortalActivationTest : public testing::Test {
  protected:
    using EventLoop = std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)>;
    using BusSlot = std::unique_ptr<sd_bus_slot, decltype(&sd_bus_slot_unref)>;

    struct RunningUnit {
        std::string name;
        std::string path;
        const Portal *portal;
        bool stopping = false;
        unsigned stop_polls = 0;
    };
    enum class StopBehavior { normal, reply_error, no_reply, unknown_state, stays_active };

    seaside::test::Directory directory;
    const std::string address = "unix:path=" + (directory.path() / "bus").string();
    seaside::test::PrivateDaemon daemon{address};
    EventLoop loop{wl_event_loop_create(), wl_event_loop_destroy};
    std::unique_ptr<SessionBus> client, manager, external;
    BusSlot manager_filter{nullptr, sd_bus_slot_unref};
    std::unique_ptr<PortalActivation> activation;
    std::optional<std::pair<StartupService, ServiceReport>> outcome;
    std::vector<RunningUnit> units;
    std::string lifetime;
    std::vector<std::string> started;
    std::vector<std::string> stopped;
    bool foreign_unit = false;
    bool next_portal_claims_on_launch = false;
    unsigned get_unit_by_pid = 0;
    StopBehavior stop_behavior = StopBehavior::normal;

    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3.2-portal-activation-ownership");
        portal_binary_errno = 0;
        portal_binary_checks = 0;
        ASSERT_TRUE(loop);
        ASSERT_TRUE(connect(client));
        ASSERT_TRUE(connect(manager));
        ASSERT_TRUE(claim(*manager, manager_name));
        sd_bus_slot *slot = nullptr;
        ASSERT_GE(sd_bus_add_filter(manager->native_handle(), &slot, accept_manager_call, this), 0);
        manager_filter.reset(slot);
    }

    void TearDown() override { activation.reset(); }

    bool pump_until(const std::function<bool()> &ready) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                wl_event_loop_dispatch(loop.get(), 10) < 0) {
                return false;
            }
        }
        return true;
    }

    bool connect(std::unique_ptr<SessionBus> &target) {
        auto connected = SessionBus::connect(loop.get(), address);
        if (!connected) {
            return false;
        }
        target = std::move(*connected);
        return pump_until([&] { return target->ready(); });
    }

    static int on_claim(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        std::uint32_t result = 0;
        *static_cast<bool *>(data) = sd_bus_message_read(message, "u", &result) > 0 && result == 1;
        return 1;
    }

    bool claim(SessionBus &bus, const char *name) {
        sd_bus_slot *raw = nullptr;
        bool owned = false;
        const int result =
            sd_bus_request_name_async(bus.native_handle(), &raw, name, 0, on_claim, &owned);
        BusSlot slot(raw, sd_bus_slot_unref);
        return result >= 0 && pump_until([&] { return owned; });
    }

    void start() {
        auto result = PortalActivation::start(
            *client, "wayland-seaside-activation-test",
            [this](StartupService service, ServiceReport report) {
                EXPECT_FALSE(outcome);
                outcome.emplace(service, report);
            },
            2s);
        ASSERT_TRUE(result);
        activation = std::move(*result);
        ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    }

    void recover() {
        outcome.reset();
        get_unit_by_pid = 0;
        ASSERT_TRUE(activation->recover(
            *client,
            [this](StartupService service, ServiceReport report) {
                EXPECT_FALSE(outcome);
                outcome.emplace(service, report);
            },
            2s));
        ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    }

    bool release(const Portal &portal) {
        sd_bus_slot *raw = nullptr;
        bool released = false;
        const int result = sd_bus_release_name_async(manager->native_handle(), &raw, portal.name,
                                                     on_claim, &released);
        BusSlot slot(raw, sd_bus_slot_unref);
        return result >= 0 && pump_until([&] { return released; });
    }

    static int accept_manager_call(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        try {
            return static_cast<PortalActivationTest *>(data)->handle_manager_call(message);
        } catch (...) {
            return -ENOMEM;
        }
    }

    int handle_manager_call(sd_bus_message *message) {
        if (sd_bus_message_is_method_call(message, manager_interface, "StartTransientUnit") > 0) {
            return start_unit(message);
        }
        if (sd_bus_message_is_method_call(message, manager_interface, "GetUnitByPID") > 0) {
            const auto &portal = portals[get_unit_by_pid++ % portals.size()];
            std::uint32_t pid = 0;
            const auto unit = std::ranges::find_if(
                units, [&](const RunningUnit &candidate) { return candidate.portal == &portal; });
            if (sd_bus_message_read(message, "u", &pid) <= 0 || unit == units.end()) {
                return -EINVAL;
            }
            EXPECT_EQ(pid, static_cast<std::uint32_t>(getpid()));
            return sd_bus_reply_method_return(message, "o",
                                              foreign_unit ? "/foreign/unit" : unit->path.c_str());
        }
        if (sd_bus_message_is_method_call(message, manager_interface, "StopUnit") > 0) {
            return stop_unit(message);
        }
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Get") > 0) {
            return read_unit_state(message);
        }
        return 0;
    }

    static bool read_environment(sd_bus_message *message) {
        if (sd_bus_message_enter_container(message, 'v', "as") <= 0 ||
            sd_bus_message_enter_container(message, 'a', "s") <= 0) {
            return false;
        }
        std::vector<std::string> values;
        const char *value = nullptr;
        int read = 0;
        while ((read = sd_bus_message_read(message, "s", &value)) > 0) {
            values.emplace_back(value);
        }
        if (read < 0 || sd_bus_message_exit_container(message) < 0 ||
            sd_bus_message_exit_container(message) < 0) {
            return false;
        }
        EXPECT_NE(std::ranges::find(values, "WAYLAND_DISPLAY=wayland-seaside-activation-test"),
                  values.end());
        EXPECT_NE(std::ranges::find(values, "XDG_CURRENT_DESKTOP=Seaside"), values.end());
        EXPECT_NE(std::ranges::find(values, "XDG_SESSION_TYPE=wayland"), values.end());
        EXPECT_NE(std::ranges::find(values, "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" +
                                                std::to_string(getuid()) + "/bus"),
                  values.end());
        return true;
    }

    int start_unit(sd_bus_message *message) {
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        EXPECT_EQ(sd_bus_message_get_allow_interactive_authorization(message), 0);
        EXPECT_STREQ(sd_bus_message_get_path(message), manager_path);
        const char *name = nullptr;
        const char *mode = nullptr;
        if (sd_bus_message_read(message, "ss", &name, &mode) <= 0 ||
            sd_bus_message_enter_container(message, 'a', "(sv)") <= 0) {
            return -EINVAL;
        }
        EXPECT_STREQ(mode, "fail");
        if (std::string_view(name).starts_with("seaside-session-")) {
            lifetime = name;
            return sd_bus_reply_method_return(message, "o", "/org/freedesktop/systemd1/job/1");
        }
        EXPECT_TRUE(std::string_view(name).starts_with("seaside-portal-"));
        const auto chosen = std::ranges::find_if(portals, [&](const Portal &portal) {
            return std::string_view(name).ends_with(portal.suffix);
        });
        if (chosen == portals.end()) {
            return -EINVAL;
        }
        bool environment = false;
        bool graphical_wants = false, graphical_after = false, bound_to_session = false;
        int entry = 0;
        while ((entry = sd_bus_message_enter_container(message, 'r', "sv")) > 0) {
            const char *key = nullptr;
            if (sd_bus_message_read(message, "s", &key) <= 0) {
                return -EINVAL;
            }
            if (std::string_view(key) == "Environment") {
                environment = read_environment(message);
                if (!environment) {
                    return -EINVAL;
                }
            } else if (std::string_view(key) == "Wants" || std::string_view(key) == "After" ||
                       std::string_view(key) == "BindsTo") {
                const char *target = nullptr;
                if (sd_bus_message_enter_container(message, 'v', "as") <= 0 ||
                    sd_bus_message_enter_container(message, 'a', "s") <= 0 ||
                    sd_bus_message_read(message, "s", &target) <= 0) {
                    return -EINVAL;
                }
                if (std::string_view(key) == "BindsTo") {
                    EXPECT_EQ(target, lifetime);
                    bound_to_session = true;
                } else {
                    EXPECT_STREQ(target, "graphical-session.target");
                    if (std::string_view(key) == "After") {
                        if (sd_bus_message_read(message, "s", &target) <= 0) {
                            return -EINVAL;
                        }
                        EXPECT_EQ(target, lifetime);
                        graphical_after = true;
                    } else {
                        graphical_wants = true;
                    }
                }
                if (sd_bus_message_exit_container(message) < 0 ||
                    sd_bus_message_exit_container(message) < 0) {
                    return -EINVAL;
                }
            } else if (sd_bus_message_skip(message, "v") < 0) {
                return -EINVAL;
            }
            if (sd_bus_message_exit_container(message) < 0) {
                return -EINVAL;
            }
        }
        EXPECT_TRUE(environment);
        EXPECT_TRUE(bound_to_session);
        EXPECT_TRUE(graphical_wants);
        EXPECT_TRUE(graphical_after);
        if (entry < 0 || sd_bus_message_exit_container(message) < 0) {
            return -EINVAL;
        }

        char *raw_path = nullptr;
        const int encoded = sd_bus_path_encode(unit_prefix, name, &raw_path);
        const std::unique_ptr<char, decltype(&std::free)> path(raw_path, std::free);
        if (encoded < 0) {
            return encoded;
        }
        started.emplace_back(name);
        if (std::ranges::none_of(units,
                                 [&](const RunningUnit &unit) { return unit.name == name; })) {
            units.push_back({name, path.get(), &*chosen});
        }
        if (next_portal_claims_on_launch && &*chosen == &portals.front()) {
            // Acquire the next name after the initial discovery pass and
            // before returning this launch. Its independent bus can reply
            // without reentering the fixture's Wayland event loop.
            const int claimed = sd_bus_request_name(external->native_handle(), portals[1].name, 0);
            if (claimed < 0) {
                return claimed;
            }
        }
        const int claimed = sd_bus_request_name_async(manager->native_handle(), nullptr,
                                                      chosen->name, 0, nullptr, nullptr);
        if (claimed < 0) {
            return claimed;
        }
        return sd_bus_reply_method_return(message, "o", "/org/freedesktop/systemd1/job/1");
    }

    int stop_unit(sd_bus_message *message) {
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        EXPECT_EQ(sd_bus_message_get_allow_interactive_authorization(message), 0);
        const char *name = nullptr;
        const char *mode = nullptr;
        if (sd_bus_message_read(message, "ss", &name, &mode) <= 0) {
            return -EINVAL;
        }
        EXPECT_STREQ(mode, "replace");
        stopped.emplace_back(name);
        auto found =
            std::ranges::find_if(units, [&](const RunningUnit &unit) { return unit.name == name; });
        if (found == units.end()) {
            ADD_FAILURE() << "Attempted to stop unowned unit " << name;
            return sd_bus_reply_method_errorf(message, "org.freedesktop.systemd1.NoSuchUnit",
                                              "test unit absent");
        }
        found->stopping = true;
        const int released = sd_bus_release_name_async(manager->native_handle(), nullptr,
                                                       found->portal->name, nullptr, nullptr);
        if (released < 0) {
            return released;
        }
        if (found->portal == &portals.back() && stop_behavior == StopBehavior::reply_error) {
            return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_FAILED, "test stop reply lost");
        }
        if (found->portal == &portals.back() && stop_behavior == StopBehavior::no_reply) {
            return 1;
        }
        return sd_bus_reply_method_return(message, "o", "/org/freedesktop/systemd1/job/2");
    }

    int read_unit_state(sd_bus_message *message) {
        const char *interface = nullptr;
        const char *property = nullptr;
        if (sd_bus_message_read(message, "ss", &interface, &property) <= 0) {
            return -EINVAL;
        }
        EXPECT_STREQ(interface, "org.freedesktop.systemd1.Unit");
        EXPECT_STREQ(property, "ActiveState");
        const auto *path = sd_bus_message_get_path(message);
        auto found =
            std::ranges::find_if(units, [&](const RunningUnit &unit) { return unit.path == path; });
        if (found == units.end()) {
            return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_UNKNOWN_OBJECT,
                                              "test unit already collected");
        }
        EXPECT_TRUE(found->stopping);
        const auto *state = found->stop_polls++ == 0 ? "deactivating" : "inactive";
        if (found->portal == &portals.back() && stop_behavior == StopBehavior::unknown_state) {
            return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_FAILED,
                                              "test unit state unavailable");
        }
        if (found->portal == &portals.front() && stop_behavior == StopBehavior::stays_active) {
            state = "active";
        }
        return sd_bus_reply_method_return(message, "v", "s", state);
    }
};

TEST_F(PortalActivationTest, RejectsAnExistingCaptureBackendWithoutStartingOrStoppingServices) {
    ASSERT_TRUE(connect(external));
    ASSERT_TRUE(claim(*external, portals[0].name));
    start();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->first, StartupService::capture_backend);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_EQ(outcome->second.native_error, -EADDRINUSE);
    EXPECT_TRUE(activation->stop());
    EXPECT_EQ(portal_binary_checks, 0U);
    EXPECT_TRUE(units.empty());
    EXPECT_TRUE(stopped.empty());
}

TEST_F(PortalActivationTest, RejectsAnExistingFrontendBeforeAcquiringTheGraphicalTarget) {
    ASSERT_TRUE(connect(external));
    ASSERT_TRUE(claim(*external, portals.back().name));
    start();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->first, StartupService::portal_frontend);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_TRUE(activation->stop());
    EXPECT_EQ(portal_binary_checks, 0U);
    EXPECT_TRUE(started.empty());
    EXPECT_TRUE(stopped.empty());
}

TEST_F(PortalActivationTest, RetryingForeignFrontendLeavesAllExternalUnitsAlone) {
    ASSERT_TRUE(connect(external));
    ASSERT_TRUE(claim(*external, portals.back().name));
    start();
    ASSERT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    recover();
    EXPECT_EQ(outcome->first, StartupService::portal_frontend);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_TRUE(started.empty());
    EXPECT_EQ(portal_binary_checks, 0U);
}

TEST_F(PortalActivationTest, DistinguishesMissingPortalBinaryFromAnInaccessibleBinary) {
    for (const auto &[error, expected] : {std::pair{ENOENT, ServiceFailure::missing},
                                          std::pair{EACCES, ServiceFailure::activation_failed}}) {
        SCOPED_TRACE(error);
        portal_binary_errno = error;
        outcome.reset();
        start();
        ASSERT_TRUE(outcome);
        EXPECT_EQ(outcome->first, StartupService::capture_backend);
        EXPECT_EQ(outcome->second.failure, expected);
        EXPECT_EQ(outcome->second.native_error, -error);
        EXPECT_TRUE(activation->stop());
        activation.reset();
    }
    EXPECT_EQ(portal_binary_checks, 2U);
    EXPECT_TRUE(units.empty());
    EXPECT_TRUE(stopped.empty());
}

TEST_F(PortalActivationTest, VerifiesAllOwnedPortalUnitsAndWaitsForThemToStop) {
    start();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::none);
    EXPECT_EQ(get_unit_by_pid, portals.size());
    ASSERT_EQ(units.size(), portals.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        EXPECT_EQ(units[index].portal, &portals[index]);
    }

    EXPECT_TRUE(activation->stop());
    ASSERT_EQ(stopped.size(), units.size());
    for (std::size_t index = 0; index < units.size(); ++index) {
        EXPECT_EQ(stopped[index], units[units.size() - 1 - index].name);
        EXPECT_GE(units[index].stop_polls, 2U);
    }
    EXPECT_TRUE(activation->stop());
    EXPECT_EQ(stopped.size(), units.size());
}

TEST_F(PortalActivationTest, ConfirmsEveryUnitStoppedDespiteAFailedStopReply) {
    start();
    ASSERT_TRUE(outcome);
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    stop_behavior = StopBehavior::reply_error;

    EXPECT_TRUE(activation->stop());
    ASSERT_EQ(stopped.size(), portals.size());
    for (const auto &unit : units) {
        EXPECT_GE(unit.stop_polls, 2U);
    }
}

TEST_F(PortalActivationTest, ConfirmsEveryUnitStoppedWithoutWaitingForAMissingReply) {
    start();
    ASSERT_TRUE(outcome);
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    stop_behavior = StopBehavior::no_reply;

    const auto started = std::chrono::steady_clock::now();
    EXPECT_TRUE(activation->stop());
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1250ms);
    ASSERT_EQ(stopped.size(), portals.size());
    for (const auto &unit : units) {
        EXPECT_GE(unit.stop_polls, 2U);
    }
}

TEST_F(PortalActivationTest, ReportsAnUnverifiableUnitDespiteASuccessfulStopReply) {
    start();
    ASSERT_TRUE(outcome);
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    stop_behavior = StopBehavior::unknown_state;

    EXPECT_FALSE(activation->stop());
    ASSERT_EQ(stopped.size(), portals.size());
    EXPECT_EQ(units.back().stop_polls, 1U);
}

TEST_F(PortalActivationTest, ReportsAUnitStillActiveAtTheShutdownDeadline) {
    start();
    ASSERT_TRUE(outcome);
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    stop_behavior = StopBehavior::stays_active;

    EXPECT_FALSE(activation->stop());
    ASSERT_EQ(stopped.size(), portals.size());
    for (const auto &unit : units) {
        EXPECT_GE(unit.stop_polls, 2U);
    }
}

TEST_F(PortalActivationTest, RollsBackOnlyItsOwnUnitWhenTheNextPortalClaimsItsName) {
    ASSERT_TRUE(connect(external));
    next_portal_claims_on_launch = true;
    start();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    ASSERT_EQ(units.size(), 1U);
    EXPECT_EQ(units.front().portal, &portals[0]);
    EXPECT_TRUE(activation->stop());
    ASSERT_EQ(stopped.size(), 1U);
    EXPECT_EQ(stopped.front(), units.front().name);
    EXPECT_GE(units.front().stop_polls, 2U);
}

TEST_F(PortalActivationTest, RejectsAClaimedNameWhoseOwnerIsOutsideTheRequestedUnit) {
    foreign_unit = true;
    start();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_EQ(outcome->second.native_error, -EPERM);
    ASSERT_EQ(units.size(), 1U);
    EXPECT_TRUE(activation->stop());
    ASSERT_EQ(stopped.size(), 1U);
    EXPECT_EQ(stopped.front(), units.front().name);
}

TEST_F(PortalActivationTest, RecoversOnlyMissingOwnedUnits) {
    RecordProperty("part", "9");
    start();
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    ASSERT_EQ(started.size(), 3U);
    const auto backend = started.front();
    ASSERT_TRUE(release(portals.front()));
    recover();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::none);
    ASSERT_EQ(started.size(), 4U);
    EXPECT_EQ(started.back(), backend);
    EXPECT_EQ(get_unit_by_pid, portals.size());
    EXPECT_TRUE(stopped.empty());
    EXPECT_TRUE(activation->stop());
    EXPECT_EQ(stopped.size(), portals.size());
}

TEST_F(PortalActivationTest, LeavesMissingBackendStoppedWhenAnotherSessionOwnsTheFrontend) {
    start();
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    ASSERT_TRUE(release(portals[0]));
    ASSERT_TRUE(release(portals[2]));
    ASSERT_TRUE(connect(external));
    ASSERT_TRUE(claim(*external, portals[2].name));
    foreign_unit = true;
    recover();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_EQ(started.size(), portals.size());
}

TEST_F(PortalActivationTest, RuntimeRecoveryVerifiesExistingOwnerBeforeReuse) {
    RecordProperty("part", "9");
    start();
    ASSERT_EQ(outcome->second.failure, ServiceFailure::none);
    foreign_unit = true;
    recover();
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->second.failure, ServiceFailure::wrong_environment);
    EXPECT_EQ(outcome->second.native_error, -EPERM);
    EXPECT_EQ(started.size(), portals.size());
    EXPECT_TRUE(stopped.empty());
}
} // namespace

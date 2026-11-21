#include "session_bus.hpp"
#include "session_portal.hpp"
#include "test_dbus_daemon.hpp"
#include "test_directory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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
using seaside::PortalProbe;
using seaside::ServiceFailure;
using seaside::ServiceReport;
using seaside::SessionBus;
using seaside::StartupService;

constexpr char desktop_path[] = "/org/freedesktop/portal/desktop";
constexpr char frontend_name[] = "org.freedesktop.portal.Desktop";
constexpr char backend_name[] = "org.freedesktop.impl.portal.desktop.wlr";

class PortalProbeTest : public testing::Test {
  protected:
    using BusSlot = std::unique_ptr<sd_bus_slot, decltype(&sd_bus_slot_unref)>;
    using EventLoop = std::unique_ptr<wl_event_loop, decltype(&wl_event_loop_destroy)>;
    using Report = std::pair<StartupService, ServiceFailure>;

    seaside::test::Directory directory;
    const std::string address = "unix:path=" + (directory.path() / "bus").string();
    seaside::test::PrivateDaemon daemon{address};
    EventLoop loop{wl_event_loop_create(), wl_event_loop_destroy};
    std::unique_ptr<SessionBus> client, frontend, backend;
    std::vector<BusSlot> vtables;
    std::unique_ptr<PortalProbe> probe;
    std::vector<Report> reports;
    std::vector<Report> runtime_reports;
    std::uint32_t version = 4;
    std::uint32_t frontend_sources = 3;
    std::uint32_t backend_sources = 3;
    std::uint32_t cursors = 3;
    std::uint32_t screenshot_version = 1;
    bool binding_matches = true;
    bool from_unrelated_sender = false;
    bool wrong_session = false;
    bool response_pending = false;
    bool backend_session_exported = true;
    bool early_response = false;
    unsigned binding_checks = 0;
    unsigned request_closes = 0;
    unsigned session_closes = 0;
    unsigned creates = 0;

    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3.1-portal-readiness");
        ASSERT_TRUE(loop);
    }

    bool pump_until(const std::function<bool()> &ready,
                    std::chrono::milliseconds timeout = 3s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!ready()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                wl_event_loop_dispatch(loop.get(), 10) < 0) {
                return false;
            }
        }
        return true;
    }

    bool connect(std::unique_ptr<SessionBus> &target, bool ready = true) {
        auto connection = SessionBus::connect(loop.get(), address);
        if (!connection) {
            return false;
        }
        target = std::move(*connection);
        return !ready || pump_until([&] { return target->ready(); });
    }

    static int acquired(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        std::uint32_t reply = 0;
        *static_cast<bool *>(data) = sd_bus_message_read(message, "u", &reply) > 0 && reply == 1;
        return 1;
    }

    bool claim(SessionBus &bus, const char *name) {
        bool ok = false;
        sd_bus_slot *slot = nullptr;
        const auto result =
            sd_bus_request_name_async(bus.native_handle(), &slot, name, 0, acquired, &ok);
        BusSlot name_slot(slot, sd_bus_slot_unref);
        return result >= 0 && pump_until([&] { return ok; });
    }

    bool export_interface(SessionBus &bus, const char *path, const char *interface,
                          const sd_bus_vtable *table) {
        sd_bus_slot *raw = nullptr;
        const auto result =
            sd_bus_add_object_vtable(bus.native_handle(), &raw, path, interface, table, this);
        BusSlot slot(raw, sd_bus_slot_unref);
        if (result < 0) {
            return false;
        }
        vtables.push_back(std::move(slot));
        return true;
    }

    static int read_property(sd_bus *, const char *, const char *interface, const char *property,
                             sd_bus_message *reply, void *data, sd_bus_error *) noexcept {
        auto &self = *static_cast<PortalProbeTest *>(data);
        std::uint32_t value = self.version;
        if (std::string_view(interface).ends_with(".Screenshot")) {
            value = self.screenshot_version;
        } else if (std::string_view(property) == "AvailableSourceTypes") {
            value = std::string_view(interface) == "org.freedesktop.portal.ScreenCast"
                        ? self.frontend_sources
                        : self.backend_sources;
        } else if (std::string_view(property) == "AvailableCursorModes") {
            value = self.cursors;
        }
        return sd_bus_message_append(reply, "u", value);
    }

    bool setup_services() {
        if (!connect(frontend) || !connect(backend) || !claim(*frontend, frontend_name) ||
            !claim(*backend, backend_name)) {
            return false;
        }
        static const sd_bus_vtable frontend_cast[]{
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("CreateSession", "a{sv}", "o", create, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_PROPERTY("version", "u", read_property, 0, 0),
            SD_BUS_PROPERTY("AvailableSourceTypes", "u", read_property, 0, 0),
            SD_BUS_PROPERTY("AvailableCursorModes", "u", read_property, 0, 0),
            SD_BUS_VTABLE_END};
        static const sd_bus_vtable backend_cast[]{
            SD_BUS_VTABLE_START(0), SD_BUS_PROPERTY("version", "u", read_property, 0, 0),
            SD_BUS_PROPERTY("AvailableSourceTypes", "u", read_property, 0, 0),
            SD_BUS_PROPERTY("AvailableCursorModes", "u", read_property, 0, 0), SD_BUS_VTABLE_END};
        static const sd_bus_vtable shot[]{SD_BUS_VTABLE_START(0),
                                          SD_BUS_PROPERTY("version", "u", read_property, 0, 0),
                                          SD_BUS_VTABLE_END};
        return export_interface(*frontend, desktop_path, "org.freedesktop.portal.ScreenCast",
                                frontend_cast) &&
               export_interface(*backend, desktop_path, "org.freedesktop.impl.portal.ScreenCast",
                                backend_cast) &&
               export_interface(*frontend, desktop_path, "org.freedesktop.portal.Screenshot",
                                shot) &&
               export_interface(*backend, desktop_path, "org.freedesktop.impl.portal.Screenshot",
                                shot);
    }

    void start(std::chrono::milliseconds timeout = 2s, uid_t expected_uid = geteuid(),
               bool monitor = false) {
        ASSERT_TRUE(connect(client, false));
        PortalProbe::Completion fault;
        if (monitor) {
            fault = [this](StartupService service, ServiceReport report) {
                runtime_reports.emplace_back(service, report.failure);
            };
        }
        auto started = PortalProbe::start(
            *client, expected_uid,
            [this](pid_t pid, uid_t uid) {
                ++binding_checks;
                EXPECT_EQ(pid, getpid());
                EXPECT_EQ(uid, geteuid());
                return binding_matches;
            },
            [this](StartupService service, ServiceReport report) {
                reports.emplace_back(service, report.failure);
            },
            timeout, std::move(fault));
        ASSERT_TRUE(started);
        probe = std::move(*started);
    }

    static int close_request(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &self = *static_cast<PortalProbeTest *>(data);
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        ++self.request_closes;
        return sd_bus_reply_method_return(message, "");
    }

    static int close_session(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto &self = *static_cast<PortalProbeTest *>(data);
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        ++self.session_closes;
        return sd_bus_reply_method_return(message, "");
    }

    static int create(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        try {
            return static_cast<PortalProbeTest *>(data)->handle_create(message);
        } catch (...) {
            return -ENOMEM;
        }
    }

    int handle_create(sd_bus_message *message) {
        ++creates;
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        EXPECT_EQ(sd_bus_message_get_allow_interactive_authorization(message), 0);
        std::string request_token, session_token;
        if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0) {
            return -EINVAL;
        }
        int entry = 0;
        while ((entry = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
            const char *key = nullptr;
            const char *value = nullptr;
            if (sd_bus_message_read(message, "sv", &key, "s", &value) <= 0 ||
                sd_bus_message_exit_container(message) < 0) {
                return -EINVAL;
            }
            if (std::string_view(key) == "handle_token") {
                request_token = value;
            } else if (std::string_view(key) == "session_handle_token") {
                session_token = value;
            } else {
                return -EINVAL;
            }
        }
        const auto *sender = sd_bus_message_get_sender(message);
        if (entry < 0 || request_token.empty() || session_token.empty() || !sender) {
            return -EINVAL;
        }
        std::string escaped(sender + 1);
        std::replace(escaped.begin(), escaped.end(), '.', '_');
        const auto request =
            std::string(desktop_path) + "/request/" + escaped + '/' + request_token;
        const auto session =
            std::string(desktop_path) + "/session/" + escaped + '/' + session_token;
        static const sd_bus_vtable request_table[]{
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Close", "", "", close_request, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END};
        static const sd_bus_vtable session_table[]{
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Close", "", "", close_session, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_VTABLE_END};
        if (!export_interface(*frontend, request.c_str(), "org.freedesktop.portal.Request",
                              request_table) ||
            !export_interface(*frontend, session.c_str(), "org.freedesktop.portal.Session",
                              session_table) ||
            (backend_session_exported &&
             !export_interface(*backend, session.c_str(), "org.freedesktop.impl.portal.Session",
                               session_table))) {
            return -EIO;
        }
        const std::string result_session = wrong_session ? session + "_invalid" : session;
        if (!response_pending && early_response) {
            if (const int result = emit_response(sender, request, result_session); result < 0) {
                return result;
            }
        }
        const int method_reply = sd_bus_reply_method_return(message, "o", request.c_str());
        if (method_reply < 0 || response_pending || early_response) {
            return method_reply;
        }
        return emit_response(sender, request, result_session) < 0 ? -EIO : method_reply;
    }

    int emit_response(const char *destination, const std::string &request,
                      const std::string &session) const noexcept {
        auto *sender = (from_unrelated_sender ? backend : frontend)->native_handle();
        sd_bus_message *raw = nullptr;
        int result = sd_bus_message_new_signal(sender, &raw, request.c_str(),
                                               "org.freedesktop.portal.Request", "Response");
        std::unique_ptr<sd_bus_message, decltype(&sd_bus_message_unref)> signal(
            raw, sd_bus_message_unref);
        if (result >= 0) {
            result = sd_bus_message_set_destination(raw, destination);
        }
        if (result >= 0) {
            result =
                sd_bus_message_append(raw, "ua{sv}", 0u, 1, "session_handle", "s", session.c_str());
        }
        return result < 0 ? result : sd_bus_send(sender, raw, nullptr);
    }
};

TEST_F(PortalProbeTest, VerifiesPublicRouteAndClosesUnstartedSessionEvenForEarlyResponse) {
    early_response = true;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    ASSERT_EQ(reports.size(), 2U);
    EXPECT_EQ(reports[0], (Report{StartupService::portal_frontend, ServiceFailure::none}));
    EXPECT_EQ(reports[1], (Report{StartupService::capture_backend, ServiceFailure::none}));
    EXPECT_EQ(creates, 1U);
    EXPECT_EQ(session_closes, 1U);
    EXPECT_EQ(request_closes, 0U);
    EXPECT_GE(binding_checks, 2U);
}

TEST_F(PortalProbeTest, FrontendWithoutScreenshotInterfaceVersionIsIncompatible) {
    screenshot_version = 0;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::portal_frontend, ServiceFailure::incompatible}}));
    EXPECT_EQ(creates, 0U);
}

TEST_F(PortalProbeTest, RejectsFrontendWithoutWindowSources) {
    frontend_sources = 1;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::portal_frontend, ServiceFailure::incompatible}}));
    EXPECT_EQ(creates, 0U);
}

TEST_F(PortalProbeTest, RejectsBackendWithoutWindowSources) {
    backend_sources = 1;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::incompatible}}));
    EXPECT_EQ(creates, 0U);
}

TEST_F(PortalProbeTest, UnownedFrontendNameDoesNotActivateServices) {
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::portal_frontend, ServiceFailure::missing}}));
}

TEST_F(PortalProbeTest, RejectsBackendWithoutBindsOnThisWaylandDisplay) {
    binding_matches = false;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports, (std::vector{Report{StartupService::capture_backend,
                                           ServiceFailure::wrong_environment}}));
    EXPECT_EQ(creates, 0U);
}

TEST_F(PortalProbeTest, RejectsBackendCredentialsOwnedByAnotherUid) {
    ASSERT_TRUE(setup_services());
    start(2s, geteuid() == 0 ? 1 : 0);
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports, (std::vector{Report{StartupService::capture_backend,
                                           ServiceFailure::wrong_environment}}));
    EXPECT_EQ(binding_checks, 0U);
}

TEST_F(PortalProbeTest, RejectsFrontendSessionRoutedToDifferentBackend) {
    backend_session_exported = false;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::incompatible}}));
    ASSERT_TRUE(pump_until([&] { return session_closes == 1 && request_closes == 1; }));
}

TEST_F(PortalProbeTest, IgnoresResponseFromAnUnrelatedBusOwner) {
    from_unrelated_sender = true;
    ASSERT_TRUE(setup_services());
    start(200ms);
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::timeout}}));
    ASSERT_TRUE(pump_until([&] { return session_closes == 1 && request_closes == 1; }));
}

TEST_F(PortalProbeTest, RejectsWrongSessionPathThroughThePinnedFrontend) {
    wrong_session = true;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::incompatible}}));
    ASSERT_TRUE(pump_until([&] { return session_closes == 1 && request_closes == 1; }));
}

TEST_F(PortalProbeTest, DestructionClosesOwnedPendingRequestWithoutReporting) {
    response_pending = true;
    ASSERT_TRUE(setup_services());
    start();
    ASSERT_TRUE(pump_until([&] { return creates == 1; }));
    probe.reset();
    ASSERT_TRUE(pump_until([&] { return session_closes == 1 && request_closes == 1; }));
    EXPECT_TRUE(reports.empty());
}

TEST_F(PortalProbeTest, TimerCompletionCanDestroyProbeAndDispatchAgain) {
    ASSERT_TRUE(setup_services());
    ASSERT_TRUE(connect(client, false));
    auto started = PortalProbe::start(
        *client, geteuid(), [](pid_t, uid_t) { return false; },
        [this](StartupService service, ServiceReport report) {
            probe.reset();
            EXPECT_EQ(wl_event_loop_dispatch(loop.get(), 0), 0);
            reports.emplace_back(service, report.failure);
        },
        2s);
    ASSERT_TRUE(started);
    probe = std::move(*started);
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    EXPECT_EQ(reports, (std::vector{Report{StartupService::capture_backend,
                                           ServiceFailure::wrong_environment}}));
    EXPECT_FALSE(probe);
}

TEST_F(PortalProbeTest, ReportsPinnedOwnersDisappearingWithoutEndingLoopOrRestarting) {
    ASSERT_TRUE(setup_services());
    start(2s, geteuid(), true);
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    ASSERT_EQ(reports.size(), 2U);
    ASSERT_TRUE(runtime_reports.empty());

    backend.reset();
    ASSERT_TRUE(pump_until([&] { return !runtime_reports.empty(); }));
    EXPECT_EQ(runtime_reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::disconnected}}));
    ASSERT_TRUE(connect(backend));
    ASSERT_TRUE(claim(*backend, backend_name));
    backend.reset();
    frontend.reset();
    ASSERT_TRUE(pump_until([&] { return runtime_reports.size() >= 2; }));
    EXPECT_EQ(runtime_reports,
              (std::vector{Report{StartupService::capture_backend, ServiceFailure::disconnected},
                           Report{StartupService::portal_frontend, ServiceFailure::disconnected}}));
    EXPECT_TRUE(client->ready());
}

TEST_F(PortalProbeTest, ReportsBothPinnedOwnersLostWhenLoginBusDisconnects) {
    ASSERT_TRUE(setup_services());
    start(2s, geteuid(), true);
    ASSERT_TRUE(pump_until([&] { return !reports.empty(); }));
    ASSERT_EQ(reports.size(), 2U);
    ASSERT_TRUE(runtime_reports.empty());
    daemon.stop();
    ASSERT_TRUE(pump_until([&] { return runtime_reports.size() >= 2; }));
    EXPECT_EQ(runtime_reports,
              (std::vector{Report{StartupService::portal_frontend, ServiceFailure::disconnected},
                           Report{StartupService::capture_backend, ServiceFailure::disconnected}}));
}
} // namespace

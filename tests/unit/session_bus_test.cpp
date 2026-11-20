#include "session_bus.hpp"
#include "session_systemd.hpp"
#include "test_directory.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-server-core.h>

extern char **environ;

namespace {
using namespace std::chrono_literals;
using seaside::BusError;
using seaside::BusReply;
using seaside::ServiceFailure;
using seaside::ServiceReport;
using seaside::SessionBus;
using seaside::SystemdUserProbe;
bool fail_bus_wakes = false;
bool fail_bus_timer_updates = false;

struct Descriptor {
    int value = -1;
    ~Descriptor() {
        if (value >= 0) {
            close(value);
        }
    }
};

template <class Value>
void expect_error(const std::expected<Value, BusError> &result, BusError expected) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), expected);
}

class PrivateDaemon {
  public:
    explicit PrivateDaemon(const std::string &address) {
        std::array<int, 2> pipe{};
        if (pipe2(pipe.data(), O_CLOEXEC) < 0) {
            throw std::runtime_error("dbus startup pipe");
        }
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error != 0) {
            close(pipe[0]);
            close(pipe[1]);
            throw std::runtime_error("dbus startup file actions");
        }
        if (error == 0) {
            error = posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
        }
        if (error == 0) {
            error = posix_spawn_file_actions_addclose(&actions, pipe[0]);
        }
        if (error == 0) {
            error = posix_spawn_file_actions_addclose(&actions, pipe[1]);
        }
        std::string option = "--address=" + address;
        std::array<char *, 7> args{const_cast<char *>("dbus-daemon"),
                                   const_cast<char *>("--session"),
                                   const_cast<char *>("--nofork"),
                                   const_cast<char *>("--nopidfile"),
                                   option.data(),
                                   const_cast<char *>("--print-address=1"),
                                   nullptr};
        if (error == 0) {
            error = posix_spawnp(&pid_, args[0], &actions, nullptr, args.data(), environ);
        }
        posix_spawn_file_actions_destroy(&actions);
        close(pipe[1]);
        if (error != 0) {
            close(pipe[0]);
            throw std::runtime_error("dbus-daemon spawn failed");
        }
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        std::string output;
        while (std::chrono::steady_clock::now() < deadline &&
               output.find('\n') == std::string::npos) {
            pollfd fd{pipe[0], POLLIN, 0};
            if (poll(&fd, 1, 20) > 0) {
                std::array<char, 4096> bytes{};
                const auto count = read(pipe[0], bytes.data(), bytes.size());
                if (count <= 0) {
                    break;
                }
                output.append(bytes.data(), static_cast<std::size_t>(count));
                if (output.size() > 4096) {
                    break;
                }
            }
        }
        close(pipe[0]);
        if (output.find(address) != 0 || output.find('\n') == std::string::npos) {
            stop();
            throw std::runtime_error("dbus-daemon did not become ready");
        }
    }
    ~PrivateDaemon() { stop(); }
    void stop() {
        if (pid_ <= 0) {
            return;
        }
        // This non-forking child has not been reaped, so its PID cannot yet be
        // reused for an unrelated process.
        kill(pid_, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
        kill(pid_, SIGKILL);
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

  private:
    pid_t pid_ = -1;
};

class SessionBusTest : public testing::Test {
  protected:
    seaside::test::Directory directory;
    std::string address;
    std::unique_ptr<PrivateDaemon> daemon;
    wl_event_loop *loop = nullptr;
    std::unique_ptr<SessionBus> bus;
    std::unique_ptr<SessionBus> service;
    sd_bus_slot *service_slot = nullptr;
    std::vector<sd_bus_message *> held;
    std::string service_name;

    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3.1-sd-bus-external-loop");
        address = "unix:path=" + (directory.path() / "bus").string();
        daemon = std::make_unique<PrivateDaemon>(address);
        loop = wl_event_loop_create();
        ASSERT_NE(loop, nullptr);
    }
    void TearDown() override {
        fail_bus_wakes = false;
        fail_bus_timer_updates = false;
        for (auto *message : held) {
            sd_bus_message_unref(message);
        }
        held.clear();
        sd_bus_slot_unref(service_slot);
        service.reset();
        bus.reset();
        wl_event_loop_destroy(loop);
        daemon.reset();
    }
    bool pump_until(const std::function<bool()> &predicate,
                    std::chrono::milliseconds timeout = 2s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                wl_event_loop_dispatch(loop, 10) < 0) {
                return false;
            }
        }
        return true;
    }
    bool connect(SessionBus::Limits limits = {}) {
        auto result = SessionBus::connect(loop, address, limits);
        if (!result) {
            return false;
        }
        bus = std::move(*result);
        return pump_until([&] { return bus->ready(); });
    }
    static int hold(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        auto *test = static_cast<SessionBusTest *>(data);
        auto *reference = sd_bus_message_ref(message);
        try {
            test->held.push_back(reference);
            return 1;
        } catch (...) {
            sd_bus_message_unref(reference);
            return -ENOMEM;
        }
    }
    static int ping(sd_bus_message *message, void *, sd_bus_error *) noexcept {
        return sd_bus_reply_method_return(message, "s", "pong");
    }
    bool start_service() {
        auto result = SessionBus::connect(loop, address);
        if (!result) {
            return false;
        }
        service = std::move(*result);
        if (!pump_until([&] { return service->ready(); })) {
            return false;
        }
        const char *name = nullptr;
        if (sd_bus_get_unique_name(service->native_handle(), &name) < 0) {
            return false;
        }
        service_name = name;
        static const sd_bus_vtable table[]{
            SD_BUS_VTABLE_START(0),
            SD_BUS_METHOD("Ping", "", "s", ping, SD_BUS_VTABLE_UNPRIVILEGED),
            SD_BUS_METHOD("Hold", "", "s", hold, SD_BUS_VTABLE_UNPRIVILEGED), SD_BUS_VTABLE_END};
        return sd_bus_add_object_vtable(service->native_handle(), &service_slot,
                                        "/org/seaside/Test", "org.seaside.Test", table, this) >= 0;
    }
    std::expected<SessionBus::CallId, BusError> request(const char *method,
                                                        SessionBus::Callback callback,
                                                        std::chrono::microseconds timeout = 1s) {
        sd_bus_message *message = nullptr;
        const int created =
            sd_bus_message_new_method_call(bus->native_handle(), &message, service_name.c_str(),
                                           "/org/seaside/Test", "org.seaside.Test", method);
        if (created < 0) {
            ADD_FAILURE() << "message creation: " << created;
            return std::unexpected(BusError::system);
        }
        auto result = bus->call(message, timeout, std::move(callback));
        sd_bus_message_unref(message);
        return result;
    }
    void release_held() {
        for (auto *message : held) {
            EXPECT_GE(sd_bus_reply_method_return(message, "s", "late"), 0);
            sd_bus_message_unref(message);
        }
        held.clear();
    }
};

class SystemdUserProbeTest : public SessionBusTest {
  protected:
    enum class VersionReply { valid, wrong_type, access_denied, held };
    VersionReply version_reply = VersionReply::valid;
    unsigned version_requests = 0;
    bool manager_named = false;

    bool prepare_manager() {
        if (!start_service()) {
            return false;
        }
        return sd_bus_add_filter(service->native_handle(), nullptr, get_version, this) >= 0;
    }

    bool register_manager() {
        if (sd_bus_request_name_async(service->native_handle(), nullptr, "org.freedesktop.systemd1",
                                      0, name_acquired, this) < 0) {
            return false;
        }
        return pump_until([&] { return manager_named; });
    }

    bool start_manager() { return prepare_manager() && register_manager(); }

    static int name_acquired(sd_bus_message *message, void *data, sd_bus_error *) noexcept {
        std::uint32_t result = 0;
        static_cast<SystemdUserProbeTest *>(data)->manager_named =
            sd_bus_message_read(message, "u", &result) > 0 && result == 1;
        return 1;
    }

    static int get_version(sd_bus_message *message, void *data, sd_bus_error *) {
        if (sd_bus_message_is_method_call(message, "org.freedesktop.DBus.Properties", "Get") <= 0) {
            return 0;
        }
        auto &test = *static_cast<SystemdUserProbeTest *>(data);
        ++test.version_requests;
        EXPECT_STREQ(sd_bus_message_get_path(message), "/org/freedesktop/systemd1");
        EXPECT_EQ(sd_bus_message_get_auto_start(message), 0);
        EXPECT_EQ(sd_bus_message_get_allow_interactive_authorization(message), 0);
        const char *interface = nullptr;
        const char *property = nullptr;
        if (sd_bus_message_read(message, "ss", &interface, &property) <= 0) {
            return -EINVAL;
        }
        EXPECT_STREQ(interface, "org.freedesktop.systemd1.Manager");
        EXPECT_STREQ(property, "Version");
        switch (test.version_reply) {
        case VersionReply::valid:
            return sd_bus_reply_method_return(message, "v", "s", "262-test");
        case VersionReply::wrong_type:
            return sd_bus_reply_method_return(message, "v", "u", 262U);
        case VersionReply::access_denied:
            return sd_bus_reply_method_errorf(message, SD_BUS_ERROR_ACCESS_DENIED, "test denial");
        case VersionReply::held:
            return hold(message, static_cast<SessionBusTest *>(&test), nullptr);
        }
        return -EINVAL;
    }
};

TEST_F(SystemdUserProbeTest, ReadsTheManagerAndChecksRealBusCredentialsAsynchronously) {
    ASSERT_TRUE(start_manager());
    auto connected = SessionBus::connect(loop, address);
    ASSERT_TRUE(connected);
    bus = std::move(*connected);
    EXPECT_FALSE(bus->ready());
    std::optional<ServiceReport> outcome;
    std::unique_ptr<SystemdUserProbe> probe;
    auto started = SystemdUserProbe::start(*bus, geteuid(), [&](ServiceReport result) {
        outcome = result;
        probe.reset();
    });
    ASSERT_TRUE(started);
    probe = std::move(*started);
    EXPECT_FALSE(outcome);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::none);
    EXPECT_FALSE(probe);
    EXPECT_EQ(version_requests, 1U);
    EXPECT_EQ(bus->pending(), 0U);
}

TEST_F(SystemdUserProbeTest, RejectsAManagerOwnedByAnotherUid) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_manager());
    std::optional<ServiceReport> outcome;
    const uid_t other_uid = geteuid() == 0 ? 1 : 0;
    auto probe =
        SystemdUserProbe::start(*bus, other_uid, [&](ServiceReport result) { outcome = result; });
    ASSERT_TRUE(probe);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::wrong_environment);
    EXPECT_EQ(version_requests, 1U);
}

TEST_F(SystemdUserProbeTest, ReportsAMissingManager) {
    ASSERT_TRUE(connect());
    std::optional<ServiceReport> outcome;
    auto probe =
        SystemdUserProbe::start(*bus, geteuid(), [&](ServiceReport result) { outcome = result; });
    ASSERT_TRUE(probe);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::missing);
    EXPECT_LT(outcome->native_error, 0);
}

TEST_F(SystemdUserProbeTest, RetriesWhenTheManagerRegistersAfterTheFirstRequest) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(prepare_manager());
    std::optional<ServiceReport> outcome;
    auto probe =
        SystemdUserProbe::start(*bus, geteuid(), [&](ServiceReport result) { outcome = result; });
    ASSERT_TRUE(probe);
    ASSERT_EQ(bus->pending(), 1U);
    // Drain the first reply while the well-known name is still unowned.
    ASSERT_TRUE(pump_until([&] { return bus->pending() == 0; }));
    ASSERT_FALSE(outcome);
    ASSERT_TRUE(register_manager());
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::none);
    EXPECT_EQ(version_requests, 1U);
}

TEST_F(SystemdUserProbeTest, DestructionCancelsTheArmedRegistrationRetry) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(prepare_manager());
    unsigned completions = 0;
    auto probe = SystemdUserProbe::start(*bus, geteuid(), [&](ServiceReport) { ++completions; });
    ASSERT_TRUE(probe);
    ASSERT_EQ(bus->pending(), 1U);
    ASSERT_TRUE(pump_until([&] { return bus->pending() == 0; }));
    ASSERT_EQ(completions, 0U);
    probe->reset();
    ASSERT_TRUE(register_manager());
    const auto observation_end = std::chrono::steady_clock::now() + 200ms;
    ASSERT_TRUE(pump_until([&] { return std::chrono::steady_clock::now() >= observation_end; }));
    EXPECT_EQ(completions, 0U);
    EXPECT_EQ(version_requests, 0U);
    EXPECT_EQ(bus->pending(), 0U);
}

TEST_F(SystemdUserProbeTest, OverdueRegistrationRetryTimesOutAndCanDestroyTheProbe) {
    ASSERT_TRUE(connect());
    std::optional<ServiceReport> outcome;
    std::unique_ptr<SystemdUserProbe> probe;
    auto started = SystemdUserProbe::start(
        *bus, geteuid(),
        [&](ServiceReport result) {
            probe.reset();
            EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
            outcome = result;
        },
        300ms);
    ASSERT_TRUE(started);
    probe = std::move(*started);
    ASSERT_EQ(bus->pending(), 1U);
    ASSERT_TRUE(pump_until([&] { return bus->pending() == 0; }));
    ASSERT_FALSE(outcome);
    // Stall dispatch so the armed retry is first delivered after the deadline.
    std::this_thread::sleep_for(300ms);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::timeout);
    EXPECT_FALSE(probe);
    EXPECT_EQ(bus->pending(), 0U);
}

TEST_F(SystemdUserProbeTest, RejectsAnIncompatibleVersionAndDistinguishesAccessDenial) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_manager());
    std::optional<ServiceReport> outcome;
    version_reply = VersionReply::wrong_type;
    auto capture = [&](ServiceReport result) { outcome = result; };
    auto incompatible = SystemdUserProbe::start(*bus, geteuid(), capture);
    ASSERT_TRUE(incompatible);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::incompatible);

    version_reply = VersionReply::access_denied;
    outcome.reset();
    auto denied = SystemdUserProbe::start(*bus, geteuid(), capture);
    ASSERT_TRUE(denied);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::activation_failed);
}

TEST_F(SystemdUserProbeTest, TimesOutAndSilentlyCancelsAnUnansweredProbe) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_manager());
    version_reply = VersionReply::held;

    std::optional<ServiceReport> outcome;
    auto capture = [&](ServiceReport result) { outcome = result; };
    auto timed = SystemdUserProbe::start(*bus, geteuid(), capture, 30ms);
    ASSERT_TRUE(timed);
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::timeout);
    EXPECT_EQ(bus->pending(), 0U);
    release_held();

    outcome.reset();
    auto cancelled = SystemdUserProbe::start(*bus, geteuid(), capture);
    ASSERT_TRUE(cancelled);
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    cancelled->reset();
    EXPECT_EQ(bus->pending(), 0U);
    release_held();
    bool ping_completed = false;
    ASSERT_TRUE(request("Ping", [&](const BusReply &) { ping_completed = true; }));
    ASSERT_TRUE(pump_until([&] { return ping_completed; }));
    EXPECT_FALSE(outcome);
}

TEST_F(SystemdUserProbeTest, ReportsLiveBusDisconnect) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_manager());
    version_reply = VersionReply::held;
    std::optional<ServiceReport> outcome;
    auto probe =
        SystemdUserProbe::start(*bus, geteuid(), [&](ServiceReport result) { outcome = result; });
    ASSERT_TRUE(probe);
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    daemon->stop();
    ASSERT_TRUE(pump_until([&] { return outcome.has_value(); }));
    EXPECT_EQ(outcome->failure, ServiceFailure::disconnected);
    EXPECT_EQ(bus->pending(), 0U);
}

TEST_F(SessionBusTest, RejectsInvalidSetupWithoutStartingAConnection) {
    expect_error(SessionBus::connect(nullptr, address), BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, ""), BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, "unixexec:path=/bin/true"), BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, address + ";" + address), BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, address + std::string(1, '\0') + "ignored"),
                 BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, address, {.max_pending_calls = 0}),
                 BusError::invalid_arguments);
    expect_error(SessionBus::connect(loop, address, {.dispatch_budget = 0}),
                 BusError::invalid_arguments);
    EXPECT_FALSE(SessionBus::connect(loop, "unix:path=" + (directory.path() / "missing").string()));
}

TEST_F(SessionBusTest, CompletesRealMethodCallsAndReportsRemoteErrorsOnOwnerLoop) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    const auto owner = std::this_thread::get_id();
    unsigned success = 0;
    unsigned errors = 0;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(std::this_thread::get_id(), owner);
        EXPECT_EQ(reply.error, BusError::none);
        const char *text = nullptr;
        ASSERT_GE(sd_bus_message_read(reply.message, "s", &text), 0);
        EXPECT_STREQ(text, "pong");
        ++success;
    }));
    ASSERT_TRUE(request("Absent", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::remote_error);
        ASSERT_NE(reply.message, nullptr);
        const auto *error = sd_bus_message_get_error(reply.message);
        ASSERT_NE(error, nullptr);
        EXPECT_STREQ(error->name, SD_BUS_ERROR_UNKNOWN_METHOD);
        ++errors;
    }));
    EXPECT_EQ(success, 0U);
    EXPECT_EQ(errors, 0U);
    ASSERT_TRUE(pump_until([&] { return success == 1 && errors == 1; }));
    EXPECT_EQ(bus->pending(), 0U);
    EXPECT_EQ(bus->last_error(), BusError::none);
}

TEST_F(SessionBusTest, EnforcesDeadlinesWithoutBlockingAndIgnoresLateReplies) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    ASSERT_TRUE(request(
        "Hold",
        [&](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::timeout);
            ++callbacks;
        },
        20ms));
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
    EXPECT_EQ(bus->pending(), 0U);
    release_held();
    bool ping_completed = false;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ping_completed = true;
    }));
    ASSERT_TRUE(pump_until([&] { return ping_completed; }));
    EXPECT_EQ(callbacks, 1U);
}

TEST_F(SessionBusTest, CallerDeadlineStillExpiresWhenAuthenticationNeverCompletes) {
    Descriptor listener{socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    ASSERT_GE(listener.value, 0);
    const auto socket_path = (directory.path() / "silent").string();
    sockaddr_un endpoint{};
    endpoint.sun_family = AF_UNIX;
    ASSERT_LT(socket_path.size(), sizeof(endpoint.sun_path));
    std::copy(socket_path.begin(), socket_path.end(), endpoint.sun_path);
    ASSERT_EQ(bind(listener.value, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)),
              0);
    ASSERT_EQ(listen(listener.value, 1), 0);
    auto connected = SessionBus::connect(loop, "unix:path=" + socket_path);
    ASSERT_TRUE(connected);
    bus = std::move(*connected);
    Descriptor accepted{accept4(listener.value, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC)};
    ASSERT_GE(accepted.value, 0);
    EXPECT_FALSE(bus->ready());
    sd_bus_message *message = nullptr;
    ASSERT_GE(sd_bus_message_new_method_call(bus->native_handle(), &message, "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                             "ListNames"),
              0);
    unsigned callbacks = 0;
    const auto called = bus->call(message, 20ms, [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::timeout);
        ++callbacks;
    });
    sd_bus_message_unref(message);
    ASSERT_TRUE(called);
    bool other_source_dispatched = false;
    ASSERT_NE(
        wl_event_loop_add_idle(
            loop, [](void *data) { *static_cast<bool *>(data) = true; }, &other_source_dispatched),
        nullptr);
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }, 500ms));
    EXPECT_TRUE(other_source_dispatched);
    EXPECT_FALSE(bus->ready());
    EXPECT_EQ(bus->pending(), 0U);
}

TEST_F(SessionBusTest, ReplyAfterTheDeadlineCannotReviveACallBeforeTimerDispatch) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    ASSERT_TRUE(request(
        "Hold",
        [&](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::timeout);
            ++callbacks;
        },
        20ms));
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    std::this_thread::sleep_for(30ms);
    release_held();
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
    EXPECT_EQ(bus->pending(), 0U);
    bool ping_completed = false;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ping_completed = true;
    }));
    ASSERT_TRUE(pump_until([&] { return ping_completed; }));
    EXPECT_EQ(callbacks, 1U);
}

TEST_F(SessionBusTest, CallbackCanCancelAnotherCallSubmitAndReenterTheLoop) {
    ASSERT_TRUE(connect({.dispatch_budget = 1}));
    ASSERT_TRUE(start_service());
    unsigned cancelled_callbacks = 0;
    auto held_call = request("Hold", [&](const BusReply &) { ++cancelled_callbacks; });
    ASSERT_TRUE(held_call);
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    unsigned pings = 0;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ++pings;
        EXPECT_TRUE(bus->cancel(*held_call));
        EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
        EXPECT_TRUE(request("Ping", [&](const BusReply &second) {
            EXPECT_EQ(second.error, BusError::none);
            ++pings;
        }));
    }));
    ASSERT_TRUE(pump_until([&] { return pings == 2; }));
    EXPECT_EQ(cancelled_callbacks, 0U);
    EXPECT_EQ(bus->pending(), 0U);
    release_held();
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(cancelled_callbacks, 0U);
}

TEST_F(SessionBusTest, CancelsWithoutDeliveringAndCapacityIsReusable) {
    ASSERT_TRUE(connect({.max_pending_calls = 1}));
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    auto first = request("Hold", [&](const BusReply &) { ++callbacks; });
    ASSERT_TRUE(first);
    expect_error(request("Ping", [&](const BusReply &) { ++callbacks; }), BusError::full);
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    ASSERT_TRUE(bus->cancel(*first));
    expect_error(bus->cancel(*first), BusError::not_found);
    EXPECT_EQ(bus->pending(), 0U);
    release_held();
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ++callbacks;
    }));
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
}

TEST_F(SessionBusTest, DisconnectCompletesPendingCallsAndKeepsTheWaylandLoopAlive) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    ASSERT_TRUE(request("Hold", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::disconnected);
        ++callbacks;
    }));
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    daemon->stop();
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
    ASSERT_TRUE(pump_until([&] { return bus->last_error() == BusError::disconnected; }));
    EXPECT_EQ(bus->pending(), 0U);
    EXPECT_EQ(bus->native_handle(), nullptr);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
}

TEST_F(SessionBusTest, ShutdownSuppressesPendingCallsAndContainsThrowingCallbacks) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    ASSERT_TRUE(request("Ping", [&](const BusReply &) {
        ++callbacks;
        throw std::runtime_error("test callback failure");
    }));
    ASSERT_TRUE(pump_until([&] { return callbacks == 1; }));
    EXPECT_EQ(bus->callback_failures(), 1U);
    ASSERT_TRUE(request("Hold", [&](const BusReply &) { ++callbacks; }));
    ASSERT_TRUE(pump_until([&] { return held.size() == 1; }));
    ASSERT_TRUE(bus->finish());
    EXPECT_EQ(bus->pending(), 0U);
    EXPECT_EQ(bus->last_error(), BusError::stopped);
    expect_error(bus->call(nullptr, 1s, [](const BusReply &) {}), BusError::stopped);
    expect_error(bus->cancel(1), BusError::stopped);
    release_held();
    bool serviced = false;
    auto *idle = wl_event_loop_add_idle(
        loop, [](void *data) { *static_cast<bool *>(data) = true; }, &serviced);
    ASSERT_NE(idle, nullptr);
    ASSERT_TRUE(pump_until([&] { return serviced; }));
    EXPECT_EQ(callbacks, 1U);
    EXPECT_TRUE(bus->finish());
}

TEST_F(SessionBusTest, CallbackMayDestroyItsOwnerAndSuppressOtherReadyReplies) {
    ASSERT_TRUE(connect());
    ASSERT_TRUE(start_service());
    unsigned callbacks = 0;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ++callbacks;
        bus.reset();
    }));
    ASSERT_TRUE(request("Ping", [&](const BusReply &) { ++callbacks; }));
    ASSERT_TRUE(pump_until([&] { return !bus; }));
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
}

TEST_F(SessionBusTest, RejectsCrossThreadOperationsAndInvalidMessages) {
    ASSERT_TRUE(connect());
    expect_error(bus->call(nullptr, 1s, [](const BusReply &) {}), BusError::invalid_arguments);
    sd_bus_message *message = nullptr;
    ASSERT_GE(sd_bus_message_new_method_call(bus->native_handle(), &message, "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                             "ListNames"),
              0);
    expect_error(bus->call(message, 0us, [](const BusReply &) {}), BusError::invalid_arguments);
    expect_error(bus->call(message, -1us, [](const BusReply &) {}), BusError::invalid_arguments);
    expect_error(bus->call(message, 1s, {}), BusError::invalid_arguments);
    sd_bus_message_unref(message);
    EXPECT_EQ(bus->pending(), 0U);
    BusError finish_error = BusError::none;
    BusError cancel_error = BusError::none;
    BusError call_error = BusError::none;
    std::jthread foreign([&] {
        const auto finished = bus->finish();
        const auto cancelled = bus->cancel(1);
        const auto called = bus->call(nullptr, 1s, [](const BusReply &) {});
        finish_error = finished ? BusError::none : finished.error();
        cancel_error = cancelled ? BusError::none : cancelled.error();
        call_error = called ? BusError::none : called.error();
        EXPECT_EQ(bus->native_handle(), nullptr);
    });
    foreign.join();
    EXPECT_EQ(finish_error, BusError::wrong_thread);
    EXPECT_EQ(cancel_error, BusError::wrong_thread);
    EXPECT_EQ(call_error, BusError::wrong_thread);
}

TEST_F(SessionBusTest, FloodedRepliesYieldToOtherReadyWaylandSources) {
    ASSERT_TRUE(connect({.dispatch_budget = 1}));
    ASSERT_TRUE(start_service());
    const int sentinel = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(sentinel, 0);
    struct Observation {
        unsigned calls = 0;
        unsigned observed_calls = 0;
        bool observed = false;
    } observation;
    auto *source = wl_event_loop_add_fd(
        loop, sentinel, WL_EVENT_READABLE,
        [](int fd, std::uint32_t, void *data) -> int {
            std::uint64_t counter = 0;
            if (read(fd, &counter, sizeof(counter)) != sizeof(counter)) {
                return 0;
            }
            auto &record = *static_cast<Observation *>(data);
            record.observed_calls = record.calls;
            record.observed = true;
            return 0;
        },
        &observation);
    ASSERT_NE(source, nullptr);
    for (unsigned i = 0; i < 8; ++i) {
        ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::none);
            ++observation.calls;
            if (observation.calls == 1) {
                const std::uint64_t counter = 1;
                EXPECT_EQ(write(sentinel, &counter, sizeof(counter)), sizeof(counter));
            }
        }));
    }
    EXPECT_TRUE(pump_until([&] { return observation.calls == 8 && observation.observed; }));
    EXPECT_LT(observation.observed_calls, 8U);
    wl_event_source_remove(source);
    close(sentinel);
}

TEST_F(SessionBusTest, PermanentWakeFailureDrainsAllFailuresThroughBoundedTimerTurns) {
    ASSERT_TRUE(connect({.dispatch_budget = 1}));
    ASSERT_TRUE(start_service());
    std::array<unsigned, 4> completions{};
    struct Progress {
        const std::array<unsigned, 4> *completions;
        unsigned observed_completions = 0;
        bool dispatched = false;
    } progress{&completions};
    std::unique_ptr<wl_event_source, decltype(&wl_event_source_remove)> heartbeat(
        wl_event_loop_add_timer(
            loop,
            [](void *data) -> int {
                auto &record = *static_cast<Progress *>(data);
                for (const auto count : *record.completions) {
                    record.observed_completions += count;
                }
                record.dispatched = true;
                return 0;
            },
            &progress),
        wl_event_source_remove);
    ASSERT_NE(heartbeat, nullptr);
    for (std::size_t i = 0; i < completions.size(); ++i) {
        ASSERT_TRUE(request("Hold", [&, i](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::system);
            EXPECT_EQ(reply.native_error, -EIO);
            ++completions[i];
            if (i == 0) {
                EXPECT_EQ(wl_event_source_timer_update(heartbeat.get(), 1), 0);
            }
        }));
    }
    ASSERT_TRUE(pump_until([&] { return held.size() == completions.size(); }));
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        fail_bus_wakes = true;
    }));
    const bool completed = pump_until(
        [&] {
            return progress.dispatched && std::all_of(completions.begin(), completions.end(),
                                                      [](auto count) { return count == 1; });
        },
        200ms);
    EXPECT_TRUE(completed);
    EXPECT_TRUE(progress.dispatched);
    EXPECT_LT(progress.observed_completions, completions.size());
    EXPECT_EQ(bus->pending(), 0U);
    EXPECT_EQ(bus->last_error(), BusError::system);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_EQ(wl_event_loop_dispatch(loop, 1), 0);
    }
    for (const auto count : completions) {
        EXPECT_EQ(count, 1U);
    }
}

TEST_F(SessionBusTest, FatalSchedulerFailureRejectsNewAdmissionAndAllowsCancelAndOwnerDestruction) {
    ASSERT_TRUE(connect({.dispatch_budget = 1}));
    ASSERT_TRUE(start_service());
    std::array<unsigned, 3> completions{};
    std::array<SessionBus::CallId, 3> ids{};
    for (std::size_t i = 0; i < completions.size(); ++i) {
        auto admitted = request("Hold", [&, i](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::system);
            ++completions[i];
            if (i == 0) {
                EXPECT_TRUE(bus->cancel(ids[1]));
            } else if (i == 2) {
                bus.reset();
            }
        });
        ASSERT_TRUE(admitted);
        ids[i] = *admitted;
    }
    ASSERT_TRUE(pump_until([&] { return held.size() == completions.size(); }));
    fail_bus_wakes = true;
    fail_bus_timer_updates = true;
    unsigned rejected_callbacks = 0;
    const auto rejected = request("Ping", [&](const BusReply &) { ++rejected_callbacks; });
    fail_bus_wakes = false;
    fail_bus_timer_updates = false;
    expect_error(rejected, BusError::system);
    EXPECT_EQ(rejected_callbacks, 0U);
    EXPECT_EQ(completions[0], 1U);
    EXPECT_EQ(completions[1], 0U);
    EXPECT_EQ(completions[2], 1U);
    EXPECT_FALSE(bus);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
}

TEST_F(SessionBusTest, FatalSchedulerFailureAfterAReplyUnwindsBeforeOwnerDestruction) {
    ASSERT_TRUE(connect({.dispatch_budget = 1}));
    ASSERT_TRUE(start_service());
    std::array<unsigned, 3> completions{};
    std::array<SessionBus::CallId, 3> ids{};
    for (std::size_t i = 0; i < completions.size(); ++i) {
        auto admitted = request("Hold", [&, i](const BusReply &reply) {
            EXPECT_EQ(reply.error, BusError::system);
            ++completions[i];
            if (i == 0) {
                EXPECT_TRUE(bus->cancel(ids[1]));
            } else if (i == 2) {
                bus.reset();
            }
        });
        ASSERT_TRUE(admitted);
        ids[i] = *admitted;
    }
    ASSERT_TRUE(pump_until([&] { return held.size() == completions.size(); }));
    unsigned pings = 0;
    ASSERT_TRUE(request("Ping", [&](const BusReply &reply) {
        EXPECT_EQ(reply.error, BusError::none);
        ++pings;
        fail_bus_wakes = true;
        fail_bus_timer_updates = true;
    }));
    const bool destroyed = pump_until([&] { return !bus; }, 200ms);
    fail_bus_wakes = false;
    fail_bus_timer_updates = false;
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(pings, 1U);
    EXPECT_EQ(completions[0], 1U);
    EXPECT_EQ(completions[1], 0U);
    EXPECT_EQ(completions[2], 1U);
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
}

} // namespace

extern "C" {
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (fail_bus_wakes && size == sizeof(std::uint64_t)) {
        std::array<char, 64> path{};
        std::array<char, 128> target{};
        std::snprintf(path.data(), path.size(), "/proc/self/fd/%d", fd);
        const auto count = readlink(path.data(), target.data(), target.size() - 1);
        if (count > 0 && std::strcmp(target.data(), "anon_inode:[eventfd]") == 0) {
            errno = EIO;
            return -1;
        }
    }
    return __real_write(fd, data, size);
}
int __real_wl_event_source_timer_update(wl_event_source *, int);
int __wrap_wl_event_source_timer_update(wl_event_source *source, int delay) {
    if (fail_bus_timer_updates) {
        errno = EIO;
        return -1;
    }
    return __real_wl_event_source_timer_update(source, delay);
}
}

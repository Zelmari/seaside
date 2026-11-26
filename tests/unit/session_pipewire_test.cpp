#include "session_pipewire.hpp"
#include "startup_gate.hpp"
#include "test_directory.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-server-core.h>

extern char **environ;

namespace {
std::atomic<bool> fail_next_delivery_update{false};
bool capture_next_io_source = false;
struct IoSource {
    int fd = -1;
    wl_event_loop_fd_func_t dispatch = nullptr;
    void *data = nullptr;
} captured_io;
} // namespace

extern "C" int __real_wl_event_source_timer_update(wl_event_source *, int);
extern "C" int __wrap_wl_event_source_timer_update(wl_event_source *source, int timeout) {
    if (timeout == 1 && fail_next_delivery_update.exchange(false)) {
        errno = ENOMEM;
        return -1;
    }
    return __real_wl_event_source_timer_update(source, timeout);
}

extern "C" wl_event_source *__real_wl_event_loop_add_fd(wl_event_loop *, int, std::uint32_t,
                                                        wl_event_loop_fd_func_t, void *);
extern "C" wl_event_source *__wrap_wl_event_loop_add_fd(wl_event_loop *loop, int fd,
                                                        std::uint32_t mask,
                                                        wl_event_loop_fd_func_t callback,
                                                        void *data) {
    if (capture_next_io_source) {
        capture_next_io_source = false;
        captured_io = {fd, callback, data};
    }
    return __real_wl_event_loop_add_fd(loop, fd, mask, callback, data);
}

namespace {
using namespace std::chrono_literals;
using seaside::PipeWireFailure;
using seaside::PipeWireProbe;
using seaside::PipeWireResult;
using seaside::PipeWireStatus;

class ClientEnvironment {
  public:
    explicit ClientEnvironment(const std::filesystem::path &directory) {
        // Scope both sides of the real exchange: a private server alone would
        // still allow an ambient client.conf/drop-in to load unrelated modules.
        for (auto **item = environ; *item; ++item) {
            const std::string entry(*item);
            const auto equals = entry.find('=');
            const auto key = entry.substr(0, equals);
            if (key.starts_with("PIPEWIRE_") || key.starts_with("SPA_")) {
                saved_.emplace(key, entry.substr(equals + 1));
            }
        }
        for (const auto *key : {"PIPEWIRE_CONFIG_DIR", "PIPEWIRE_CONFIG_NAME", "PIPEWIRE_DAEMON",
                                "PIPEWIRE_RUNTIME_DIR", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME"}) {
            const auto *value = getenv(key);
            saved_.try_emplace(key, value ? std::optional<std::string>(value) : std::nullopt);
        }
        try {
            for (const auto &[key, value] : saved_) {
                (void)value;
                if (unsetenv(key.c_str()) < 0) {
                    throw std::runtime_error("unset fixture PipeWire environment");
                }
            }
            const auto runtime = directory.string();
            const auto config = (directory / "xdg-config").string();
            if (setenv("PIPEWIRE_CONFIG_DIR", runtime.c_str(), 1) < 0 ||
                setenv("PIPEWIRE_CONFIG_NAME", "seaside-probe-client.conf", 1) < 0 ||
                setenv("PIPEWIRE_RUNTIME_DIR", runtime.c_str(), 1) < 0 ||
                setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1) < 0 ||
                setenv("XDG_CONFIG_HOME", config.c_str(), 1) < 0) {
                throw std::runtime_error("set fixture PipeWire environment");
            }
        } catch (...) {
            restore();
            throw;
        }
    }
    ~ClientEnvironment() { restore(); }
    ClientEnvironment(const ClientEnvironment &) = delete;
    ClientEnvironment &operator=(const ClientEnvironment &) = delete;

  private:
    std::map<std::string, std::optional<std::string>> saved_;
    void restore() noexcept {
        for (const auto &[key, value] : saved_) {
            const int result =
                value ? setenv(key.c_str(), value->c_str(), 1) : unsetenv(key.c_str());
            if (result < 0) {
                ADD_FAILURE() << "restore fixture environment: " << key;
            }
        }
    }
};

class PipeWireProbeTest : public testing::Test {
  protected:
    seaside::test::Directory directory;
    std::unique_ptr<ClientEnvironment> client_environment;
    wl_event_loop *loop = nullptr;
    std::unique_ptr<PipeWireProbe> probe;
    pid_t child = -1;
    int listener = -1;
    int peer = -1;
    unsigned completions = 0;
    PipeWireResult completed;

    void SetUp() override {
        RecordProperty("part", "3");
        RecordProperty("requirement", "spec-3.1-pipewire-core-roundtrip");
        ASSERT_TRUE(write_client_config());
        client_environment = std::make_unique<ClientEnvironment>(directory.path());
        loop = wl_event_loop_create();
        ASSERT_NE(loop, nullptr);
    }

    void TearDown() override {
        fail_next_delivery_update = false;
        capture_next_io_source = false;
        captured_io = {};
        probe.reset();
        if (peer >= 0) {
            close(peer);
        }
        if (listener >= 0) {
            close(listener);
        }
        stop_server();
        if (loop) {
            wl_event_loop_destroy(loop);
        }
        client_environment.reset();
    }

    std::string socket() const { return (directory.path() / "seaside-test").string(); }

    bool write_client_config(std::string_view extra_properties = {}, bool audio = false) {
        std::ofstream config(directory.path() / "seaside-probe-client.conf");
        config << "context.properties = { support.dbus = false mem.allow-mlock = false "
               << extra_properties
               << " }\n"
                  "context.spa-libs = { support.* = support/libspa-support }\n"
                  "context.modules = [ { name = libpipewire-module-protocol-native }";
        if (audio) {
            config << " { name = libpipewire-module-metadata }";
        }
        config << " ]\n";
        config.close();
        return static_cast<bool>(config);
    }

    std::string server_log() const {
        std::ifstream stream(directory.path() / "server.log");
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }

    bool start_server(bool audio = false) {
        const auto config = (directory.path() / "server.conf").string();
        std::ofstream output(config);
        // The fixture uses its own PipeWire daemon without access to the
        // machine's actual audio or video devices.
        output << "context.properties = { core.daemon = true core.name = seaside-test "
                  "support.dbus = false mem.allow-mlock = false }\n"
                  "context.spa-libs = { support.* = support/libspa-support "
                  "audio.convert.* = audioconvert/libspa-audioconvert }\n"
                  "context.modules = [ { name = libpipewire-module-protocol-native } "
                  "{ name = libpipewire-module-access args = { access.socket = { "
                  "seaside-test = unrestricted } } }";
        if (audio) {
            output << " { name = libpipewire-module-metadata } "
                      "{ name = libpipewire-module-spa-node-factory } "
                      "{ name = libpipewire-module-adapter }";
        }
        output << " ]\n";
        if (audio) {
            output << "context.objects = [ { factory = metadata args = { metadata.name = default } "
                      "}\n";
            for (const auto *name : {"first", "second"}) {
                output << "{ factory = adapter args = { factory.name = support.null-audio-sink "
                          "media.class = Audio/Sink audio.position = [ FL FR ] node.name = "
                       << name << " } }\n";
            }
            output << "]\n";
        }
        output.close();
        if (!output) {
            return false;
        }
        std::vector<std::string> environment;
        for (auto **item = environ; *item; ++item) {
            std::string value(*item);
            if (!value.starts_with("PIPEWIRE_") && !value.starts_with("SPA_") &&
                !value.starts_with("XDG_RUNTIME_DIR=")) {
                environment.push_back(std::move(value));
            }
        }
        environment.push_back("PIPEWIRE_RUNTIME_DIR=" + directory.path().string());
        environment.push_back("XDG_RUNTIME_DIR=" + directory.path().string());
        environment.push_back("PIPEWIRE_CONFIG_DIR=" + directory.path().string());
        std::vector<char *> env;
        for (auto &value : environment) {
            env.push_back(value.data());
        }
        env.push_back(nullptr);
        std::string executable = "/usr/bin/pipewire";
        std::string flag = "-c";
        std::string argument = config;
        char *arguments[] = {executable.data(), flag.data(), argument.data(), nullptr};
        const int log = open((directory.path() / "server.log").c_str(),
                             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (log < 0) {
            return false;
        }
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            close(log);
            return false;
        }
        const bool redirected =
            posix_spawn_file_actions_adddup2(&actions, log, STDOUT_FILENO) == 0 &&
            posix_spawn_file_actions_adddup2(&actions, log, STDERR_FILENO) == 0 &&
            posix_spawn_file_actions_addclose(&actions, log) == 0;
        const int result = redirected ? posix_spawn(&child, executable.c_str(), &actions, nullptr,
                                                    arguments, env.data())
                                      : EINVAL;
        posix_spawn_file_actions_destroy(&actions);
        close(log);
        if (result != 0) {
            child = -1;
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!std::filesystem::exists(socket())) {
            int status = 0;
            if (waitpid(child, &status, WNOHANG) == child) {
                child = -1;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }

    void stop_server() {
        if (child < 0) {
            return;
        }
        // The exact unreaped child owns its PID until waitpid, including if it
        // already exited. No numeric PID belonging to another process is used.
        kill(child, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        int status = 0;
        for (;;) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD)) {
                child = -1;
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
                child = -1;
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
    }

    bool stall_peer() {
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener < 0) {
            return false;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = socket();
        if (name.size() >= sizeof(address.sun_path)) {
            return false;
        }
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        return bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0 &&
               listen(listener, 4) == 0;
    }

    bool create(std::chrono::milliseconds timeout = 1s) {
        auto result =
            PipeWireProbe::create(loop, {.timeout = timeout, .remote = socket()}, [&](auto value) {
                ++completions;
                completed = value;
            });
        if (!result) {
            ADD_FAILURE() << "PipeWire probe create: " << static_cast<int>(result.error().code)
                          << " " << result.error().system_error;
            return false;
        }
        probe = std::move(*result);
        return true;
    }

    template <class Predicate> bool dispatch_until(Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline ||
                wl_event_loop_dispatch(loop, 10) < 0) {
                return false;
            }
        }
        return true;
    }

    void dispatch_quietly() {
        for (unsigned i = 0; i < 4; ++i) {
            ASSERT_GE(wl_event_loop_dispatch(loop, 5), 0);
        }
    }
};

TEST_F(PipeWireProbeTest, RejectsInvalidArgumentsBeforeConnecting) {
    auto callback = [](auto) {};
    EXPECT_FALSE(PipeWireProbe::create(nullptr, {}, callback));
    EXPECT_FALSE(PipeWireProbe::create(loop, {}, {}));
    EXPECT_FALSE(PipeWireProbe::create(loop, {.timeout = 0ms}, callback));
    EXPECT_FALSE(PipeWireProbe::create(loop, {.timeout = -1ms}, callback));
    EXPECT_FALSE(
        PipeWireProbe::create(loop, {.timeout = std::chrono::milliseconds{INT64_MAX}}, callback));
    EXPECT_FALSE(PipeWireProbe::create(loop, {.remote = std::string("x\0y", 3)}, callback));
}

TEST_F(PipeWireProbeTest, MissingSocketFailsWithoutClaimingRoundtripSuccess) {
    auto result = PipeWireProbe::create(loop, {.remote = socket()}, [&](auto) { ++completions; });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, PipeWireFailure::connection);
    // The native protocol may translate a missing socket to EHOSTDOWN rather
    // than preserving connect(2)'s ENOENT. Preserve a real negative failure.
    EXPECT_LT(result.error().system_error, 0);
    dispatch_quietly();
    EXPECT_EQ(completions, 0U);
}

TEST_F(PipeWireProbeTest, ClientConfigCannotSubstituteAnInProcessCore) {
    for (const auto *properties :
         {"remote.intention = internal", "remote.name = internal",
          "core.daemon = true core.name = seaside-test remote.name = seaside-test"}) {
        SCOPED_TRACE(properties);
        ASSERT_TRUE(write_client_config(properties));
        auto result = PipeWireProbe::create(loop, {}, [&](auto) { ++completions; });
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, PipeWireFailure::connection);
        EXPECT_LT(result.error().system_error, 0);
        dispatch_quietly();
        EXPECT_EQ(completions, 0U);
    }
}

TEST_F(PipeWireProbeTest, DaemonEnvironmentCannotOpenAProbeSocket) {
    ASSERT_EQ(setenv("PIPEWIRE_DAEMON", "true", 1), 0);
    ASSERT_TRUE(write_client_config("core.name = seaside-test remote.name = seaside-test"));
    auto result = PipeWireProbe::create(loop, {}, [&](auto) { ++completions; });
    EXPECT_FALSE(std::filesystem::exists(socket()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, PipeWireFailure::connection);
    EXPECT_LT(result.error().system_error, 0);
    EXPECT_EQ(completions, 0U);
}

TEST_F(PipeWireProbeTest, DefaultProbeUsesConfiguredNativeSocketWithoutPortal) {
    ASSERT_TRUE(start_server()) << server_log();
    ASSERT_TRUE(write_client_config("remote.name = seaside-test remote.intention = screencast"));
    auto result = PipeWireProbe::create(loop, {}, [&](auto value) {
        completed = value;
        ++completions;
    });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; })) << server_log();
    EXPECT_EQ(completed.status, PipeWireStatus::ready);
}

TEST_F(PipeWireProbeTest, RapidAudioEditsAndReconnectRestoreTheOriginalRoute) {
    ASSERT_TRUE(write_client_config({}, true));
    ASSERT_TRUE(start_server(true)) << server_log();
    constexpr auto tool =
        "/usr/bin/timeout 1s /usr/bin/pw-metadata --remote seaside-test --name default "
        "0 default.configured.audio.sink";
    constexpr auto original = R"({"name":"prior-sink","stream":"rear"})";
    const std::string seed = std::string(tool) + " '" + original + "' Spa:String:JSON >/dev/null";
    ASSERT_EQ(std::system(seed.c_str()), 0);
    const auto contains_selection = [&](std::string_view selection) {
        auto *reader = popen(tool, "r");
        if (!reader) {
            return false;
        }
        std::string response;
        std::array<char, 256> buffer{};
        while (fgets(buffer.data(), buffer.size(), reader)) {
            response += buffer.data();
        }
        return pclose(reader) == 0 && response.find(selection) != std::string::npos;
    };
    ASSERT_TRUE(contains_selection(original)) << server_log();
    auto ownership = std::make_shared<seaside::AudioRouteOwnership>();
    const auto connect_audio = [&] {
        auto opened = PipeWireProbe::create(
            loop, {.remote = socket(), .audio_ownership = ownership},
            [&](auto outcome) { EXPECT_EQ(outcome.status, PipeWireStatus::ready); },
            [](auto) { ADD_FAILURE() << "The private PipeWire daemon disconnected"; });
        EXPECT_TRUE(opened);
        if (!opened) {
            return false;
        }
        probe = std::move(*opened);
        return dispatch_until([&] { return probe->audio_route_status(true).metadata_ready; });
    };
    ASSERT_TRUE(connect_audio());
    probe->set_audio_routes("first", std::nullopt);
    ASSERT_TRUE(probe->audio_route_status(true).available) << server_log();
    ASSERT_TRUE(dispatch_until([&] { return contains_selection(R"("name":"first")"); }))
        << server_log();
    // Exercise the edit while the earlier requests and their echoes are
    // still being drained from the private PipeWire connection.
    probe->set_audio_routes("second", std::nullopt);
    ASSERT_GE(wl_event_loop_dispatch(loop, 0), 0);
    probe->set_audio_routes(std::nullopt, std::nullopt);
    ASSERT_TRUE(dispatch_until([&] { return contains_selection(original); }));
    dispatch_quietly();
    EXPECT_TRUE(contains_selection(original));

    probe->set_audio_routes("second", std::nullopt);
    ASSERT_TRUE(dispatch_until([&] { return contains_selection(R"("name":"second")"); }));
    probe.reset();
    ASSERT_TRUE(connect_audio());
    ASSERT_TRUE(dispatch_until([&] { return contains_selection(original); }));
}

TEST_F(PipeWireProbeTest, PrivateRealServerCompletesCoreRoundtrip) {
    ASSERT_TRUE(start_server()) << server_log();
    ASSERT_TRUE(create());
    EXPECT_EQ(probe->result().status, PipeWireStatus::pending);
    EXPECT_EQ(completions, 0U);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; })) << server_log();
    EXPECT_EQ(completed.status, PipeWireStatus::ready);
    EXPECT_EQ(completed.system_error, 0);
    EXPECT_EQ(probe->result().status, PipeWireStatus::ready);
    ASSERT_TRUE(probe->cancel());
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, RetainedCoreReportsRuntimeDisconnectWithoutChangingStartupResult) {
    ASSERT_TRUE(start_server()) << server_log();
    unsigned faults = 0;
    auto result = PipeWireProbe::create(
        loop, {.remote = socket()},
        [&](PipeWireResult value) {
            completed = value;
            ++completions;
        },
        [&](int error) {
            EXPECT_LT(error, 0);
            EXPECT_EQ(probe->result().status, PipeWireStatus::ready);
            ++faults;
            probe.reset();
            EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
        });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    ASSERT_EQ(completed.status, PipeWireStatus::ready);
    EXPECT_EQ(faults, 0U);

    stop_server();
    ASSERT_TRUE(dispatch_until([&] { return faults != 0; }));
    dispatch_quietly();
    EXPECT_EQ(faults, 1U);
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, CancelAfterReadinessSilentlyReleasesRuntimeConnection) {
    ASSERT_TRUE(start_server()) << server_log();
    unsigned faults = 0;
    auto result = PipeWireProbe::create(
        loop, {.remote = socket()},
        [&](PipeWireResult value) {
            completed = value;
            ++completions;
        },
        [&](int) { ++faults; });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    ASSERT_EQ(completed.status, PipeWireStatus::ready);
    ASSERT_TRUE(probe->cancel());

    stop_server();
    dispatch_quietly();
    EXPECT_EQ(faults, 0U);
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(probe->result().status, PipeWireStatus::ready);
}

TEST_F(PipeWireProbeTest, ConnectedButSilentPeerTimesOutWhileWaylandKeepsDispatching) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create(40ms));
    ASSERT_EQ(probe->result().status, PipeWireStatus::pending);
    struct IdleDispatch {
        const PipeWireProbe &probe;
        bool ran_while_pending = false;
    } idle{*probe};
    // Wayland dispatches queued idle work before polling the deadline timer.
    ASSERT_NE(wl_event_loop_add_idle(
                  loop,
                  [](void *data) noexcept {
                      auto &dispatch = *static_cast<IdleDispatch *>(data);
                      dispatch.ran_while_pending =
                          dispatch.probe.result().status == PipeWireStatus::pending;
                  },
                  &idle),
              nullptr);
    ASSERT_GE(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_TRUE(idle.ran_while_pending);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::timed_out);
    EXPECT_EQ(completed.system_error, -ETIMEDOUT);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, PeerDisconnectIsAnAsynchronousFailure) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create());
    peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    ASSERT_GE(peer, 0);
    close(peer);
    peer = -1;
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::failed);
    EXPECT_LT(completed.system_error, 0);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, CancellationAndLatePeerEventsDeliverOnlyOnce) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create());
    peer = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    ASSERT_GE(peer, 0);
    ASSERT_TRUE(probe->cancel());
    ASSERT_TRUE(probe->cancel());
    EXPECT_EQ(completions, 0U);
    EXPECT_EQ(probe->result().status, PipeWireStatus::cancelled);
    close(peer);
    peer = -1;
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::cancelled);
    EXPECT_EQ(completed.system_error, -ECANCELED);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, PendingDestructionSilentlyRemovesSourcesAndListener) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create(10ms));
    probe.reset();
    dispatch_quietly();
    EXPECT_EQ(completions, 0U);
}

TEST_F(PipeWireProbeTest, CompletionCanDestroyItsOwningProbe) {
    ASSERT_TRUE(start_server()) << server_log();
    auto result = PipeWireProbe::create(loop, {.remote = socket()}, [&](auto value) {
        completed = value;
        ++completions;
        probe.reset();
    });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::ready);
    EXPECT_EQ(probe, nullptr);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, ThrowingCompletionDoesNotUnwindThroughWayland) {
    ASSERT_TRUE(stall_peer());
    auto result = PipeWireProbe::create(loop, {.timeout = 10ms, .remote = socket()}, [&](auto) {
        ++completions;
        throw std::runtime_error("fixture completion error");
    });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(probe->result().status, PipeWireStatus::failed);
    EXPECT_TRUE(probe->result().callback_failed);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, WrongThreadCannotCancelPendingProbe) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create());
    std::jthread foreign([&] {
        auto result = probe->cancel();
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, PipeWireFailure::wrong_thread);
    });
    foreign.join();
    EXPECT_EQ(probe->result().status, PipeWireStatus::pending);
    ASSERT_TRUE(probe->cancel());
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::cancelled);
}

TEST_F(PipeWireProbeTest, FailedSuccessDeliverySchedulingCannotReportReadyOrHang) {
    ASSERT_TRUE(start_server()) << server_log();
    ASSERT_TRUE(create());
    fail_next_delivery_update = true;
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_FALSE(fail_next_delivery_update.load());
    EXPECT_EQ(completed.status, PipeWireStatus::failed);
    EXPECT_EQ(completed.system_error, -ENOMEM);
    EXPECT_EQ(probe->result().status, PipeWireStatus::failed);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, FailedTimeoutDeliverySchedulingStillReportsFailure) {
    ASSERT_TRUE(stall_peer());
    ASSERT_TRUE(create(10ms));
    fail_next_delivery_update = true;
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::failed);
    EXPECT_EQ(completed.system_error, -ENOMEM);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, FailedCancelSchedulingMayDestroyProbeDuringSynchronousFallback) {
    ASSERT_TRUE(stall_peer());
    auto result = PipeWireProbe::create(loop, {.remote = socket()}, [&](auto value) {
        completed = value;
        ++completions;
        probe.reset();
    });
    ASSERT_TRUE(result);
    probe = std::move(*result);
    fail_next_delivery_update = true;
    ASSERT_TRUE(probe->cancel());
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(completed.status, PipeWireStatus::failed);
    EXPECT_EQ(completed.system_error, -ENOMEM);
    EXPECT_EQ(probe, nullptr);
    dispatch_quietly();
    EXPECT_EQ(completions, 1U);
}

TEST_F(PipeWireProbeTest, OverdueRealReplyCannotBeatItsUndispatchedDeadlineSource) {
    ASSERT_TRUE(start_server()) << server_log();
    capture_next_io_source = true;
    const auto begun = std::chrono::steady_clock::now();
    ASSERT_TRUE(create(100ms));
    ASSERT_NE(captured_io.dispatch, nullptr);
    // Flush the actual request, then deliberately stop the outer dispatcher.
    // Invoking its captured production I/O callback later fixes callback order:
    // the overdue deadline timer cannot run first and hide the regression.
    ASSERT_EQ(captured_io.dispatch(captured_io.fd, WL_EVENT_READABLE, captured_io.data), 0);
    pollfd ready{captured_io.fd, POLLIN, 0};
    ASSERT_GT(poll(&ready, 1, 1000), 0);
    std::this_thread::sleep_until(begun + 120ms);
    ASSERT_EQ(captured_io.dispatch(captured_io.fd, WL_EVENT_READABLE, captured_io.data), 0);
    EXPECT_EQ(probe->result().status, PipeWireStatus::timed_out);
    EXPECT_EQ(completions, 0U);
    ASSERT_TRUE(dispatch_until([&] { return completions != 0; }));
    EXPECT_EQ(completed.status, PipeWireStatus::timed_out);
    EXPECT_EQ(completed.system_error, -ETIMEDOUT);
}

TEST_F(PipeWireProbeTest, RepeatedRealProbesReleaseTheirFileDescriptors) {
    ASSERT_TRUE(start_server()) << server_log();
    auto count_fds = [] {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto &entry :
             std::filesystem::directory_iterator("/proc/self/fd")) {
            ++count;
        }
        return count;
    };
    ASSERT_TRUE(create());
    ASSERT_TRUE(dispatch_until([&] { return completions == 1; }));
    ASSERT_EQ(completed.status, PipeWireStatus::ready);
    probe.reset();
    const auto baseline = count_fds();
    for (unsigned i = 0; i < 5; ++i) {
        ASSERT_TRUE(create());
        ASSERT_TRUE(dispatch_until([&] { return completions == i + 2; }));
        ASSERT_EQ(completed.status, PipeWireStatus::ready);
        probe.reset();
        EXPECT_EQ(count_fds(), baseline);
    }
}

TEST_F(PipeWireProbeTest, RealCoreRoundtripFeedsGateWithOtherReportsExplicitlySimulated) {
    RecordProperty("requirement", "spec-3.1-mixed-real-pipewire-simulated-other-probes");
    ASSERT_TRUE(start_server()) << server_log();
    unsigned gate_completions = 0;
    bool inside_pipewire_callback = false;
    seaside::StartupResult observed;
    auto created = seaside::StartupGate::create(loop, [&](auto value) {
        EXPECT_FALSE(inside_pipewire_callback);
        ++gate_completions;
        observed = value;
    });
    ASSERT_TRUE(created);
    auto gate = std::move(*created);
    // These three collaborators are simulated; only the private PipeWire core
    // exchange is real. This mixed test cannot establish the G2 session gate.
    ASSERT_TRUE(gate->report(seaside::StartupService::systemd_user));
    ASSERT_TRUE(gate->report(seaside::StartupService::portal_frontend));
    ASSERT_TRUE(gate->report(seaside::StartupService::capture_backend));
    EXPECT_EQ(gate->result().phase, seaside::StartupPhase::checking);
    auto connected = PipeWireProbe::create(loop, {.remote = socket()}, [&](auto value) {
        inside_pipewire_callback = true;
        ASSERT_EQ(value.status, PipeWireStatus::ready);
        ASSERT_TRUE(gate->report(seaside::StartupService::pipewire));
        EXPECT_EQ(gate_completions, 0U);
        inside_pipewire_callback = false;
    });
    ASSERT_TRUE(connected);
    probe = std::move(*connected);
    EXPECT_EQ(gate->result().phase, seaside::StartupPhase::checking);
    EXPECT_EQ(gate_completions, 0U);
    ASSERT_TRUE(dispatch_until([&] { return gate_completions != 0; }));
    EXPECT_EQ(observed.phase, seaside::StartupPhase::ready);
    dispatch_quietly();
    EXPECT_EQ(gate_completions, 1U);
}

TEST_F(PipeWireProbeTest, RealRoundtripTimeoutFailsGateWithOtherReportsExplicitlySimulated) {
    RecordProperty("requirement", "spec-3.1-mixed-real-pipewire-simulated-other-probes");
    ASSERT_TRUE(stall_peer());
    unsigned gate_completions = 0;
    seaside::StartupResult observed;
    auto created = seaside::StartupGate::create(loop, [&](auto value) {
        ++gate_completions;
        observed = value;
    });
    ASSERT_TRUE(created);
    auto gate = std::move(*created);
    ASSERT_TRUE(gate->report(seaside::StartupService::systemd_user));
    ASSERT_TRUE(gate->report(seaside::StartupService::portal_frontend));
    ASSERT_TRUE(gate->report(seaside::StartupService::capture_backend));
    auto connected =
        PipeWireProbe::create(loop, {.timeout = 10ms, .remote = socket()}, [&](auto value) {
            ASSERT_EQ(value.status, PipeWireStatus::timed_out);
            ASSERT_TRUE(gate->report(seaside::StartupService::pipewire,
                                     {seaside::ServiceFailure::timeout, value.system_error}));
            EXPECT_EQ(gate_completions, 0U);
        });
    ASSERT_TRUE(connected);
    probe = std::move(*connected);
    ASSERT_TRUE(dispatch_until([&] { return gate_completions != 0; }));
    EXPECT_EQ(observed.phase, seaside::StartupPhase::failed);
    EXPECT_EQ(observed.service, seaside::StartupService::pipewire);
    EXPECT_EQ(observed.detail.failure, seaside::ServiceFailure::timeout);
    EXPECT_EQ(observed.detail.native_error, -ETIMEDOUT);
    dispatch_quietly();
    EXPECT_EQ(gate_completions, 1U);
}
} // namespace

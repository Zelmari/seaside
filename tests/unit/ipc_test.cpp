#include "ipc.hpp"
#include "ipc_protocol.hpp"
#include "test_directory.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <gtest/gtest.h>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

namespace seaside {
namespace {

std::string request(std::uint64_t id, std::string_view method = "query.status",
                    IpcJson params = IpcJson::object()) {
    return IpcJson{{"v", 1}, {"id", id}, {"method", method}, {"params", std::move(params)}}.dump() +
           '\n';
}

class IpcFixture : public ::testing::Test {
  protected:
    void SetUp() override {
        if (const char *value = std::getenv("XDG_RUNTIME_DIR")) {
            previous_runtime = value;
        }
        ASSERT_EQ(setenv("XDG_RUNTIME_DIR", runtime.path().c_str(), 1), 0);
        ASSERT_NE(loop, nullptr);
        auto created = make_server("wayland-11");
        ASSERT_TRUE(created) << created.error();
        server = std::move(*created);
    }

    void TearDown() override {
        for (const auto &[fd, unread] : clients) {
            close(fd);
        }
        server.reset();
        if (loop) {
            wl_event_loop_destroy(loop);
        }
        if (previous_runtime) {
            EXPECT_EQ(setenv("XDG_RUNTIME_DIR", previous_runtime->c_str(), 1), 0);
        } else {
            EXPECT_EQ(unsetenv("XDG_RUNTIME_DIR"), 0);
        }
    }

    auto make_server(std::string_view display, IpcServer::DeferredHandler deferred = {})
        -> std::expected<std::unique_ptr<IpcServer>, std::string> {
        return IpcServer::create(
            loop, display,
            [this](std::string_view method, const IpcJson &params) -> IpcResult {
                ++calls;
                if (method == "query.windows") {
                    return IpcJson{{"windows", std::string(ipc::record_limit, 'x')}};
                }
                if (method == "action.exit") {
                    return IpcJson{{"accepted", true}, {"changed", true}};
                }
                if (method == "action.workspace_select") {
                    server->event("workspace.changed", {{"workspace_id", params["workspace_id"]}});
                }
                return IpcJson{{"method", method}, {"params", params}};
            },
            [this] { ++exits; }, std::move(deferred));
    }

    int connect_to(std::string_view path = {}) {
        const auto address = ipc::address(path.empty() ? server->path() : path);
        if (!address) {
            ADD_FAILURE() << address.error();
            return -1;
        }
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0 ||
            connect(fd, reinterpret_cast<const sockaddr *>(&*address), sizeof(*address))) {
            ADD_FAILURE() << "Connect to test socket, errno " << errno;
            if (fd >= 0) {
                close(fd);
            }
            return -1;
        }
        clients.emplace(fd, std::string{});
        return fd;
    }

    void send_all(int fd, std::string_view bytes) {
        for (int rounds = 0; !bytes.empty() && rounds < 200; ++rounds) {
            const auto sent = send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            if (sent > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(sent));
            } else if (sent < 0) {
                ASSERT_EQ(errno, EAGAIN);
                ASSERT_EQ(wl_event_loop_dispatch(loop, 1), 0);
            } else {
                FAIL() << "Test socket made no send progress";
            }
        }
        ASSERT_TRUE(bytes.empty());
    }

    IpcJson receive(int fd) {
        auto &wire = clients.at(fd);
        for (int rounds = 0; rounds < 400; ++rounds) {
            if (auto newline = wire.find('\n'); newline != std::string::npos) {
                auto response =
                    IpcJson::parse(std::string_view(wire).substr(0, newline), nullptr, false);
                wire.erase(0, newline + 1);
                return response;
            }
            EXPECT_EQ(wl_event_loop_dispatch(loop, 1), 0);
            std::array<char, 8192> bytes;
            const auto size = recv(fd, bytes.data(), bytes.size(), 0);
            if (size > 0) {
                wire.append(bytes.data(), static_cast<std::size_t>(size));
            } else if (size == 0 || errno != EAGAIN) {
                break;
            }
        }
        ADD_FAILURE() << "IPC record was not returned";
        return IpcJson::object();
    }

    test::Directory runtime;
    std::optional<std::string> previous_runtime;
    wl_event_loop *loop = wl_event_loop_create();
    std::unique_ptr<IpcServer> server;
    std::map<int, std::string> clients;
    int calls = 0;
    int exits = 0;
};

TEST_F(IpcFixture, PrivateSocketNamesSeparateInstancesAndCleanUp) {
    struct stat directory{};
    ASSERT_EQ(stat((runtime.path() / "seaside").c_str(), &directory), 0);
    EXPECT_EQ(directory.st_mode & 0777, 0700);
    struct stat endpoint{};
    ASSERT_EQ(stat(server->path().c_str(), &endpoint), 0);
    EXPECT_EQ(endpoint.st_mode & 0777, 0600);
    EXPECT_FALSE(make_server("wayland-11"));
    auto other = make_server("wayland-12");
    ASSERT_TRUE(other) << other.error();
    const auto other_path = (*other)->path();
    EXPECT_NE(server->path(), other_path);
    const int fd = connect_to(other_path);
    ASSERT_GE(fd, 0);
    send_all(fd, request(1));
    EXPECT_EQ(receive(fd)["ok"], true);
    other->reset();
    EXPECT_FALSE(std::filesystem::exists(other_path));
    EXPECT_FALSE(std::filesystem::exists(other_path + ".lock"));
    EXPECT_TRUE(std::filesystem::exists(server->path()));
}

TEST_F(IpcFixture, RemovesStaleSocketsAndRejectsUnsafeDirectories) {
    const auto path = ipc::runtime_path("wayland-stale");
    ASSERT_TRUE(path);
    const auto address = ipc::address(*path);
    ASSERT_TRUE(address);
    const int stale = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(stale, 0);
    EXPECT_EQ(bind(stale, reinterpret_cast<const sockaddr *>(&*address), sizeof(*address)), 0);
    EXPECT_EQ(close(stale), 0);
    ASSERT_TRUE(std::filesystem::exists(*path));
    auto service = make_server("wayland-stale");
    ASSERT_TRUE(service) << service.error();
    ASSERT_EQ(chmod((runtime.path() / "seaside").c_str(), 0755), 0);
    EXPECT_FALSE(make_server("wayland-unsafe"));
    ASSERT_EQ(chmod((runtime.path() / "seaside").c_str(), 0700), 0);
}

TEST_F(IpcFixture, ReassemblesSplitAndCoalescedRecordsAndRejectsInflightDuplicateIds) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    const auto first = request(10);
    send_all(fd, std::string_view(first).substr(0, 10));
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(calls, 0);
    send_all(fd, first.substr(10) + request(10) + request(11));
    EXPECT_EQ(receive(fd)["ok"], true);
    EXPECT_EQ(receive(fd)["error"]["code"], "invalid_request");
    EXPECT_EQ(receive(fd)["id"], 11);
    EXPECT_EQ(calls, 2);
    send_all(fd, request(10));
    EXPECT_EQ(receive(fd)["ok"], true);
}

TEST_F(IpcFixture, RejectsBadEnvelopesArgumentsAndAbsentMethods) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(fd, "{\"id\":5,\"params\":}\n"
                 "{\"v\":1,\"id\":6,\"method\":\"query.status\",\"params\":{\"x\":1,\"x\":2}}\n"
                 "{\"v\":1,\"id\":7,\"id\":8,\"method\":\"query.status\",\"params\":{}}\n"
                 "{\"v\":1,\"id\":9007199254740992,\"method\":\"query.status\",\"params\":{}}\n"
                 "{\"v\":2,\"id\":9,\"method\":\"query.status\",\"params\":{}}\n" +
                     request(10, "action.exec", {{"shell", "true"}}) +
                     request(11, "action.window_resize", {{"direction", "east"}}) +
                     request(12, "action.workspace_select", {{"workspace_id", true}}) +
                     request(13, "action.workspace_select", {{"workspace_id", 2147483648ULL}}) +
                     request(14, "action.window_resize", {{"delta", 2}}) +
                     request(15, "query.status", {{"unknown", 1}}));
    for (const auto &[id, code] :
         std::array<std::pair<IpcJson, const char *>, 11>{{{5, "invalid_request"},
                                                           {6, "invalid_request"},
                                                           {nullptr, "invalid_request"},
                                                           {nullptr, "invalid_request"},
                                                           {9, "unsupported_version"},
                                                           {10, "unknown_method"},
                                                           {11, "invalid_args"},
                                                           {12, "invalid_args"},
                                                           {13, "invalid_args"},
                                                           {14, "invalid_args"},
                                                           {15, "invalid_args"}}}) {
        auto response = receive(fd);
        EXPECT_EQ(response["id"], id);
        EXPECT_EQ(response["error"]["code"], code);
    }
    EXPECT_EQ(calls, 0);
}

TEST_F(IpcFixture, RejectsRawNulByteOrderMarkAndInvalidUtf8) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    auto nul = request(1);
    nul.insert(nul.size() - 1, "\0ignored", 8);
    send_all(fd, nul + "\xef\xbb\xbf" + request(2) +
                     "{\"id\":3,\"v\":1,\"method\":\"\xff\",\"params\":{}}\n");
    for (int id = 1; id <= 3; ++id) {
        auto response = receive(fd);
        EXPECT_EQ(response["id"], id);
        EXPECT_EQ(response["error"]["code"], "invalid_request");
    }
    EXPECT_EQ(calls, 0);
}

TEST_F(IpcFixture, RejectsExcessNestingAndOversizedRecordsWithBoundedReplies) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    const auto deep =
        "{\"id\":1,\"v\":1,\"method\":\"query.status\",\"params\":{\"x\":" + std::string(16, '[') +
        "0" + std::string(16, ']') + "}}\n";
    send_all(fd, deep);
    const auto nested = receive(fd);
    EXPECT_EQ(nested["id"], 1);
    EXPECT_EQ(nested["error"]["code"], "invalid_request");
    auto maximum = request(2);
    maximum.insert(maximum.size() - 1, ipc::request_limit - maximum.size(), ' ');
    send_all(fd, maximum);
    EXPECT_EQ(receive(fd)["ok"], true);
    send_all(fd, request(3, "query.windows"));
    EXPECT_EQ(receive(fd)["error"]["code"], "response_too_large");
    auto oversized = request(4);
    oversized.resize(ipc::request_limit - 1, ' ');
    oversized[oversized.find('\n')] = ' ';
    send_all(fd, oversized);
    send_all(fd, " ");
    const auto response = receive(fd);
    EXPECT_EQ(response["id"], 4);
    EXPECT_EQ(response["error"]["code"], "invalid_request");
    char byte;
    EXPECT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(recv(fd, &byte, 1, 0), 0);
}

TEST_F(IpcFixture, BoundedPipelineStillMakesProgress) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    std::string burst;
    for (std::uint64_t id = 1; id <= 45; ++id) {
        burst += request(id);
    }
    send_all(fd, burst);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(calls, 32);
    for (std::uint64_t id = 1; id <= 45; ++id) {
        EXPECT_EQ(receive(fd)["id"], id);
    }
    EXPECT_EQ(calls, 45);
}

TEST_F(IpcFixture, QueuesSubscriptionReplyBeforeEventsAndTracksSnapshotWatermark) {
    server->event("workspace.changed", {{"workspace_id", 1}});
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(fd, request(1, "events.subscribe") +
                     request(2, "action.workspace_select", {{"workspace_id", 2}}) + request(3));
    auto subscribed = receive(fd);
    EXPECT_EQ(subscribed["id"], 1);
    EXPECT_EQ(subscribed["seq"], 1);
    EXPECT_EQ(subscribed["result"]["subscribed"], true);
    auto event = receive(fd);
    EXPECT_EQ(event["event"], "workspace.changed");
    EXPECT_EQ(event["seq"], 2);
    EXPECT_EQ(event["data"]["workspace_id"], 2);
    EXPECT_EQ(receive(fd)["id"], 2);
    EXPECT_EQ(receive(fd)["seq"], 2);
}

TEST_F(IpcFixture, HalfClosedSubscriberReceivesLaterEventsAfterQuietPeriod) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(fd, request(1, "events.subscribe"));
    ASSERT_EQ(shutdown(fd, SHUT_WR), 0);
    ASSERT_EQ(receive(fd)["result"]["subscribed"], true);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 1), 0);
    pollfd events{.fd = wl_event_loop_get_fd(loop), .events = POLLIN, .revents = 0};
    EXPECT_EQ(poll(&events, 1, 5), 0);
    server->event("workspace.changed", {{"workspace_id", 2}});
    EXPECT_EQ(receive(fd)["event"], "workspace.changed");
    clients.erase(fd);
    ASSERT_EQ(close(fd), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 1), 0);
    EXPECT_EQ(poll(&events, 1, 5), 0);
}

TEST_F(IpcFixture, DeferredReplyKeepsIdUntilFlushAndUsesCompletionWatermark) {
    std::vector<IpcServer::Completion> pending;
    auto deferred = make_server("wayland-12",
                                [&](std::string_view, const IpcJson &,
                                    IpcServer::Completion finish) -> std::optional<IpcResult> {
                                    pending.push_back(std::move(finish));
                                    return std::nullopt;
                                });
    ASSERT_TRUE(deferred);
    server = std::move(*deferred);
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(fd,
             request(1, "events.subscribe") + request(2, "trace.export") + request(2) + request(3));
    EXPECT_EQ(receive(fd)["id"], 1);
    EXPECT_EQ(receive(fd)["error"]["code"], "invalid_request");
    EXPECT_EQ(receive(fd)["id"], 3);
    ASSERT_EQ(pending.size(), 1);
    server->event("workspace.changed", {{"workspace_id", 2}});
    pending[0](IpcJson{{"path", "/state/trace.jsonl"}});
    EXPECT_EQ(receive(fd)["event"], "workspace.changed");
    const auto completion = receive(fd);
    EXPECT_EQ(completion["id"], 2);
    EXPECT_EQ(completion["seq"], 1);
    EXPECT_EQ(completion["result"]["path"], "/state/trace.jsonl");
    send_all(fd, request(2, "trace.export"));
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    ASSERT_EQ(pending.size(), 2);
    pending[0](IpcJson{{"path", "/state/stale.jsonl"}});
    pending[1](std::unexpected(IpcError{"internal_error", "export did not finish"}));
    EXPECT_EQ(receive(fd)["error"]["message"], "export did not finish");
}

TEST_F(IpcFixture, SettingsUpdatesValidateTypesAndWaitForTheCommitEvent) {
    IpcServer::Completion completion;
    auto deferred = make_server("wayland-12",
                                [&](std::string_view method, const IpcJson &params,
                                    IpcServer::Completion finish) -> std::optional<IpcResult> {
                                    EXPECT_EQ(method, "action.settings_update");
                                    const auto edit = ipc::decode_setting_edit(params);
                                    EXPECT_TRUE(edit);
                                    if (edit) {
                                        EXPECT_EQ(edit->setting, Setting::night_light_enabled);
                                        EXPECT_EQ(edit->revision, 7);
                                        EXPECT_EQ(std::get<NightLightAction>(edit->value),
                                                  NightLightAction::toggle);
                                    }
                                    completion = std::move(finish);
                                    return std::nullopt;
                                });
    ASSERT_TRUE(deferred);
    server = std::move(*deferred);
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(
        fd,
        request(1, "events.subscribe") +
            request(2, "action.settings_update",
                    {{"setting", "idle.display_off_after_minutes"}, {"value", true}}) +
            request(3, "action.settings_update",
                    {{"setting", "night_light.enabled"}, {"value", "toggle"}, {"revision", 7}}));
    EXPECT_EQ(receive(fd)["id"], 1);
    EXPECT_EQ(receive(fd)["error"]["code"], "invalid_args");
    ASSERT_TRUE(completion);
    server->event("settings.changed", {{"revision", 8}});
    completion(IpcJson{{"accepted", true}, {"revision", 8}});
    EXPECT_EQ(receive(fd)["event"], "settings.changed");
    EXPECT_EQ(receive(fd)["result"]["revision"], 8);
}

TEST_F(IpcFixture, DeferredPipelineStaysBoundedWithoutDisconnectingOrStarvingOtherPeers) {
    std::vector<IpcServer::Completion> pending;
    auto deferred = make_server("wayland-12",
                                [&](std::string_view, const IpcJson &,
                                    IpcServer::Completion finish) -> std::optional<IpcResult> {
                                    pending.push_back(std::move(finish));
                                    return std::nullopt;
                                });
    ASSERT_TRUE(deferred);
    server = std::move(*deferred);
    const int fd = connect_to();
    const int other = connect_to();
    ASSERT_GE(fd, 0);
    ASSERT_GE(other, 0);
    std::string burst;
    for (std::uint64_t id = 1; id <= 33; ++id) {
        burst += request(id, "trace.export");
    }
    send_all(fd, burst);
    send_all(other, request(99));
    EXPECT_EQ(receive(other)["id"], 99);
    ASSERT_EQ(pending.size(), 32);
    ASSERT_EQ(shutdown(fd, SHUT_WR), 0);
    pending[0](IpcJson{{"path", "/state/first.jsonl"}});
    EXPECT_EQ(receive(fd)["id"], 1);
    ASSERT_EQ(pending.size(), 33);
    pending[32](IpcJson{{"path", "/state/last.jsonl"}});
    EXPECT_EQ(receive(fd)["id"], 33);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    pending[1](IpcJson{{"path", "/state/half-closed.jsonl"}});
    EXPECT_EQ(receive(fd)["id"], 2);
    clients.erase(fd);
    ASSERT_EQ(close(fd), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    pending[2](IpcJson{{"path", "/state/disconnected.jsonl"}});
    server.reset();
    pending[3](IpcJson{{"path", "/state/stopped.jsonl"}});
}

TEST_F(IpcFixture, DisconnectsSlowSubscribersAndAllowsRecoveryOnAnotherConnection) {
    const int many = connect_to();
    const int large = connect_to();
    const int healthy = connect_to();
    ASSERT_GE(many, 0);
    ASSERT_GE(large, 0);
    ASSERT_GE(healthy, 0);
    send_all(many, request(1, "events.subscribe"));
    EXPECT_EQ(receive(many)["ok"], true);
    for (int count = 0; count <= 256; ++count) {
        server->event("workspace.changed", {{"workspace_id", 3}});
    }
    char byte;
    EXPECT_EQ(recv(many, &byte, 1, 0), 0);
    send_all(large, request(1, "events.subscribe"));
    EXPECT_EQ(receive(large)["seq"], 257);
    for (int count = 0; count < 2; ++count) {
        server->event("config.error", {{"message", std::string(600 * 1024, 'x')}});
    }
    EXPECT_EQ(recv(large, &byte, 1, 0), 0);
    send_all(healthy, request(1));
    EXPECT_EQ(receive(healthy)["seq"], 259);
}

TEST_F(IpcFixture, FlushesExitAcknowledgementBeforeRequestingShutdown) {
    const int fd = connect_to();
    ASSERT_GE(fd, 0);
    send_all(fd, request(1, "action.exit"));
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    ASSERT_EQ(wl_event_loop_dispatch(loop, 0), 0);
    EXPECT_EQ(exits, 0);
    const auto response = receive(fd);
    EXPECT_EQ(response["result"]["accepted"], true);
    EXPECT_EQ(exits, 1);
}

} // namespace
} // namespace seaside

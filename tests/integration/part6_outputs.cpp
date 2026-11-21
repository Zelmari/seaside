#include "ipc_protocol.hpp"
#include "part3_bridge.h"
#include "server.hpp"
#include "session_control.hpp"
#include "test_directory.hpp"
#include "wlroots_bridge.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

// Supply deterministic results at the DRM boundary. OutputConfig, LiveConfig,
// the scene, and the IPC state projection are the production implementations.
struct seaside_tty_drm {
    std::map<wlr_output *, seaside_drm_output_status> state;
    std::vector<std::pair<wlr_output *, float>> commits;
    std::vector<wlr_output *> rejected;
    wlr_output *reject_change = nullptr;
    wlr_output *reject_restore = nullptr;
};

extern "C" {
bool seaside_tty_drm_output_status(seaside_tty_drm *drm, wlr_output *output,
                                   seaside_drm_output_status *status) {
    *status = drm->state.at(output);
    return true;
}

bool seaside_tty_drm_select_mode(wlr_output *, seaside_drm_output_settings *settings) {
    settings->width = 160;
    settings->height = 90;
    settings->refresh_millihz = 60000;
    return true;
}

bool seaside_tty_drm_check_settings(wlr_output *, const seaside_drm_output_settings *) {
    return true;
}

bool seaside_tty_drm_apply_settings(seaside_tty_drm *drm, wlr_output *output,
                                    const seaside_drm_output_settings *settings) {
    auto &current = drm->state.at(output);
    drm->commits.emplace_back(output, settings->scale);
    if ((output == drm->reject_change && settings->scale == 2) ||
        (output == drm->reject_restore && settings->scale == 1)) {
        return false;
    }
    current.settings = *settings;
    current.logical_width = settings->scale == 2 ? 80 : 160;
    current.logical_height = settings->scale == 2 ? 45 : 90;
    return true;
}

void seaside_tty_drm_set_direct_scanout(seaside_tty_drm *, wlr_output *, bool) {}
void seaside_tty_drm_reconcile_cursor(seaside_tty_drm *) {}

void seaside_tty_drm_reject_unrestorable(seaside_tty_drm *drm, wlr_output *output) {
    drm->rejected.push_back(output);
}
}

namespace seaside {
namespace {
using namespace std::chrono_literals;
using FixtureOutput = std::unique_ptr<part3_output, decltype(&part3_output_destroy)>;

class OutputTrial : public testing::Test {
  protected:
    void SetUp() override {
        auto created_server = Server::create();
        ASSERT_TRUE(created_server);
        server = std::move(*created_server);
        auto *display = server->display();
        loop = wl_display_get_event_loop(display);
        auto created_scene = Scene::create(display);
        ASSERT_TRUE(created_scene);
        scene = std::move(*created_scene);
        auto *backend = part3_headless_backend_create(display);
        ASSERT_NE(backend, nullptr);
        auto *renderer = part3_pixman_renderer_create();
        if (!renderer) {
            seaside_wlr_backend_destroy(backend);
            FAIL() << "Cannot create Pixman fixture renderer";
        }
        ASSERT_TRUE(server->start(backend, renderer));
        desktop = std::make_unique<Desktop>(*scene, default_config());
        outputs = std::make_unique<OutputConfig>(&drm, *desktop);
        auto live_config = LiveConfig::create(
            loop, store, paths, load_startup_config(store, paths), {},
            [this](const Config &, const Config &candidate, const LiveConfig::Prepared &) {
                return outputs->apply(candidate);
            },
            {});
        ASSERT_TRUE(live_config) << live_config.error();
        live = std::move(*live_config);
        live->session_ready();
        ASSERT_TRUE(wait([this] { return live->transaction().recovery_persisted(); }));
    }

    FixtureOutput make_output() {
        return {part3_output_create(server->display(), server->backend(), server->allocator(),
                                    server->renderer()),
                part3_output_destroy};
    }

    void attach(part3_output *handle, std::string_view name, int x) {
        auto *output = part3_output_handle(handle);
        seaside_drm_output_status status{};
        status.settings = {nullptr, 160, 90, 60000, x, 0, 0, 1.0F, false};
        status.logical_width = 160;
        status.logical_height = 90;
        status.cursor = true;
        drm.state.insert_or_assign(output, status);
        outputs->observe(output, name, {}, true, nullptr);
        outputs->enabled(name, true);
        ASSERT_TRUE(desktop->add_output(output, name, {}, {x, 0, 160, 90}));
    }

    bool wait(const std::function<bool()> &condition) {
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            if (wl_event_loop_dispatch(loop, 20) < 0) {
                return false;
            }
        }
        return condition();
    }

    void replace(std::string_view config) {
        const auto temporary = paths.user.parent_path() / "editor.tmp";
        ASSERT_TRUE(std::ofstream(temporary) << config);
        std::filesystem::rename(temporary, paths.user);
    }

    test::Directory directory;
    ConfigPaths paths{directory.path() / "config/seaside/config.toml",
                      directory.path() / "state/seaside/last-good-config.toml"};
    ConfigStore store;
    std::unique_ptr<Server> server;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<Desktop> desktop;
    seaside_tty_drm drm;
    std::unique_ptr<OutputConfig> outputs;
    std::unique_ptr<LiveConfig> live;
    wl_event_loop *loop = nullptr;
};

TEST_F(OutputTrial, RestoresAnEarlierOutputWhenASecondCommitFails) {
    auto left = make_output();
    auto right = make_output();
    ASSERT_TRUE(left && right);
    ASSERT_NO_FATAL_FAILURE(attach(left.get(), "left", 0));
    ASSERT_NO_FATAL_FAILURE(attach(right.get(), "right", 160));
    auto *left_handle = part3_output_handle(left.get());
    auto *right_handle = part3_output_handle(right.get());
    drm.reject_change = right_handle;

    ASSERT_NO_FATAL_FAILURE(replace("[monitors]\nscale=2.0\n"));
    ASSERT_TRUE(wait([this] { return live->transaction().diagnostic().has_value(); }));
    EXPECT_EQ(live->transaction().diagnostic()->field, "monitors");
    const std::vector<std::pair<wlr_output *, float>> expected{
        {left_handle, 2}, {right_handle, 2}, {right_handle, 1}, {left_handle, 1}};
    EXPECT_EQ(drm.commits, expected);
    EXPECT_TRUE(drm.rejected.empty());
    EXPECT_EQ(drm.state.at(left_handle).settings.scale, 1);
    EXPECT_EQ(drm.state.at(right_handle).settings.x, 160);
    EXPECT_EQ(desktop->output_bounds(left_handle), (WindowRect{0, 0, 160, 90}));
    EXPECT_EQ(desktop->output_bounds(right_handle), (WindowRect{160, 0, 160, 90}));
    EXPECT_EQ(live->transaction().active().monitors.defaults.scale, 1);
    auto last_good = store.read(paths.recovery, true);
    ASSERT_TRUE(last_good && *last_good);
    EXPECT_EQ(**last_good, default_template());
}

TEST_F(OutputTrial, MarksAnOutputUnusableWhenItsRollbackFails) {
    auto left = make_output();
    auto right = make_output();
    ASSERT_TRUE(left && right);
    ASSERT_NO_FATAL_FAILURE(attach(left.get(), "left", 0));
    ASSERT_NO_FATAL_FAILURE(attach(right.get(), "right", 160));
    auto *left_handle = part3_output_handle(left.get());
    drm.reject_change = part3_output_handle(right.get());
    drm.reject_restore = left_handle;
    ASSERT_NO_FATAL_FAILURE(replace("[monitors]\nscale=2.0\n"));
    ASSERT_TRUE(wait([this] { return live->transaction().diagnostic().has_value(); }));
    EXPECT_EQ(drm.rejected, std::vector{left_handle});
    EXPECT_EQ(live->transaction().active().monitors.defaults.scale, 1);
}

TEST_F(OutputTrial, ReportsNoUsableOutputsAndRecoversTheSameOutputAndWorkspace) {
    auto output = make_output();
    ASSERT_TRUE(output);
    ASSERT_NO_FATAL_FAILURE(attach(output.get(), "monitor", 0));
    SessionControl control(*scene, *desktop, *outputs);
    const char *runtime_env = std::getenv("XDG_RUNTIME_DIR");
    const auto runtime = runtime_env ? std::optional<std::string>(runtime_env) : std::nullopt;
    ASSERT_EQ(setenv("XDG_RUNTIME_DIR", directory.path().c_str(), 1), 0);
    auto ipc_server = IpcServer::create(
        loop, "wayland-6",
        [&](std::string_view method, const IpcJson &) {
            return control.query(method, *live, paths.user.string(), "ready", nullptr);
        },
        [] {});
    ASSERT_EQ(
        runtime ? setenv("XDG_RUNTIME_DIR", runtime->c_str(), 1) : unsetenv("XDG_RUNTIME_DIR"), 0);
    ASSERT_TRUE(ipc_server) << ipc_server.error();
    const auto address = ipc::address((*ipc_server)->path());
    ASSERT_TRUE(address);
    const auto query = [&](std::string_view method) -> IpcJson {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const auto request =
            IpcJson{{"v", 1}, {"id", 1}, {"method", method}, {"params", IpcJson::object()}}.dump() +
            '\n';
        if (fd < 0 ||
            connect(fd, reinterpret_cast<const sockaddr *>(&*address), sizeof(*address)) ||
            send(fd, request.data(), request.size(), MSG_NOSIGNAL) !=
                static_cast<ssize_t>(request.size())) {
            if (fd >= 0) {
                close(fd);
            }
            ADD_FAILURE() << "Could not send test query " << method;
            return IpcJson::object();
        }
        std::string reply;
        const bool complete = wait([&] {
            std::array<char, 4096> buffer{};
            const auto received = recv(fd, buffer.data(), buffer.size(), 0);
            if (received > 0) {
                reply.append(buffer.data(), static_cast<std::size_t>(received));
            }
            return reply.ends_with('\n');
        });
        close(fd);
        if (!complete) {
            ADD_FAILURE() << "Timed out reading " << method;
            return IpcJson::object();
        }
        auto response = IpcJson::parse(reply);
        EXPECT_EQ(response["ok"], true);
        return response.value("result", IpcJson::object());
    };
    const auto first = query("query.outputs")["outputs"].at(0);
    const auto workspace = query("query.focus")["workspace_id"];
    EXPECT_TRUE(first["connected"].get<bool>());
    EXPECT_TRUE(workspace.is_number_unsigned());

    auto *handle = part3_output_handle(output.get());
    desktop->remove_output(handle);
    outputs->observe(handle, "monitor", {}, false, nullptr);
    output.reset();
    const auto disconnected = query("query.outputs")["outputs"].at(0);
    EXPECT_EQ(disconnected["id"], first["id"]);
    EXPECT_EQ(disconnected["connected"], false);
    EXPECT_EQ(disconnected["enabled"], false);
    EXPECT_TRUE(query("query.focus")["output_id"].is_null());
    const auto remembered = query("query.workspaces")["workspaces"].at(0);
    EXPECT_EQ(remembered["owner_output_id"], first["id"]);
    EXPECT_TRUE(remembered["output_id"].is_null());
    EXPECT_EQ(remembered["visible"], false);

    output = make_output();
    ASSERT_TRUE(output);
    ASSERT_NO_FATAL_FAILURE(attach(output.get(), "monitor", 0));
    EXPECT_EQ(query("query.outputs")["outputs"].at(0), first);
    EXPECT_EQ(query("query.focus")["workspace_id"], workspace);
}

} // namespace
} // namespace seaside

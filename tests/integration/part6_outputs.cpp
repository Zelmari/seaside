#include "ipc_protocol.hpp"
#include "part3_bridge.h"
#include "server.hpp"
#include "session_control.hpp"
#include "test_directory.hpp"
#include "wlroots_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <stdexcept>
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
    wlr_output *reject_adaptive_sync = nullptr;
};

namespace {
std::map<wlr_output *, seaside_drm_output_description> descriptions;
constexpr seaside_drm_output_description standard_display{.make = "Seaside",
                                                          .model = "Fixture",
                                                          .preferred_width = 160,
                                                          .preferred_height = 90,
                                                          .max_refresh_millihz = 60000,
                                                          .max_pixels = 160 * 90,
                                                          .adaptive_sync_supported = false};
} // namespace

extern "C" {
seaside_drm_output_description seaside_tty_drm_describe_output(wlr_output *output) {
    return descriptions.at(output);
}

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
        (output == drm->reject_restore && settings->scale == 1) ||
        (output == drm->reject_adaptive_sync && settings->adaptive_sync)) {
        return false;
    }
    current.settings = *settings;
    const bool rotated = settings->rotation == 90 || settings->rotation == 270;
    current.logical_width = static_cast<int>(
        static_cast<float>(rotated ? settings->height : settings->width) / settings->scale);
    current.logical_height = static_cast<int>(
        static_cast<float>(rotated ? settings->width : settings->height) / settings->scale);
    current.adaptive_sync = settings->adaptive_sync;
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

    void TearDown() override { descriptions.clear(); }

    FixtureOutput make_output() {
        return {part3_output_create(server->display(), server->backend(), server->allocator(),
                                    server->renderer()),
                part3_output_destroy};
    }

    void attach(part3_output *handle, std::string_view name, int x, std::string_view identity = {},
                seaside_drm_output_description description = standard_display) {
        auto *output = part3_output_handle(handle);
        seaside_drm_output_status status{};
        status.settings = {nullptr, 160, 90, 60000, x, 0, 0, 1.0F, false};
        status.logical_width = 160;
        status.logical_height = 90;
        status.cursor = true;
        drm.state.insert_or_assign(output, status);
        descriptions.insert_or_assign(output, description);
        outputs->observe(output, name, identity, true, nullptr);
        const auto *record = outputs->enabled(output, true);
        ASSERT_NE(record, nullptr);
        ASSERT_TRUE(desktop->add_output(output, name, identity, {x, 0, 160, 90}, record->id));
    }

    void detach(FixtureOutput &handle, std::string_view connector) {
        auto *output = part3_output_handle(handle.get());
        outputs->enabled(output, false);
        desktop->remove_output(output, outputs->ranked_fallback());
        outputs->observe(output, connector, {}, false, nullptr);
        descriptions.erase(output);
        handle.reset();
    }

    const OutputConfig::Output &connected(std::string_view connector) const {
        auto found = std::ranges::find_if(outputs->outputs(), [&](const auto &row) {
            return row.second.handle && row.second.connector == connector;
        });
        if (found == outputs->outputs().end()) {
            throw std::runtime_error("Fixture output is disconnected");
        }
        return found->second;
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

    detach(output, "monitor");
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

TEST_F(OutputTrial, RanksAdvertisedModesAndKeepsTheAutomaticPrimaryStable) {
    auto first = make_output();
    auto smaller = make_output();
    auto larger = make_output();
    auto newcomer = make_output();
    ASSERT_TRUE(first && smaller && larger && newcomer);
    auto small_mode = standard_display;
    small_mode.max_refresh_millihz = 144000;
    auto large_mode = small_mode;
    large_mode.max_pixels = 1920 * 1080;
    auto fastest_mode = standard_display;
    fastest_mode.max_refresh_millihz = 240000;
    ASSERT_NO_FATAL_FAILURE(attach(first.get(), "a-first", 0));
    ASSERT_NO_FATAL_FAILURE(attach(smaller.get(), "b-smaller", 160, {}, small_mode));
    ASSERT_NO_FATAL_FAILURE(attach(larger.get(), "c-larger", 320, {}, large_mode));

    auto config = default_config();
    ASSERT_TRUE(outputs->apply(config));
    ASSERT_NE(outputs->primary(), nullptr);
    EXPECT_EQ(outputs->primary()->connector, "c-larger");
    EXPECT_EQ(outputs->primary()->status.settings.x, 0);
    EXPECT_EQ(connected("b-smaller").status.settings.x, 160);

    ASSERT_NO_FATAL_FAILURE(attach(newcomer.get(), "d-newcomer", 480, {}, fastest_mode));
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(outputs->primary()->connector, "c-larger");
    EXPECT_EQ(connected("d-newcomer").status.settings.x, 160);

    // The first display has focus history. Migration uses that history when
    // the primary disappears, even though another display ranks higher.
    detach(larger, "c-larger");
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(outputs->primary()->connector, "a-first");
    config.monitors.primary = OutputSelector{SelectorKind::connector, "b-smaller"};
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(outputs->primary()->connector, "b-smaller");
    config.monitors.primary.reset();
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(outputs->primary()->connector, "d-newcomer");
}

TEST_F(OutputTrial, RestoresThePhysicalMonitorAndWorkspaceAcrossConnectorChanges) {
    auto left = make_output();
    auto right = make_output();
    ASSERT_TRUE(left && right);
    auto config = default_config();
    config.workspaces.mode = WorkspaceMode::per_monitor;
    desktop->apply_initial_config(config);
    ASSERT_NO_FATAL_FAILURE(attach(left.get(), "left", 0, "panel-a"));
    ASSERT_NO_FATAL_FAILURE(attach(right.get(), "right", 160, "panel-b"));
    const auto left_id = connected("left").id;
    const auto right_id = connected("right").id;
    const auto &windows = desktop->windows();
    const auto workspace = windows.state().output(right_id)->shown;
    ASSERT_TRUE(workspace);

    detach(right, "right");
    EXPECT_EQ(windows.workspace_output(*workspace), left_id);
    EXPECT_EQ(windows.workspace_owner(*workspace), right_id);
    right = make_output();
    ASSERT_TRUE(right);
    ASSERT_NO_FATAL_FAILURE(attach(right.get(), "other-connector", 160, "panel-b"));
    EXPECT_EQ(connected("other-connector").id, right_id);
    EXPECT_EQ(windows.workspace_output(*workspace), right_id);
    EXPECT_EQ(windows.state().output(right_id)->shown, workspace);
    EXPECT_EQ(windows.focused_output(), left_id);
    EXPECT_EQ(outputs->outputs().size(), 2);
}

TEST_F(OutputTrial, UsesModelAndModeHistoryAndKeepsDuplicateSerialsDistinct) {
    auto first = make_output();
    auto second = make_output();
    ASSERT_TRUE(first && second);
    ASSERT_NO_FATAL_FAILURE(attach(first.get(), "left", 0));
    const auto id = connected("left").id;
    detach(first, "left");
    first = make_output();
    ASSERT_TRUE(first);
    ASSERT_NO_FATAL_FAILURE(attach(first.get(), "new-left", 0));
    EXPECT_EQ(connected("new-left").id, id);

    auto different = standard_display;
    different.model = "Other Display";
    detach(first, "new-left");
    first = make_output();
    ASSERT_TRUE(first);
    ASSERT_NO_FATAL_FAILURE(attach(first.get(), "new-left", 0, "duplicate", different));
    const auto first_duplicate = connected("new-left").id;
    EXPECT_NE(first_duplicate, id);
    ASSERT_NO_FATAL_FAILURE(attach(second.get(), "right", 160, "duplicate", different));
    const auto second_duplicate = connected("right").id;
    EXPECT_NE(second_duplicate, first_duplicate);

    auto config = default_config();
    config.monitors.primary = OutputSelector{SelectorKind::identity, "duplicate"};
    const auto invalid = outputs->apply(config);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().field, "monitors.primary");

    detach(second, "right");
    second = make_output();
    ASSERT_TRUE(second);
    ASSERT_NO_FATAL_FAILURE(attach(second.get(), "right", 160, "duplicate", different));
    EXPECT_EQ(connected("right").id, second_duplicate);
    EXPECT_EQ(outputs->outputs().size(), 3);
}

TEST_F(OutputTrial, PlacesFractionalRotatedOutputsUsingTransformedDimensions) {
    auto left = make_output();
    auto right = make_output();
    ASSERT_TRUE(left && right);
    ASSERT_NO_FATAL_FAILURE(attach(left.get(), "left", 0));
    ASSERT_NO_FATAL_FAILURE(attach(right.get(), "right", 160));
    auto config = default_config();
    auto &settings = config.monitors.defaults;
    settings.scale = 1.25;
    settings.rotation = 90;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(desktop->output_bounds(part3_output_handle(left.get())), (WindowRect{0, 0, 72, 128}));
    EXPECT_EQ(desktop->output_bounds(part3_output_handle(right.get())),
              (WindowRect{72, 0, 72, 128}));

    settings.scale = 1.5;
    settings.rotation = 180;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(desktop->output_bounds(part3_output_handle(left.get())), (WindowRect{0, 0, 106, 60}));
    EXPECT_EQ(desktop->output_bounds(part3_output_handle(right.get())),
              (WindowRect{106, 0, 106, 60}));

    settings.rotation = 270;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(desktop->output_bounds(part3_output_handle(right.get())),
              (WindowRect{60, 0, 60, 106}));
}

TEST_F(OutputTrial, KeepsUsableOutputsWhenAdaptiveSyncIsUnsupportedOrRejected) {
    auto capable = make_output();
    auto fixed = make_output();
    ASSERT_TRUE(capable && fixed);
    auto description = standard_display;
    description.adaptive_sync_supported = true;
    ASSERT_NO_FATAL_FAILURE(attach(capable.get(), "capable", 0, {}, description));
    ASSERT_NO_FATAL_FAILURE(attach(fixed.get(), "fixed", 160));
    auto config = default_config();
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_EQ(connected("capable").adaptive_sync_supported, true);
    EXPECT_EQ(connected("fixed").adaptive_sync_supported, false);
    EXPECT_FALSE(connected("capable").status.adaptive_sync);

    config.render.adaptive_sync = AdaptiveSync::always;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_TRUE(connected("capable").status.adaptive_sync);
    EXPECT_FALSE(connected("fixed").status.adaptive_sync);

    config.render.adaptive_sync = AdaptiveSync::fullscreen;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_FALSE(connected("capable").status.adaptive_sync);

    drm.reject_adaptive_sync = part3_output_handle(capable.get());
    config.render.adaptive_sync = AdaptiveSync::always;
    config.monitors.defaults.scale = 2;
    ASSERT_TRUE(outputs->apply(config));
    EXPECT_TRUE(connected("capable").adaptive_sync_rejected);
    EXPECT_TRUE(connected("capable").enabled);
    EXPECT_FALSE(connected("capable").status.adaptive_sync);
    EXPECT_EQ(connected("capable").status.settings.scale, 2);
    EXPECT_FALSE(connected("fixed").adaptive_sync_rejected);
}

} // namespace
} // namespace seaside

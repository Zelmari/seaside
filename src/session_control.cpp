#include "session_control.hpp"

#include <algorithm>
#include <cstring>

namespace seaside {
namespace {
using Json = IpcJson;

template <typename T> Json nullable(const std::optional<T> &value) {
    return value ? Json(*value) : Json(nullptr);
}

Json capability(std::optional<bool> supported, bool applied, const char *reason) {
    return {{"supported", nullable(supported)},
            {"applied", applied},
            {"reason", reason ? Json(reason) : Json(nullptr)}};
}

Json capability(seaside_output_support supported, bool applied, const char *reason) {
    const auto known = supported == SEASIDE_OUTPUT_UNKNOWN
                           ? std::nullopt
                           : std::optional{supported == SEASIDE_OUTPUT_SUPPORTED};
    return capability(known, applied, reason);
}

std::string bounded_utf8(const char *value, std::size_t limit) {
    if (!value) {
        return {};
    }
    const std::string_view input(value, strnlen(value, limit));
    std::string result;
    result.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto lead = static_cast<unsigned char>(input[i]);
        std::size_t count = 0;
        if (lead < 0x80) {
            count = 1;
        } else if (lead >= 0xc2 && lead <= 0xdf) {
            count = 2;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            count = 3;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            count = 4;
        }
        if (count > input.size() - i) {
            break;
        }
        bool valid = count != 0;
        for (std::size_t j = 1; j < count && valid; ++j) {
            const auto byte = static_cast<unsigned char>(input[i + j]);
            valid = byte >= 0x80 && byte <= 0xbf;
            if (j == 1) {
                valid = valid && (lead != 0xe0 || byte >= 0xa0) && (lead != 0xed || byte < 0xa0) &&
                        (lead != 0xf0 || byte >= 0x90) && (lead != 0xf4 || byte < 0x90);
            }
        }
        if (valid && count <= limit - result.size()) {
            result.append(input.substr(i, count));
            i += count;
        } else {
            if (limit - result.size() < 3) {
                break;
            }
            result.append("\xef\xbf\xbd");
            ++i;
        }
    }
    return result;
}

std::string_view workspace_mode(WorkspaceMode mode) {
    return mode == WorkspaceMode::global ? "global" : "per-monitor";
}

Json config_status(const LiveConfig &config, std::string_view path) {
    const auto &transaction = config.transaction();
    const auto &active = transaction.active();
    constexpr std::string_view sources[] = {"user", "last_good", "defaults"};
    return {{"path", path},
            {"source", sources[static_cast<unsigned>(config.source())]},
            {"active_layout", active.window.layout == Layout::dwindle ? "dwindle" : "scrolling"},
            {"active_tiling", active.window.tiling},
            {"active_workspace_mode", workspace_mode(active.workspaces.mode)},
            {"requested_workspace_mode", workspace_mode(transaction.requested().workspaces.mode)},
            {"pending_restart", transaction.restart_pending()},
            {"recovery_persisted", transaction.recovery_persisted()},
            {"error",
             transaction.diagnostic() ? ipc_diagnostic(*transaction.diagnostic()) : Json(nullptr)}};
}

Json bindings(const BindingMap &active) {
    auto entries = Json::array();
    for (const auto &[chord, action] : active) {
        entries.push_back({{"chord", format_chord(chord)}, {"action", format_action(action)}});
    }
    return {{"bindings", std::move(entries)}};
}
} // namespace

IpcJson ipc_diagnostic(const Diagnostic &diagnostic, std::string_view component) {
    return {{"component", component},
            {"message", diagnostic.message},
            {"path", diagnostic.path.empty() ? Json(nullptr) : Json(diagnostic.path)},
            {"line", diagnostic.line ? Json(diagnostic.line) : Json(nullptr)},
            {"field", diagnostic.field.empty() ? Json(nullptr) : Json(diagnostic.field)}};
}

IpcJson ipc_action_result(bool changed) {
    return {{"accepted", true}, {"changed", changed}};
}

IpcResult desktop_action_result(std::expected<bool, DesktopActionError> result) {
    if (result) {
        return ipc_action_result(*result);
    }
    switch (result.error()) {
    case DesktopActionError::no_target:
        return std::unexpected(IpcError{"no_target", "No eligible target for this action"});
    case DesktopActionError::not_found:
        return std::unexpected(IpcError{"not_found", "The requested target is unavailable"});
    case DesktopActionError::invalid_state:
        return std::unexpected(IpcError{"invalid_state", "The target cannot perform this action"});
    case DesktopActionError::ambiguous_output:
        return std::unexpected(
            IpcError{"ambiguous_output", "The output selector matches more than one output"});
    }
    return std::unexpected(IpcError{"internal_error", "Unknown desktop action result"});
}

IpcAction decode_ipc_action(std::string_view method, const IpcJson &params) {
    const std::pair<std::string_view, ActionKind> kinds[] = {
        {"action.focus", ActionKind::focus},
        {"action.cycle_focus_mru", ActionKind::cycle_focus_mru},
        {"action.workspace_select", ActionKind::workspace},
        {"action.workspace_transfer", ActionKind::transfer_workspace},
        {"action.window_to_workspace", ActionKind::move_to_workspace},
        {"action.window_toggle_tiling", ActionKind::toggle_window_tiling},
        {"action.window_toggle_pin", ActionKind::toggle_window_pin},
        {"action.window_toggle_maximize", ActionKind::toggle_window_maximize},
        {"action.window_toggle_fullscreen", ActionKind::toggle_window_fullscreen},
        {"action.window_close", ActionKind::close_window},
        {"action.window_move", ActionKind::move_window},
        {"action.window_swap", ActionKind::swap_window},
        {"action.window_resize", ActionKind::resize_window},
        {"action.night_light_set", ActionKind::night_light},
        {"action.exit", ActionKind::exit},
    };
    const auto selected =
        std::ranges::find(kinds, method, &std::pair<std::string_view, ActionKind>::first);
    IpcAction request{};
    if (selected == std::end(kinds)) {
        return request;
    }
    request.action.kind = selected->second;
    if (params.contains("direction")) {
        const auto &direction = params.at("direction");
        const std::pair<std::string_view, Direction> choices[] = {{"left", Direction::left},
                                                                  {"right", Direction::right},
                                                                  {"up", Direction::up},
                                                                  {"down", Direction::down}};
        for (const auto &[name, value] : choices) {
            if (direction == name) {
                request.action.direction = value;
                break;
            }
        }
    }
    if (params.contains("window_id")) {
        request.window = params.at("window_id").get<WindowId>();
    }
    if (params.contains("workspace_id")) {
        request.workspace = params.at("workspace_id").get<WorkspaceId>();
        request.action.argument = *request.workspace;
    }
    if (request.action.kind == ActionKind::resize_window) {
        request.action.argument = params.value("delta", 32);
    }
    if (params.contains("selector")) {
        request.action.text = params.at("selector").get<std::string>();
    }
    if (params.contains("enabled")) {
        request.action.night_light =
            params.at("enabled").get<bool>() ? NightLightAction::on : NightLightAction::off;
    }
    return request;
}

IpcJson SessionControl::FocusState::json() const {
    return {{"window_id", nullable(window)},
            {"workspace_id", nullable(workspace)},
            {"output_id", nullable(output)}};
}

SessionControl::FocusState SessionControl::focus_state() const {
    const auto &desktop = desktop_.windows();
    return {desktop_.layer_has_keyboard_focus() ? std::nullopt : desktop.focused_window(),
            desktop.focused_workspace(), std::optional<OutputId>(desktop.focused_output())};
}

IpcJson SessionControl::focus() const {
    return focus_state().json();
}

SessionControl::WorkspaceState SessionControl::workspace_state(WorkspaceId id) const {
    const auto &desktop = desktop_.windows();
    const auto output = desktop.workspace_output(id);
    const auto *state = output ? desktop.state().output(*output) : nullptr;
    return {std::optional<OutputId>(desktop.workspace_owner(id)), std::optional<OutputId>(output),
            state && state->shown == id};
}

IpcJson SessionControl::workspaces() const {
    const auto &desktop = desktop_.windows();
    auto result = Json::array();
    for (const auto &[id, workspace] : desktop.state().workspaces()) {
        const auto state = workspace_state(id);
        result.push_back({{"id", id},
                          {"owner_output_id", nullable(state.owner)},
                          {"output_id", nullable(state.output)},
                          {"visible", state.visible},
                          {"focused", desktop.focused_workspace() == id}});
    }
    return result;
}

IpcJson SessionControl::windows() const {
    const auto &desktop = desktop_.windows();
    auto result = Json::array();
    for (const auto &[id, state] : desktop.state().windows()) {
        const auto view = desktop.view(id);
        const auto metadata = scene_.window_metadata(id);
        result.push_back({{"id", id},
                          {"workspace_id", state.home_workspace},
                          {"app_id", bounded_utf8(metadata.app_id, 1024)},
                          {"title", bounded_utf8(metadata.title, 4096)},
                          {"mapped", state.mapped},
                          {"floating", !view.tiled},
                          {"fullscreen", view.fullscreen},
                          {"maximized", view.maximized},
                          {"pinned", view.pinned}});
    }
    return result;
}

IpcJson SessionControl::output_rows() const {
    auto result = Json::array();
    for (const auto &[id, output] : outputs_.outputs()) {
        const auto &state = output.status.settings;
        const auto render = scene_.render_status(output.handle);
        const auto &last_mode = output.status.last_mode;
        const auto width = output.status.sleeping ? last_mode.width : state.width;
        const auto height = output.status.sleeping ? last_mode.height : state.height;
        const auto refresh =
            output.status.sleeping ? last_mode.refresh_millihz : state.refresh_millihz;
        auto mode = width > 0 && height > 0 && refresh > 0
                        ? Json{{"width", width}, {"height", height}, {"refresh_millihz", refresh}}
                        : Json(nullptr);
        result.push_back({
            {"id", output.id},
            {"selector", "connector:" + output.connector},
            {"identity_selector",
             output.identity.empty() ? Json(nullptr) : Json("identity:" + output.identity)},
            {"connector", output.connector},
            {"connected", output.handle != nullptr},
            {"enabled", output.enabled},
            {"power_state", !output.enabled || !output.handle ? "disabled"
                            : output.status.sleeping          ? "asleep"
                                                              : "awake"},
            {"focused", desktop_.windows().focused_output() == id},
            {"disabled_reason",
             output.disabled_reason.empty() ? Json(nullptr) : Json(output.disabled_reason)},
            {"position", {{"x", state.x}, {"y", state.y}}},
            {"scale", state.scale > 0 ? state.scale : 1},
            {"rotation", state.rotation},
            {"mode", std::move(mode)},
            {"hardware_cursor", output.enabled && !output.status.sleeping
                                    ? Json(output.status.cursor)
                                    : Json(nullptr)},
            {"adaptive_sync",
             capability(output.adaptive_sync_supported,
                        output.enabled && output.status.adaptive_sync,
                        output.adaptive_sync_supported == false ? "output lacks adaptive sync"
                        : output.adaptive_sync_rejected         ? "backend rejected adaptive sync"
                                                                : nullptr)},
            {"tearing", capability(render.tearing, render.tearing_applied, render.tearing_reason)},
            {"night_light",
             capability(render.night_light, render.night_light_applied, render.night_light_reason)},
        });
    }
    return result;
}

IpcJson SessionControl::night_light(const ConfigTransaction &config) const {
    auto outputs = Json::array();
    for (const auto &[id, output] : outputs_.outputs()) {
        const auto render = scene_.render_status(output.handle);
        auto cap =
            capability(render.night_light, render.night_light_applied, render.night_light_reason);
        cap["output_id"] = output.id;
        outputs.push_back(std::move(cap));
    }
    return {{"configured_enabled", config.active().night_light.enabled},
            {"enabled", config.runtime_night_light()},
            {"temperature_kelvin", config.active().night_light.temperature_kelvin},
            {"outputs", std::move(outputs)}};
}

IpcJson SessionControl::diagnostics() const {
    auto outputs = Json::array();
    for (const auto &[id, output] : outputs_.outputs()) {
        const auto &status = output.status;
        const auto render = scene_.render_status(output.handle);
        const auto fallback =
            render.fallback_reason ? Json::array({render.fallback_reason}) : Json::array();
        outputs.push_back({
            {"output_id", output.id},
            {"backend", render.backend ? Json(render.backend) : Json(nullptr)},
            {"renderer", renderer_ ? Json(renderer_) : Json(nullptr)},
            {"frame_state", render.frame_state},
            {"presentation_mode", render.presentation_mode},
            {"fallback_reasons", fallback},
            {"committed_frames", status.committed_frames},
            {"presented_frames", status.presented_frames},
            {"discarded_frames", status.discarded_frames},
            {"last_presentation_ns", status.last_presentation_ns
                                         ? Json(std::to_string(status.last_presentation_ns))
                                         : Json(nullptr)},
        });
    }
    return {{"outputs", std::move(outputs)},
            {"trace",
             {{"state", "empty"},
              {"entries", 0},
              {"bytes", 0},
              {"dropped_samples", 0},
              {"stop_reason", nullptr}}}};
}

IpcResult SessionControl::query(std::string_view method, const LiveConfig &config,
                                std::string_view path, std::string_view phase,
                                const Json &last_error) {
    if (method == "query.status") {
        return Json{{"ready", phase == "ready"},
                    {"phase", phase},
                    {"config", config_status(config, path)},
                    {"last_error", last_error}};
    }
    if (method == "query.bindings") {
        return bindings(config.transaction().active().keybinds);
    }
    if (method == "query.outputs") {
        outputs_.refresh();
        return Json{{"outputs", output_rows()}};
    }
    if (method == "query.workspaces") {
        return Json{{"workspaces", workspaces()}};
    }
    if (method == "query.windows") {
        return Json{{"windows", windows()}};
    }
    if (method == "query.focus") {
        return focus();
    }
    if (method == "query.config") {
        return config_status(config, path);
    }
    if (method == "query.diagnostics") {
        outputs_.refresh();
        return diagnostics();
    }
    if (method == "query.night_light") {
        return night_light(config.transaction());
    }
    return std::unexpected(IpcError{"unknown_method", "Unknown query"});
}

void SessionControl::publish(IpcServer &ipc) {
    const auto &desktop = desktop_.windows();
    for (auto old = published_windows_.begin(); old != published_windows_.end();) {
        if (desktop.state().window(old->first)) {
            ++old;
            continue;
        }
        if (old->second.second) {
            ipc.event("window.unmapped",
                      {{"window_id", old->first}, {"workspace_id", old->second.first}});
        }
        ipc.event("window.destroyed", {{"window_id", old->first}});
        old = published_windows_.erase(old);
    }
    for (const auto &[id, window] : desktop.state().windows()) {
        auto [previous, inserted] =
            published_windows_.try_emplace(id, window.home_workspace, false);
        if (previous->second.second != window.mapped) {
            ipc.event(window.mapped ? "window.mapped" : "window.unmapped",
                      {{"window_id", id}, {"workspace_id", window.home_workspace}});
        }
        previous->second = {window.home_workspace, window.mapped};
    }
    for (const auto &[id, output] : outputs_.outputs()) {
        const auto &state = output.status.settings;
        const OutputState next{
            .connector = output.connector,
            .geometry = {state.x, state.y, state.width, state.height},
            .refresh_millihz = state.refresh_millihz,
            .rotation = state.rotation,
            .scale = state.scale,
            .connected = output.handle != nullptr,
            .enabled = output.enabled,
            .adaptive_sync = output.status.adaptive_sync,
        };
        auto [previous, inserted] = published_outputs_.try_emplace(output.id, next);
        if (inserted || previous->second != next) {
            ipc.event("output.changed", {{"output_id", output.id},
                                         {"connected", next.connected},
                                         {"enabled", next.enabled}});
            previous->second = next;
        }
    }
    for (const auto &[id, workspace] : desktop.state().workspaces()) {
        const auto next = workspace_state(id);
        auto [previous, inserted] = published_workspaces_.try_emplace(id, next);
        if (inserted || previous->second != next) {
            ipc.event("workspace.changed", {{"workspace_id", id},
                                            {"output_id", nullable(next.output)},
                                            {"visible", next.visible}});
            previous->second = next;
        }
    }
    auto focused = focus_state();
    if (published_focus_ != focused) {
        ipc.event("window.focused", focused.json());
        published_focus_ = std::move(focused);
    }
}

} // namespace seaside

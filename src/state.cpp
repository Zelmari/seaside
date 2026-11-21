#include "state.hpp"

#include <limits>
#include <new>

namespace seaside {

std::expected<void, StateError> State::add_output(std::string_view id) noexcept {
    if (id.empty()) {
        return std::unexpected(StateError::invalid_id);
    }
    try {
        if (!outputs_.emplace(id, OutputState{OutputId(id), {}}).second) {
            return std::unexpected(StateError::duplicate);
        }
        return {};
    } catch (const std::bad_alloc &) {
        return std::unexpected(StateError::allocation);
    }
}
std::expected<void, StateError> State::remove_output(std::string_view id) noexcept {
    auto found = outputs_.find(id);
    if (found == outputs_.end()) {
        return std::unexpected(StateError::missing_output);
    }
    outputs_.erase(found);
    // Workspaces keep the last owner for later policy-driven reconnection.
    // Removing the final output never destroys logical client/window state.
    return {};
}
std::expected<void, StateError> State::add_workspace(WorkspaceId id) noexcept {
    if (id == 0 || id > 2147483647U) {
        return std::unexpected(StateError::invalid_id);
    }
    try {
        if (!workspaces_.emplace(id, WorkspaceState{id, {}, {}}).second) {
            return std::unexpected(StateError::duplicate);
        }
        return {};
    } catch (const std::bad_alloc &) {
        return std::unexpected(StateError::allocation);
    }
}
std::expected<void, StateError> State::assign_workspace(WorkspaceId workspace_id,
                                                        std::string_view output_id) noexcept {
    auto workspace = workspaces_.find(workspace_id);
    if (workspace == workspaces_.end()) {
        return std::unexpected(StateError::missing_workspace);
    }
    if (!outputs_.contains(output_id)) {
        return std::unexpected(StateError::missing_output);
    }
    try {
        std::optional<OutputId> owner{output_id};
        for (auto &[id, output] : outputs_) {
            if (output.shown == workspace_id && id != output_id) {
                output.shown.reset();
            }
        }
        workspace->second.owner_or_last_output = std::move(owner);
    } catch (const std::bad_alloc &) {
        return std::unexpected(StateError::allocation);
    }
    return {};
}
std::expected<void, StateError> State::show_workspace(std::string_view output_id,
                                                      WorkspaceId workspace_id) noexcept {
    auto output = outputs_.find(output_id);
    if (output == outputs_.end()) {
        return std::unexpected(StateError::missing_output);
    }
    auto workspace = workspaces_.find(workspace_id);
    if (workspace == workspaces_.end()) {
        return std::unexpected(StateError::missing_workspace);
    }
    if (workspace->second.owner_or_last_output != output_id) {
        return std::unexpected(StateError::wrong_owner);
    }
    output->second.shown = workspace_id;
    return {};
}
std::expected<WindowId, StateError> State::create_window(WorkspaceId workspace_id) noexcept {
    auto workspace = workspaces_.find(workspace_id);
    if (workspace == workspaces_.end()) {
        return std::unexpected(StateError::missing_workspace);
    }
    if (next_window_id_ > 9007199254740991ULL) {
        return std::unexpected(StateError::id_exhausted);
    }
    const auto id = next_window_id_++;
    try {
        workspace->second.windows.insert(id);
        try {
            windows_.emplace(id, WindowState{id, workspace_id, false, false, {}, 0});
        } catch (const std::bad_alloc &) {
            workspace->second.windows.erase(id);
            throw;
        }
        return id;
    } catch (const std::bad_alloc &) {
        return std::unexpected(StateError::allocation);
    }
}
std::expected<void, StateError> State::initialize_window(WindowId id) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    window->second.initialized = true;
    return {};
}
std::expected<void, StateError> State::map_window(WindowId id) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    if (!window->second.initialized) {
        return std::unexpected(StateError::not_initialized);
    }
    if (window->second.mapped) {
        return std::unexpected(StateError::already_mapped);
    }
    if (window->second.initial_mapping_seq == 0) {
        if (next_mapping_seq_ == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(StateError::id_exhausted);
        }
        window->second.initial_mapping_seq = next_mapping_seq_++;
    }
    window->second.mapped = true;
    return {};
}
std::expected<void, StateError> State::unmap_window(WindowId id) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    if (!window->second.mapped) {
        return std::unexpected(StateError::not_mapped);
    }
    window->second.mapped = false;
    return {};
}
std::expected<void, StateError> State::destroy_window(WindowId id) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    workspaces_.at(window->second.home_workspace).windows.erase(id);
    windows_.erase(window);
    return {};
}
std::expected<void, StateError> State::move_window(WindowId id, WorkspaceId destination) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    auto workspace = workspaces_.find(destination);
    if (workspace == workspaces_.end()) {
        return std::unexpected(StateError::missing_workspace);
    }
    if (window->second.home_workspace == destination) {
        return {};
    }
    // Transferring the existing set node keeps parent-and-dialog moves free of
    // allocation failures after the destination workspace has been prepared.
    auto &source = workspaces_.at(window->second.home_workspace).windows;
    workspace->second.windows.insert(source.extract(id));
    window->second.home_workspace = destination;
    return {};
}
std::expected<void, StateError> State::set_tiling_override(WindowId id,
                                                           std::optional<bool> value) noexcept {
    auto window = windows_.find(id);
    if (window == windows_.end()) {
        return std::unexpected(StateError::missing_window);
    }
    window->second.tiling_override = value;
    return {};
}
void State::clear_tiling_overrides() noexcept {
    for (auto &[id, window] : windows_) {
        (void)id;
        window.tiling_override.reset();
    }
}
const WindowState *State::window(WindowId id) const noexcept {
    auto found = windows_.find(id);
    return found == windows_.end() ? nullptr : &found->second;
}
const WorkspaceState *State::workspace(WorkspaceId id) const noexcept {
    auto found = workspaces_.find(id);
    return found == workspaces_.end() ? nullptr : &found->second;
}
const OutputState *State::output(std::string_view id) const noexcept {
    auto found = outputs_.find(id);
    return found == outputs_.end() ? nullptr : &found->second;
}
std::expected<std::vector<WindowId>, StateError>
State::mapped_windows(WorkspaceId id) const noexcept {
    const auto *found = workspace(id);
    if (!found) {
        return std::unexpected(StateError::missing_workspace);
    }
    try {
        std::vector<WindowId> result;
        for (const auto window_id : found->windows) {
            if (windows_.at(window_id).mapped) {
                result.push_back(window_id);
            }
        }
        return result;
    } catch (const std::bad_alloc &) {
        return std::unexpected(StateError::allocation);
    }
}
bool State::valid() const noexcept {
    for (const auto &[id, window] : windows_) {
        auto workspace = workspaces_.find(window.home_workspace);
        if (workspace == workspaces_.end() || !workspace->second.windows.contains(id) ||
            (window.mapped && (!window.initialized || window.initial_mapping_seq == 0))) {
            return false;
        }
    }
    for (const auto &[id, workspace] : workspaces_) {
        for (const auto window_id : workspace.windows) {
            auto window = windows_.find(window_id);
            if (window == windows_.end() || window->second.home_workspace != id) {
                return false;
            }
        }
    }
    for (const auto &[id, output] : outputs_) {
        if (output.shown) {
            auto workspace = workspaces_.find(*output.shown);
            if (workspace == workspaces_.end() || workspace->second.owner_or_last_output != id) {
                return false;
            }
        }
    }
    return true;
}

} // namespace seaside

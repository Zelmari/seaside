#include "window_manager.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace seaside {
namespace {

constexpr WorkspaceId max_workspace_id = 2147483647U;
constexpr auto fullscreen_priority = std::uint64_t{1} << 63;
constexpr auto maximized_focus_priority = std::uint64_t{1} << 62;
constexpr auto floating_priority = std::uint64_t{1} << 61;

int coordinate(std::int64_t value) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(value, std::numeric_limits<int>::min(),
                                                     std::numeric_limits<int>::max()));
}

std::int64_t distance(std::int64_t value) noexcept {
    return value < 0 ? -value : value;
}

bool valid_area(WindowRect bounds, WindowRect usable) noexcept {
    return bounds.width > 0 && bounds.height > 0 && usable.width > 0 && usable.height > 0 &&
           usable.x >= bounds.x && usable.y >= bounds.y &&
           std::int64_t{usable.x} + usable.width <= std::int64_t{bounds.x} + bounds.width &&
           std::int64_t{usable.y} + usable.height <= std::int64_t{bounds.y} + bounds.height;
}

LayoutSettings layout_settings(const Config &config) noexcept {
    return {.kind = config.window.layout,
            .insertion = config.dwindle.insertion,
            .split_ratio = config.dwindle.split_ratio,
            .default_column_width = config.scrolling.default_column_width,
            .outer_gap = config.window.gaps_outer,
            .inner_gap = config.window.gaps_inner,
            .border_width = config.window.border_width};
}

bool contains(WindowRect rectangle, double x, double y) noexcept {
    return x >= rectangle.x && y >= rectangle.y &&
           x < static_cast<double>(rectangle.x) + rectangle.width &&
           y < static_cast<double>(rectangle.y) + rectangle.height;
}

bool intersects_horizontally(WindowRect rectangle, WindowRect area) noexcept {
    return std::int64_t{rectangle.x} + rectangle.width > area.x &&
           std::int64_t{area.x} + area.width > rectangle.x;
}

int fit_position(std::int64_t position, int extent, int available) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(position, 0, std::max(0, available - extent)));
}

} // namespace

WindowManager::WindowManager(WorkspaceMode mode, bool tiling) noexcept
    : mode_(mode), tiling_(tiling) {}

WindowManager::WindowManager(const Config &config) noexcept
    : mode_(config.workspaces.mode), tiling_(config.window.tiling),
      layout_settings_(layout_settings(config)) {}

bool WindowManager::create_workspace(WorkspaceId id) {
    if (id == 0 || id > max_workspace_id) {
        return false;
    }
    auto [workspace, inserted] = workspaces_.try_emplace(id);
    if (!inserted) {
        return true;
    }
    if (!state_.add_workspace(id)) {
        workspaces_.erase(workspace);
        return false;
    }
    workspace->second.layout.set_settings(layout_settings_);
    return true;
}

WorkspaceLayout &WindowManager::layout_for(WindowId id) {
    return workspaces_.at(state_.window(id)->home_workspace).layout;
}

void WindowManager::update_layout_area(WorkspaceId id) {
    // Keep the last usable geometry while disconnected so tree placement and
    // scrolling widths and offsets survive output loss.
    if (const auto output = workspace_output(id)) {
        workspaces_.at(id).layout.set_area(outputs_.find(*output)->second.usable);
    }
}

void WindowManager::update_layout_areas() {
    for (const auto &[id, workspace] : workspaces_) {
        update_layout_area(id);
    }
}

std::optional<WindowId> WindowManager::layout_focus(WorkspaceId id) const {
    const auto &workspace = workspaces_.at(id);
    if (focused_workspace() == id && focused_window_ &&
        workspace.layout.contains(*focused_window_)) {
        return focused_window_;
    }
    // The currently focused floating window does not select a Dwindle split.
    // Background workspaces and scrolling strips retain their local tile focus.
    if (focused_workspace() == id && focused_window_ && layout_settings_.kind == Layout::dwindle) {
        return {};
    }
    const auto previous = std::ranges::find_if(workspace.focus_history, [&](WindowId window) {
        return workspace.layout.contains(window);
    });
    return previous == workspace.focus_history.end() ? std::nullopt : std::optional{*previous};
}

void WindowManager::attach_tile(WindowId id) {
    const auto *entry = state_.window(id);
    const auto &window = windows_.at(id);
    if (!entry->mapped || !is_tiled(id, window) || (tile_move_ && tile_move_->tile.id == id)) {
        return;
    }
    auto &layout = layout_for(id);
    if (!layout.contains(id)) {
        const TiledWindow tile{id, entry->initial_mapping_seq, window.limits};
        const auto target = layout_focus(entry->home_workspace);
        layout.add(tile, target);
        if (tile_move_ && tile_move_->workspace == entry->home_workspace) {
            tile_move_->previous.add(tile, target);
        }
    }
}

void WindowManager::remove_tile(WindowId id) {
    if (tile_move_ && tile_move_->tile.id == id) {
        cancel_tile_move();
    }
    const bool removed = layout_for(id).erase(id).has_value();
    if (tile_move_ && tile_move_->workspace == state_.window(id)->home_workspace) {
        tile_move_->previous.erase(id);
    }
    if (removed && focused_window_ && focused_window_ != id) {
        reveal(*focused_window_);
    }
}

void WindowManager::restore_after_removal(std::optional<WindowId> preferred,
                                          WorkspaceId workspace) {
    if (preferred && focused_workspace() == workspace && eligible(*preferred, workspace) &&
        view(*preferred).tile_layout) {
        set_focus(preferred);
    } else {
        restore_focus();
    }
}

void WindowManager::reveal(WindowId id, std::optional<Direction> direction) {
    if (layout_settings_.kind != Layout::scrolling) {
        return;
    }
    // A floating dialog also needs its tiled parent brought into view.
    const Window *window = &windows_.at(id);
    while (!window->maximized && !window->fullscreen) {
        if (!window->parent) {
            layout_for(id).reveal(id, direction);
            return;
        }
        id = *window->parent;
        window = &windows_.at(id);
    }
}

std::optional<WorkspaceId> WindowManager::fresh_workspace() {
    WorkspaceId id = 1;
    for (const auto &[used, workspace] : workspaces_) {
        if (used != id) {
            break;
        }
        if (id == max_workspace_id) {
            return {};
        }
        ++id;
    }
    return create_workspace(id) ? std::optional{id} : std::nullopt;
}

bool WindowManager::show_workspace(std::string_view output, WorkspaceId workspace) {
    if (!state_.show_workspace(output, workspace)) {
        return false;
    }
    workspaces_.at(workspace).last_shown = ++sequence_;
    outputs_.find(output)->second.last_workspace = workspace;
    update_layout_area(workspace);
    return true;
}

bool WindowManager::show_fallback(std::string_view output) {
    std::optional<WorkspaceId> candidate;
    std::uint64_t last_shown = 0;
    for (const auto &[id, workspace] : workspaces_) {
        if (workspace_output(id) == output && (!candidate || workspace.last_shown > last_shown)) {
            candidate = id;
            last_shown = workspace.last_shown;
        }
    }
    if (!candidate) {
        candidate = fresh_workspace();
        if (!candidate || !state_.assign_workspace(*candidate, output)) {
            return false;
        }
    }
    return show_workspace(output, *candidate);
}

bool WindowManager::add_output(std::string_view id, WindowRect bounds, WindowRect usable,
                               bool primary) {
    if (!valid_area(bounds, usable) || id.empty()) {
        return false;
    }
    cancel_tile_move();
    auto [position, inserted] = outputs_.try_emplace(OutputId{id});
    auto &[name, output] = *position;
    if (output.connected || !state_.add_output(name)) {
        if (inserted) {
            outputs_.erase(position);
        }
        return false;
    }
    output.bounds = bounds;
    output.usable = usable;
    output.connected = true;
    std::optional<WorkspaceId> restored;
    for (auto &[workspace_id, workspace] : workspaces_) {
        const auto &owner = state_.workspace(workspace_id)->owner_or_last_output;
        if (workspace.migration && workspace.migration->origin == name) {
            if (!state_.assign_workspace(workspace_id, name)) {
                return false;
            }
            if (workspace.migration->was_shown) {
                restored = workspace_id;
            }
            workspace.migration.reset();
        } else if (!owner) {
            if (!state_.assign_workspace(workspace_id, name)) {
                return false;
            }
        } else if (!workspace_output(workspace_id)) {
            // A different output may be the first to return from zero outputs.
            // Make disconnected workspaces usable while retaining their origin.
            if (!workspace.migration) {
                const auto &previous = outputs_.at(*owner);
                workspace.migration = Migration{*owner, previous.last_workspace == workspace_id};
            }
            if (!state_.assign_workspace(workspace_id, name)) {
                return false;
            }
        }
    }

    const auto preferred = restored ? restored : output.last_workspace;
    if (preferred && workspace_output(*preferred) == name) {
        if (!show_workspace(name, *preferred)) {
            return false;
        }
    } else if (!show_fallback(name)) {
        return false;
    }

    // Restoring a formerly migrated visible workspace can leave the remaining
    // output with nothing shown. Select that output's own most recent fallback.
    for (const auto &[other, entry] : outputs_) {
        if (entry.connected && !state_.output(other)->shown && !show_fallback(other)) {
            return false;
        }
    }
    for (auto &[window_id, window] : windows_) {
        if (state_.window(window_id)->mapped) {
            position_window(window_id, window);
        }
    }
    update_layout_areas();
    if (!primary_output_ || primary) {
        primary_output_ = name;
    }
    if (!focused_output_ || primary) {
        return focus_on_output(name);
    }
    restore_focus();
    return true;
}

bool WindowManager::remove_output(std::string_view id, std::string_view ranked_fallback) {
    auto position = outputs_.find(id);
    if (position == outputs_.end() || !position->second.connected) {
        return false;
    }
    cancel_tile_move();
    finish_focus_cycle();
    auto &[name, output] = *position;
    const auto shown = state_.output(name)->shown;
    if (!state_.remove_output(name)) {
        return false;
    }
    output.connected = false;
    std::optional<std::string_view> fallback;
    if (primary_output_ && primary_output_ != name) {
        fallback = primary_output_;
    } else {
        std::uint64_t last_focused = 0;
        for (const auto &[other, entry] : outputs_) {
            if (entry.connected && (!fallback || entry.last_focused > last_focused ||
                                    (!last_focused && other == ranked_fallback))) {
                fallback = other;
                last_focused = entry.last_focused;
            }
        }
        primary_output_ = fallback;
    }

    if (fallback) {
        for (auto &[workspace_id, workspace] : workspaces_) {
            if (state_.workspace(workspace_id)->owner_or_last_output == name) {
                if (!workspace.migration) {
                    workspace.migration = Migration{name, shown == workspace_id};
                }
                if (!state_.assign_workspace(workspace_id, *fallback)) {
                    return false;
                }
            }
        }
    }
    update_layout_areas();
    if (focused_output_ == name) {
        focused_output_.reset();
        if (fallback) {
            return focus_on_output(*fallback);
        }
    }
    restore_focus();
    return true;
}

bool WindowManager::update_output(std::string_view id, WindowRect bounds, WindowRect usable) {
    auto found = outputs_.find(id);
    if (found == outputs_.end() || !found->second.connected || !valid_area(bounds, usable)) {
        return false;
    }
    cancel_tile_move();
    found->second.bounds = bounds;
    found->second.usable = usable;
    update_layout_areas();
    if (focused_window_) {
        if (is_visible(*focused_window_)) {
            reveal(*focused_window_);
        } else {
            restore_focus();
        }
    }
    return true;
}

std::optional<std::string_view> WindowManager::focused_output() const noexcept {
    return focused_output_;
}

std::optional<WindowRect> WindowManager::output_bounds(std::string_view id) const noexcept {
    auto found = outputs_.find(id);
    if (found != outputs_.end() && found->second.connected) {
        return found->second.bounds;
    }
    return {};
}

std::optional<WorkspaceId> WindowManager::focused_workspace() const noexcept {
    return focused_output_ ? state_.output(*focused_output_)->shown : std::nullopt;
}

bool WindowManager::focus_on_output(std::string_view id, bool restore_workspace_focus) {
    auto found = outputs_.find(id);
    if (found == outputs_.end() || !found->second.connected) {
        return false;
    }
    const auto previous = focused_output_;
    if (previous != id) {
        cancel_tile_move();
    }
    focused_output_ = found->first;
    found->second.last_focused = ++sequence_;
    if (previous != focused_output_ || restore_workspace_focus) {
        restore_focus();
    }
    return true;
}

void WindowManager::set_primary_output(std::string_view id) noexcept {
    const auto found = outputs_.find(id);
    if (found != outputs_.end() && found->second.connected) {
        primary_output_ = found->first;
    }
}

void WindowManager::set_initial_workspace_mode(WorkspaceMode mode) noexcept {
    if (outputs_.empty() && workspaces_.empty() && windows_.empty()) {
        mode_ = mode;
    }
}

bool WindowManager::has_fullscreen(std::string_view id) const {
    return fullscreen_on(id).has_value();
}

bool WindowManager::focus_output(std::string_view id) {
    finish_focus_cycle();
    return focus_on_output(id);
}

bool WindowManager::focus_output_at(double x, double y) {
    if (focused_output_ && contains(outputs_.find(*focused_output_)->second.bounds, x, y)) {
        return focus_output(*focused_output_);
    }
    for (const auto &[id, output] : outputs_) {
        if (output.connected && contains(output.bounds, x, y)) {
            return focus_output(id);
        }
    }
    return false;
}

std::optional<std::string_view> WindowManager::workspace_output(WorkspaceId id) const noexcept {
    const auto *workspace = state_.workspace(id);
    if (workspace && workspace->owner_or_last_output) {
        auto found = outputs_.find(*workspace->owner_or_last_output);
        if (found != outputs_.end() && found->second.connected) {
            return found->first;
        }
    }
    return {};
}

std::optional<std::string_view> WindowManager::window_output(WindowId id) const noexcept {
    const auto *window = state_.window(id);
    return window ? workspace_output(window->home_workspace) : std::nullopt;
}

std::optional<std::string_view> WindowManager::workspace_owner(WorkspaceId id) const noexcept {
    const auto entry = workspaces_.find(id);
    if (entry != workspaces_.end() && entry->second.migration) {
        return entry->second.migration->origin;
    }
    const auto *workspace = state_.workspace(id);
    if (workspace && workspace->owner_or_last_output) {
        return *workspace->owner_or_last_output;
    }
    return {};
}

bool WindowManager::select_workspace(WorkspaceId id) {
    if (!focused_output_) {
        return false;
    }
    if (focused_workspace() != id) {
        cancel_tile_move();
    }
    finish_focus_cycle();
    if (!create_workspace(id)) {
        return false;
    }
    auto output = workspace_output(id);
    if (!output) {
        output = focused_output_;
        if (!state_.assign_workspace(id, *output)) {
            return false;
        }
    }
    return show_workspace(*output, id) && focus_on_output(*output, true);
}

bool WindowManager::transfer_workspace(std::string_view destination,
                                       std::optional<WorkspaceId> target) {
    const auto workspace = target ? target : focused_workspace();
    const auto record = workspace ? workspaces_.find(*workspace) : workspaces_.end();
    const auto source = workspace ? workspace_output(*workspace) : std::nullopt;
    const bool moves_output = source != destination;
    // A transfer to the current fallback can make a temporary assignment permanent.
    if (record == workspaces_.end() || (!moves_output && !record->second.migration) ||
        !state_.output(destination)) {
        return false;
    }
    const auto *source_state = source ? state_.output(*source) : nullptr;
    const bool replace_source = moves_output && source_state && source_state->shown == workspace;
    cancel_tile_move();
    finish_focus_cycle();
    if ((moves_output && !state_.assign_workspace(*workspace, destination)) ||
        !show_workspace(destination, *workspace) || (replace_source && !show_fallback(*source))) {
        return false;
    }
    record->second.migration.reset();
    return focus_on_output(destination, true);
}

std::optional<WindowId> WindowManager::create_window(std::optional<WindowId> parent) {
    std::optional<WorkspaceId> home;
    if (parent) {
        const auto *parent_state = state_.window(*parent);
        if (!parent_state) {
            return {};
        }
        home = parent_state->home_workspace;
    } else {
        home = focused_workspace();
    }
    if (!home) {
        // Clients may connect while no output is usable. Reuse the last
        // workspace when available, and keep it owned by its remembered output.
        if (workspaces_.empty()) {
            home = fresh_workspace();
        } else {
            const auto latest = std::ranges::max_element(
                workspaces_, {}, [](const auto &entry) { return entry.second.last_shown; });
            home = latest->first;
        }
    }
    if (!home) {
        return {};
    }
    auto id = state_.create_window(*home);
    if (!id) {
        return {};
    }
    try {
        windows_.try_emplace(*id).first->second.parent = parent;
    } catch (...) {
        (void)state_.destroy_window(*id);
        throw;
    }
    return *id;
}

bool WindowManager::initialize_window(WindowId id) {
    return state_.initialize_window(id).has_value();
}

std::optional<WindowRect> WindowManager::initial_tile_geometry(WindowId id) const {
    const auto *entry = state_.window(id);
    if (!entry || !entry->initialized || entry->mapped || !window_output(id)) {
        return {};
    }
    const auto &window = windows_.at(id);
    if (!is_tiled(id, window) || window.maximized || window.fullscreen) {
        return {};
    }
    // Preview the insertion without shrinking mapped tiles or changing focus.
    const auto workspace = entry->home_workspace;
    auto layout = workspaces_.at(workspace).layout;
    if (!layout.add({id, entry->initial_mapping_seq, window.limits}, layout_focus(workspace))) {
        return {};
    }
    const auto geometry = layout.geometry(id);
    return geometry && geometry->width > 0 && geometry->height > 0 ? geometry : std::nullopt;
}

void WindowManager::position_window(WindowId id, Window &window) {
    auto output = window_output(id);
    if (!output || window.positioned || window.ordinary.width <= 0 || window.ordinary.height <= 0) {
        return;
    }
    const auto &usable = outputs_.find(*output)->second.usable;
    const auto order = state_.window(id)->initial_mapping_seq;
    constexpr int step = 32;
    const auto offset = static_cast<int>(order == 0 ? 0 : (order - 1) % 7) * step;
    window.ordinary.x = fit_position((usable.width - window.ordinary.width) / 2 + offset,
                                     window.ordinary.width, usable.width);
    window.ordinary.y = fit_position((usable.height - window.ordinary.height) / 2 + offset,
                                     window.ordinary.height, usable.height);
    window.positioned = true;
}

bool WindowManager::map_window(WindowId id, WindowRect committed, bool ordinary_buffer) {
    if (!state_.map_window(id)) {
        return false;
    }
    auto &window = windows_.at(id);
    window.stacking = ++sequence_;
    // A buffer drawn for the initial tile does not establish a floating size.
    if (ordinary_buffer && !window.fullscreen && !window.maximized &&
        (!is_tiled(id, window) || window.ordinary.width == 0 || window.ordinary.height == 0)) {
        commit_geometry(id, committed);
    }
    position_window(id, window);
    attach_tile(id);

    const auto *state = state_.window(id);
    const auto active_dialog =
        window.parent && focused_window_ &&
        (*focused_window_ == *window.parent || is_descendant(*focused_window_, *window.parent));
    if (state->home_workspace == focused_workspace() && (!window.parent || active_dialog) &&
        is_visible(id)) {
        finish_focus_cycle();
        set_focus(id);
    }
    return true;
}

bool WindowManager::unmap_window(WindowId id) {
    const auto *entry = state_.window(id);
    if (!entry || !entry->mapped) {
        return false;
    }
    if (tile_move_ && tile_move_->tile.id == id) {
        cancel_tile_move();
    }
    const auto workspace = entry->home_workspace;
    const auto preferred = focused_window_ == id ? layout_for(id).close_focus(id) : std::nullopt;
    if (!state_.unmap_window(id)) {
        return false;
    }
    remove_tile(id);
    forget_focus(id);
    auto &window = windows_.at(id);
    // The next map starts a fresh XDG toplevel state, while its ID and original
    // mapping order belong to the lifetime of the Wayland object.
    window.maximized = false;
    window.fullscreen = false;
    window.pinned = false;
    (void)state_.set_tiling_override(id, {});
    if (focused_window_ == id || (focused_window_ && !is_visible(*focused_window_))) {
        restore_after_removal(preferred, workspace);
    }
    return true;
}

bool WindowManager::destroy_window(WindowId id) {
    const auto *entry = state_.window(id);
    if (!entry) {
        return false;
    }
    if (tile_move_ && tile_move_->tile.id == id) {
        cancel_tile_move();
    }
    const auto workspace = entry->home_workspace;
    const auto preferred = focused_window_ == id ? layout_for(id).close_focus(id) : std::nullopt;
    const auto parent = windows_.at(id).parent;
    for (auto &[child, window] : windows_) {
        if (window.parent == id) {
            set_parent(child, parent);
        }
    }
    remove_tile(id);
    forget_focus(id);
    if (!state_.destroy_window(id)) {
        return false;
    }
    windows_.erase(id);
    if (focused_window_ == id || (focused_window_ && !is_visible(*focused_window_))) {
        restore_after_removal(preferred, workspace);
    }
    return true;
}

bool WindowManager::is_descendant(WindowId id, WindowId ancestor) const {
    auto position = windows_.find(id);
    while (position != windows_.end() && position->second.parent) {
        const auto parent = *position->second.parent;
        if (parent == ancestor) {
            return true;
        }
        position = windows_.find(parent);
    }
    return false;
}

bool WindowManager::relocate_window(WindowId id, WorkspaceId workspace) {
    if (state_.window(id)->home_workspace != workspace) {
        cancel_tile_move();
    }
    for (const auto &[member, window] : windows_) {
        if (member != id && !is_descendant(member, id)) {
            continue;
        }
        const auto old = state_.window(member)->home_workspace;
        if (!state_.move_window(member, workspace)) {
            return false;
        }
        if (old != workspace) {
            workspaces_.at(old).layout.erase(member);
            forget_focus(member);
            attach_tile(member);
        }
    }
    return true;
}

bool WindowManager::set_parent(WindowId id, std::optional<WindowId> parent) {
    auto position = windows_.find(id);
    if (position == windows_.end() ||
        (parent && (!windows_.contains(*parent) || *parent == id || is_descendant(*parent, id)))) {
        return false;
    }
    auto &window = position->second;
    if (window.parent == parent) {
        return true;
    }
    if (tile_move_ && tile_move_->tile.id == id) {
        cancel_tile_move();
    }
    if (!parent && window.parent) {
        if (const auto output = window_output(id)) {
            const auto current = geometry(id, window);
            const auto &usable = outputs_.find(*output)->second.usable;
            window.ordinary.x = coordinate(std::int64_t{current.x} - usable.x);
            window.ordinary.y = coordinate(std::int64_t{current.y} - usable.y);
        }
    }
    if (parent) {
        const auto previous_parent = window.parent;
        window.parent = parent;
        if (!relocate_window(id, state_.window(*parent)->home_workspace)) {
            window.parent = previous_parent;
            return false;
        }
        remove_tile(id);
        window.pinned = false;
        window.dialog_dx = 0;
        window.dialog_dy = 0;
    }
    window.parent = parent;
    attach_tile(id);
    if (focused_window_ &&
        (!is_visible(*focused_window_) || window_output(*focused_window_) != focused_output_)) {
        restore_focus();
    } else if (focused_window_) {
        reveal(*focused_window_);
    }
    return true;
}

bool WindowManager::commit_geometry(WindowId id, WindowRect committed) {
    auto position = windows_.find(id);
    if (position == windows_.end()) {
        return false;
    }
    auto &window = position->second;
    if (window.fullscreen || window.maximized || layout_for(id).contains(id) ||
        (tile_move_ && tile_move_->tile.id == id)) {
        return true;
    }
    if (committed.width > 0) {
        window.ordinary.width = committed.width;
    }
    if (committed.height > 0) {
        window.ordinary.height = committed.height;
    }
    if (state_.window(id)->mapped) {
        position_window(id, window);
    }
    return true;
}

bool WindowManager::set_size_limits(WindowId id, int min_width, int min_height, int max_width,
                                    int max_height) {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return false;
    }
    const TileLimits limits{min_width, min_height, max_width, max_height};
    if (found->second.limits == limits) {
        return false;
    }
    found->second.limits = limits;
    layout_for(id).update_limits(id, limits);
    if (tile_move_ && tile_move_->workspace == state_.window(id)->home_workspace) {
        tile_move_->previous.update_limits(id, limits);
        if (tile_move_->tile.id == id) {
            tile_move_->tile.limits = limits;
        }
    }
    if (focused_window_) {
        reveal(*focused_window_);
    }
    return true;
}

WindowRect WindowManager::geometry(WindowId id, const Window &window) const {
    auto ordinary = window.ordinary;
    auto output = window_output(id);
    if (!output) {
        return ordinary;
    }
    const auto &display = outputs_.find(*output)->second;
    if (window.fullscreen) {
        return display.bounds;
    }
    if (window.maximized) {
        return inset_window_rect(display.usable, layout_settings_.outer_gap);
    }
    if (tile_move_ && tile_move_->tile.id == id) {
        return tile_move_->geometry;
    }
    const auto &layout = workspaces_.at(state_.window(id)->home_workspace).layout;
    if (const auto tile = layout.geometry(id)) {
        return *tile;
    }

    const auto &usable = display.usable;
    std::int64_t x = ordinary.x;
    std::int64_t y = ordinary.y;
    if (!window.positioned) {
        x = (usable.width - ordinary.width) / 2;
        y = (usable.height - ordinary.height) / 2;
    }
    if (window.parent) {
        const auto *parent = state_.window(*window.parent);
        const auto anchor = parent && parent->mapped
                                ? geometry(*window.parent, windows_.at(*window.parent))
                                : usable;
        x = std::int64_t{anchor.x} - usable.x + (anchor.width - ordinary.width) / 2 +
            window.dialog_dx;
        y = std::int64_t{anchor.y} - usable.y + (anchor.height - ordinary.height) / 2 +
            window.dialog_dy;
    }
    if (has_tile_ancestor(id) && layout_settings_.kind == Layout::scrolling) {
        ordinary.x = coordinate(std::int64_t{usable.x} + x);
        ordinary.y = coordinate(std::int64_t{usable.y} + y);
        return ordinary;
    }
    ordinary.x = coordinate(std::int64_t{usable.x} + fit_position(x, ordinary.width, usable.width));
    ordinary.y =
        coordinate(std::int64_t{usable.y} + fit_position(y, ordinary.height, usable.height));
    return ordinary;
}

std::optional<WindowId> WindowManager::local_focus(std::string_view output,
                                                   WorkspaceId workspace) const {
    const auto valid_focus = [&](WindowId id) {
        const auto *entry = state_.window(id);
        return entry && entry->mapped && entry->home_workspace == workspace;
    };
    if (focused_output_ == output && focused_window_ && valid_focus(*focused_window_)) {
        return focused_window_;
    }
    const auto &history = workspaces_.at(workspace).focus_history;
    const auto previous = std::ranges::find_if(history, valid_focus);
    return previous == history.end() ? std::nullopt : std::optional{*previous};
}

std::optional<WindowId> WindowManager::fullscreen_on(std::string_view output) const {
    const auto shown = state_.output(output)->shown;
    if (!shown) {
        return {};
    }
    // A newly focused ordinary window must remain visible when it opens over
    // a fullscreen client. Keep the client's fullscreen configure and promote
    // its stack again when the user focuses it or one of its dialogs.
    if (const auto focused = local_focus(output, *shown)) {
        for (auto current = focused; current;) {
            const auto &window = windows_.at(*current);
            if (window.fullscreen && state_.window(*current)->mapped) {
                return current;
            }
            current = window.parent;
        }
        return {};
    }
    std::optional<WindowId> result;
    std::uint64_t highest = 0;
    for (const auto id : state_.workspace(*shown)->windows) {
        const auto &window = windows_.at(id);
        if (state_.window(id)->mapped && window.fullscreen && window.stacking > highest) {
            highest = window.stacking;
            result = id;
        }
    }
    return result;
}

bool WindowManager::is_visible(WindowId id) const {
    const auto *entry = state_.window(id);
    if (!entry || !entry->mapped) {
        return false;
    }
    const auto output = workspace_output(entry->home_workspace);
    if (!output) {
        return false;
    }
    const auto &window = windows_.at(id);
    if (!window.maximized && !window.fullscreen) {
        const auto tile = workspaces_.at(entry->home_workspace).layout.geometry(id);
        if (tile && (tile->width <= 0 || tile->height <= 0)) {
            return false;
        }
    }
    const auto shown = state_.output(*output)->shown;
    bool pinned = window.pinned;
    for (auto ancestor = window.parent; ancestor && !pinned;) {
        const auto &parent = windows_.at(*ancestor);
        pinned = parent.pinned && state_.window(*ancestor)->mapped;
        ancestor = parent.parent;
    }
    if (shown == entry->home_workspace) {
        const auto fullscreen = pinned ? fullscreen_on(*output) : std::nullopt;
        return !fullscreen || id == fullscreen || is_descendant(id, *fullscreen);
    }
    return shown && pinned && !window.fullscreen && !fullscreen_on(*output);
}

bool WindowManager::is_tiled(WindowId id, const Window &window) const {
    return !window.parent && !window.pinned && state_.window(id)->tiling_override.value_or(tiling_);
}

bool WindowManager::has_tile_ancestor(WindowId id) const {
    const auto &layout = workspaces_.at(state_.window(id)->home_workspace).layout;
    for (auto parent = windows_.at(id).parent; parent; parent = windows_.at(*parent).parent) {
        const auto &window = windows_.at(*parent);
        if (window.maximized || window.fullscreen) {
            return false;
        }
        if (layout.contains(*parent)) {
            return true;
        }
        if (tile_move_ && tile_move_->tile.id == *parent) {
            return true;
        }
    }
    return false;
}

WindowView WindowManager::view(WindowId id) const {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return {};
    }
    const auto &window = found->second;
    const auto workspace = state_.window(id)->home_workspace;
    const auto &layout = workspaces_.at(workspace).layout;
    const bool moving = tile_move_ && tile_move_->tile.id == id;
    const bool ordinary = !window.maximized && !window.fullscreen;
    const bool tile_layout = ordinary && layout.contains(id);
    const bool clipped = ordinary && (tile_layout || moving || has_tile_ancestor(id));
    int border = 0;
    if (moving) {
        border = tile_move_->border_width;
    } else if (ordinary) {
        border = tile_layout ? layout.border_width(id) : layout_settings_.border_width;
    }
    auto stacking = moving ? tile_move_->stacking : window.stacking;
    if (ordinary && !is_tiled(id, window)) {
        stacking |= floating_priority;
    }
    if (const auto output = window_output(id)) {
        if (window.fullscreen || window.parent) {
            if (const auto fullscreen = fullscreen_on(*output);
                fullscreen && (*fullscreen == id || is_descendant(id, *fullscreen))) {
                // Scene order matters even if a fullscreen client submits a
                // smaller buffer than its requested output rectangle.
                stacking |= fullscreen_priority;
            } else if (window.fullscreen) {
                stacking = 0;
            }
        }
        if (!window.fullscreen && (tile_layout || window.maximized) &&
            state_.output(*output)->shown == workspace && local_focus(*output, workspace) == id) {
            const auto &siblings = state_.workspace(workspace)->windows;
            const bool covered = tile_layout && std::ranges::any_of(siblings, [&](WindowId other) {
                                     const auto &occluder = windows_.at(other);
                                     return occluder.maximized && !occluder.fullscreen &&
                                            occluder.stacking > stacking &&
                                            state_.window(other)->mapped;
                                 });
            // Reveal the focused tile over a covering maximized window
            // without changing layout order or either window's saved state.
            if (window.maximized || covered) {
                stacking |= maximized_focus_priority;
            }
        }
    }
    return WindowView{
        .geometry = geometry(id, window),
        .visible = is_visible(id),
        .maximized = window.maximized,
        .fullscreen = window.fullscreen,
        .pinned = window.pinned,
        .tiled = is_tiled(id, window),
        .tile_layout = tile_layout,
        .clip = clipped ? layout.viewport() : WindowRect{},
        .border_width = border,
        .stacking = stacking,
    };
}

void WindowManager::remember_focus(WindowId id, WorkspaceId workspace) {
    const auto *window = state_.window(id);
    if (window && window->mapped) {
        auto &history = workspaces_.at(workspace).focus_history;
        const auto previous = std::ranges::find(history, id);
        if (previous != history.end()) {
            std::rotate(history.begin(), previous, previous + 1);
        } else {
            history.insert(history.begin(), id);
        }
    }
}

void WindowManager::forget_focus(WindowId id) {
    // A pinned window can have focus history on several workspaces.
    for (auto &[workspace, state] : workspaces_) {
        std::erase(state.focus_history, id);
    }
}

void WindowManager::set_focus(std::optional<WindowId> id, bool remember,
                              std::optional<Direction> direction) {
    if (id && remember && focused_workspace()) {
        remember_focus(*id, *focused_workspace());
    }
    if (id && id != focused_window_ && !is_tiled(*id, windows_.at(*id))) {
        windows_.at(*id).stacking = ++sequence_;
    }
    focused_window_ = id;
    if (id) {
        reveal(*id, direction);
    }
}

bool WindowManager::eligible(WindowId id, WorkspaceId workspace) const {
    const auto *entry = state_.window(id);
    if (!entry || !is_visible(id)) {
        return false;
    }
    const auto output = window_output(id);
    return entry->home_workspace == workspace ||
           (output && state_.output(*output)->shown == workspace);
}

void WindowManager::restore_focus() {
    const auto workspace = focused_workspace();
    if (!workspace) {
        set_focus({});
        return;
    }
    const auto &history = workspaces_.at(*workspace).focus_history;
    for (const auto id : history) {
        if (eligible(id, *workspace)) {
            set_focus(id, false);
            return;
        }
    }
    std::optional<WindowId> candidate;
    std::uint64_t highest = 0;
    for (const auto &[id, window] : windows_) {
        if (eligible(id, *workspace) && window.stacking > highest) {
            candidate = id;
            highest = window.stacking;
        }
    }
    set_focus(candidate, false);
}

bool WindowManager::click(WindowId id) {
    if (!is_visible(id)) {
        return false;
    }
    const auto output = *window_output(id);
    const auto top = fullscreen_on(output);
    if (top && *top != id && !is_descendant(id, *top)) {
        return false;
    }
    finish_focus_cycle();
    focus_on_output(output);
    auto &window = windows_.at(id);
    if (focused_window_ == id && !is_tiled(id, window)) {
        window.stacking = ++sequence_;
    }
    set_focus(id);
    return true;
}

bool WindowManager::focus_direction(Direction direction) {
    finish_focus_cycle();
    const auto home = focused_workspace();
    if (!home || direction == Direction::none) {
        return false;
    }
    if (!focused_window_) {
        restore_focus();
        return focused_window_.has_value();
    }
    const bool horizontal = direction == Direction::left || direction == Direction::right;
    const auto &layout = workspaces_.at(*home).layout;
    const bool scrolling = layout_settings_.kind == Layout::scrolling;
    const bool from_column = scrolling && layout.contains(*focused_window_);
    if (from_column && horizontal) {
        if (const auto next = layout.neighbor(*focused_window_, direction);
            next && eligible(*next, *home)) {
            set_focus(next, true, direction);
            return true;
        }
    }
    const bool can_wrap = !scrolling || !horizontal;
    const int sign = (direction == Direction::left || direction == Direction::up) ? -1 : 1;
    const auto origin = view(*focused_window_).geometry;
    const auto center = [horizontal, sign](WindowRect rect) {
        const auto x = std::int64_t{2} * rect.x + rect.width;
        const auto y = std::int64_t{2} * rect.y + rect.height;
        return horizontal ? std::pair{x * sign, y} : std::pair{y * sign, x};
    };
    const auto [from_axis, from_cross] = center(origin);
    // Prefer candidates intersecting the originating row or column, then the
    // nearest center. At the edge, pick the opposite extreme on the same axis.
    using Rank = std::tuple<bool, std::int64_t, std::int64_t, WindowId>;
    std::optional<Rank> closest;
    std::optional<std::tuple<std::int64_t, std::int64_t, WindowId>> wrap;
    for (const auto &[id, window] : windows_) {
        if (id == focused_window_ || !eligible(id, *home) || (from_column && layout.contains(id))) {
            continue;
        }
        const auto rect = view(id).geometry;
        const auto [towards, across] = center(rect);
        const auto cross = distance(across - from_cross);
        if (can_wrap) {
            const auto next = std::tuple(towards, cross, id);
            if (!wrap || next < *wrap) {
                wrap = next;
            }
        }
        if (towards > from_axis) {
            const auto spans = horizontal ? std::int64_t{origin.height} + rect.height
                                          : std::int64_t{origin.width} + rect.width;
            const auto rank = Rank{cross >= spans, towards - from_axis + cross, cross, id};
            if (!closest || rank < *closest) {
                closest = rank;
            }
        }
    }
    if (closest) {
        set_focus(std::get<3>(*closest), true, direction);
        return true;
    }
    if (wrap) {
        set_focus(std::get<2>(*wrap), true, direction);
        return true;
    }
    return false;
}

bool WindowManager::cycle_focus(bool held) {
    const auto workspace = focused_workspace();
    if (!workspace) {
        return false;
    }
    if (!cycle_) {
        FocusCycle cycle;
        const auto append = [this, workspace, &cycle](WindowId id) {
            if (eligible(id, *workspace) &&
                std::ranges::find(cycle.windows, id) == cycle.windows.end()) {
                cycle.windows.push_back(id);
            }
        };
        if (focused_window_) {
            append(*focused_window_);
        }
        for (const auto id : workspaces_.at(*workspace).focus_history) {
            append(id);
        }
        for (const auto &[id, window] : windows_) {
            append(id);
        }
        if (cycle.windows.empty()) {
            return false;
        }
        cycle.next = focused_window_ == cycle.windows.front() ? 1 % cycle.windows.size() : 0;
        cycle_ = std::move(cycle);
    }
    for (std::size_t attempt = 0; attempt < cycle_->windows.size(); ++attempt) {
        const auto id = cycle_->windows[cycle_->next];
        cycle_->next = (cycle_->next + 1) % cycle_->windows.size();
        if (eligible(id, *workspace) && id != focused_window_) {
            set_focus(id, false);
            if (!held) {
                finish_focus_cycle();
            }
            return true;
        }
    }
    if (!held) {
        finish_focus_cycle();
    }
    return false;
}

void WindowManager::finish_focus_cycle() {
    if (!cycle_) {
        return;
    }
    cycle_.reset();
    if (focused_window_ && focused_workspace()) {
        remember_focus(*focused_window_, *focused_workspace());
    }
}

bool WindowManager::move_to_workspace(WindowId id, WorkspaceId destination) {
    const auto *entry = state_.window(id);
    if (!entry || !entry->mapped || windows_.at(id).parent || !select_workspace(destination) ||
        !relocate_window(id, destination)) {
        return false;
    }
    // The explicit move gives a pinned window focus even if the destination
    // currently suppresses pinned windows behind a fullscreen client.
    if (windows_.at(id).pinned || is_visible(id)) {
        set_focus(id);
    } else {
        restore_focus();
    }
    return true;
}

bool WindowManager::set_maximized(WindowId id, bool enabled) {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return false;
    }
    cancel_move(id);
    finish_focus_cycle();
    found->second.maximized = enabled;
    if (!enabled && focused_window_ &&
        (*focused_window_ == id || is_descendant(*focused_window_, id))) {
        if (is_visible(*focused_window_)) {
            reveal(*focused_window_);
        } else {
            restore_focus();
        }
    }
    return true;
}

bool WindowManager::set_fullscreen(WindowId id, bool enabled) {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return false;
    }
    cancel_move(id);
    finish_focus_cycle();
    auto &window = found->second;
    window.fullscreen = enabled;
    const auto *entry = state_.window(id);
    const auto output = window_output(id);
    if (enabled && entry->mapped && output &&
        state_.output(*output)->shown == entry->home_workspace) {
        window.stacking = ++sequence_;
        if (focused_output_ == output) {
            set_focus(id);
        } else {
            remember_focus(id, entry->home_workspace);
        }
    } else if (focused_window_ && !is_visible(*focused_window_)) {
        restore_focus();
    } else if (!enabled && focused_window_ &&
               (*focused_window_ == id || is_descendant(*focused_window_, id))) {
        reveal(*focused_window_);
    }
    return true;
}

bool WindowManager::toggle_pin(WindowId id) {
    auto found = windows_.find(id);
    if (found == windows_.end() || !state_.window(id)->mapped || found->second.parent) {
        return false;
    }
    cancel_tile_move();
    finish_focus_cycle();
    if (found->second.pinned) {
        const auto output = window_output(id);
        const auto shown = output ? state_.output(*output)->shown : std::nullopt;
        if (shown && !relocate_window(id, *shown)) {
            return false;
        }
    }
    found->second.pinned = !found->second.pinned;
    if (found->second.pinned) {
        remove_tile(id);
    } else {
        attach_tile(id);
    }
    if (focused_window_ && !is_visible(*focused_window_)) {
        restore_focus();
    } else if (focused_window_ == id) {
        set_focus(id);
    }
    return true;
}

bool WindowManager::toggle_tiling(WindowId id) {
    auto found = windows_.find(id);
    if (found == windows_.end() || !state_.window(id)->mapped || found->second.parent ||
        found->second.pinned || found->second.fullscreen || found->second.maximized) {
        return false;
    }
    cancel_tile_move();
    finish_focus_cycle();
    const bool enabled = !is_tiled(id, found->second);
    if (!state_.set_tiling_override(id, enabled)) {
        return false;
    }
    if (enabled) {
        attach_tile(id);
        if (focused_window_ == id) {
            reveal(id);
        }
    } else {
        remove_tile(id);
        found->second.stacking = ++sequence_;
    }
    return true;
}

void WindowManager::apply_layout_config(const Config &config) {
    const auto settings = layout_settings(config);
    const bool rebuild = settings.kind != layout_settings_.kind;
    const bool changed_tiling = config.window.tiling != tiling_;
    if (settings == layout_settings_ && !changed_tiling) {
        return;
    }
    cancel_tile_move();
    if (rebuild || changed_tiling) {
        state_.clear_tiling_overrides();
    }
    tiling_ = config.window.tiling;
    layout_settings_ = settings;
    for (auto &[workspace_id, workspace] : workspaces_) {
        workspace.layout.set_settings(settings);
        if (!rebuild && !changed_tiling) {
            continue;
        }
        std::vector<WindowId> participants;
        for (const auto id : state_.workspace(workspace_id)->windows) {
            const auto *entry = state_.window(id);
            if (entry->mapped && is_tiled(id, windows_.at(id))) {
                participants.push_back(id);
            } else {
                workspace.layout.erase(id);
            }
        }
        std::ranges::sort(participants, {},
                          [this](WindowId id) { return state_.window(id)->initial_mapping_seq; });
        std::optional<WindowId> previous;
        for (const auto id : participants) {
            if (!workspace.layout.contains(id)) {
                const auto *entry = state_.window(id);
                workspace.layout.add({id, entry->initial_mapping_seq, windows_.at(id).limits},
                                     previous);
            }
            previous = id;
        }
    }
    if (focused_window_ && !is_visible(*focused_window_)) {
        restore_focus();
    } else if (focused_window_ && !rebuild) {
        reveal(*focused_window_);
    } else if (rebuild && settings.kind == Layout::scrolling && focused_window_) {
        const auto current = view(*focused_window_);
        const auto workspace = focused_workspace();
        if (current.clip.width > 0 && workspace &&
            !intersects_horizontally(current.geometry, current.clip)) {
            // A scrolling rebuild opens at the left edge. If the old focus is
            // beyond it, put focus in the first presented column.
            std::optional<std::pair<int, WindowId>> first;
            for (const auto id : state_.workspace(*workspace)->windows) {
                const auto column = view(id);
                if (column.tile_layout && column.visible &&
                    intersects_horizontally(column.geometry, current.clip)) {
                    const auto candidate = std::pair{column.geometry.x, id};
                    if (!first || candidate < *first) {
                        first = candidate;
                    }
                }
            }
            if (first) {
                set_focus(first->second);
            }
        }
    }
}

bool WindowManager::move_window(WindowId id, Direction direction, int step) {
    if (step <= 0) {
        return false;
    }
    cancel_tile_move();
    if (const auto current = view(id); current.tile_layout && current.visible) {
        finish_focus_cycle();
        const bool moved = layout_for(id).move(id, direction);
        if (moved && focused_window_ == id) {
            reveal(id, direction);
        }
        return moved;
    }
    switch (direction) {
    case Direction::left:
        return move_window_by(id, -step, 0);
    case Direction::right:
        return move_window_by(id, step, 0);
    case Direction::up:
        return move_window_by(id, 0, -step);
    case Direction::down:
        return move_window_by(id, 0, step);
    case Direction::none:
        return false;
    }
    return false;
}

bool WindowManager::move_window_by(WindowId id, int dx, int dy) {
    auto found = windows_.find(id);
    const auto output = window_output(id);
    if (found == windows_.end() || !output || !state_.window(id)->mapped) {
        return false;
    }
    if (tile_move_ && tile_move_->tile.id == id) {
        auto &rect = tile_move_->geometry;
        rect.x = coordinate(std::int64_t{rect.x} + dx);
        rect.y = coordinate(std::int64_t{rect.y} + dy);
        return true;
    }
    auto &window = found->second;
    if (window.maximized || window.fullscreen || is_tiled(id, window)) {
        return false;
    }
    finish_focus_cycle();
    const auto &area = outputs_.find(*output)->second.usable;
    const auto current = geometry(id, window);
    const auto target_x =
        fit_position(std::int64_t{current.x} - area.x + dx, current.width, area.width);
    const auto target_y =
        fit_position(std::int64_t{current.y} - area.y + dy, current.height, area.height);
    if (window.parent) {
        window.dialog_dx =
            coordinate(std::int64_t{window.dialog_dx} + target_x - current.x + area.x);
        window.dialog_dy =
            coordinate(std::int64_t{window.dialog_dy} + target_y - current.y + area.y);
    } else {
        window.ordinary.x = target_x;
        window.ordinary.y = target_y;
        window.positioned = true;
    }
    return true;
}

bool WindowManager::swap_window(WindowId id, Direction direction) {
    cancel_tile_move();
    if (!view(id).tile_layout || !is_visible(id)) {
        return false;
    }
    finish_focus_cycle();
    auto &layout = layout_for(id);
    const auto target = layout.neighbor(id, direction);
    if (!target || !view(*target).tile_layout || !layout.swap(id, *target)) {
        return false;
    }
    if (focused_window_ == id) {
        reveal(id, direction);
    }
    return true;
}

bool WindowManager::resize_window(WindowId id, Direction direction, int distance) {
    cancel_tile_move();
    if (!view(id).tile_layout || !is_visible(id)) {
        return false;
    }
    finish_focus_cycle();
    const bool resized = layout_for(id).resize(id, direction, distance);
    if (resized && focused_window_ == id) {
        reveal(id);
    }
    return resized;
}

ResizeResult WindowManager::resize_window_by(WindowId id, int dw, int dh) {
    auto found = windows_.find(id);
    const auto output = window_output(id);
    if (found == windows_.end() || !output || !state_.window(id)->mapped) {
        return ResizeResult::invalid;
    }
    auto &window = found->second;
    if (window.maximized || window.fullscreen || (tile_move_ && tile_move_->tile.id == id)) {
        return ResizeResult::invalid;
    }
    finish_focus_cycle();
    if (is_tiled(id, window)) {
        const bool resized = layout_for(id).resize_by(id, dw, dh);
        if (resized && focused_window_ == id) {
            reveal(id);
        }
        return resized ? ResizeResult::changed : ResizeResult::invalid;
    }
    const auto extent = [](int size, int delta, int minimum, int maximum, int available) {
        if (!delta) {
            return size;
        }
        const int lower = std::clamp(minimum, 1, available);
        const int upper = maximum > 0 ? std::clamp(maximum, lower, available) : available;
        return static_cast<int>(std::clamp<std::int64_t>(std::int64_t{size} + delta, lower, upper));
    };
    const auto &area = outputs_.find(*output)->second.usable;
    const auto &limits = window.limits;
    auto &rect = window.ordinary;
    const int width = extent(rect.width, dw, limits.min_width, limits.max_width, area.width);
    const int height = extent(rect.height, dh, limits.min_height, limits.max_height, area.height);
    if (width == rect.width && height == rect.height) {
        return ResizeResult::unchanged;
    }
    rect.width = width;
    rect.height = height;
    return ResizeResult::changed;
}

bool WindowManager::has_tile_resize_edge(WindowId id, Direction edge) const noexcept {
    const auto *window = state_.window(id);
    return window && workspaces_.at(window->home_workspace).layout.has_resize_edge(id, edge);
}

bool WindowManager::resize_tile_edges(WindowId id, WindowRect start, Direction horizontal, int dx,
                                      Direction vertical, int dy) {
    const auto current = view(id);
    if (!current.tile_layout || !current.visible || tile_move_) {
        return false;
    }
    finish_focus_cycle();
    const auto delta = [](int movement, int initial, int now, int initial_extent, int now_extent,
                          bool trailing) {
        auto distance = std::int64_t{initial} + movement - now;
        if (trailing) {
            distance += std::int64_t{initial_extent} - now_extent;
        }
        return coordinate(distance);
    };
    const auto box = current.geometry;
    auto &layout = layout_for(id);
    const bool x = layout.resize_edge(
        id, horizontal,
        delta(dx, start.x, box.x, start.width, box.width, horizontal == Direction::right));
    const bool y = layout.resize_edge(
        id, vertical,
        delta(dy, start.y, box.y, start.height, box.height, vertical == Direction::down));
    return x || y;
}

bool WindowManager::resize_floating(WindowId id, WindowRect start, std::optional<int> dx,
                                    std::optional<int> dy, bool from_left, bool from_top) {
    const auto output = window_output(id);
    if (!output || !is_visible(id)) {
        return false;
    }
    auto &window = windows_.at(id);
    if (is_tiled(id, window) || window.maximized || window.fullscreen) {
        return false;
    }
    const auto &area = outputs_.find(*output)->second.usable;
    const auto current = geometry(id, window);
    const auto axis = [](int position, int extent, int area_start, int area_extent, int delta,
                         bool near, int minimum, int maximum) {
        const std::int64_t anchor = std::int64_t{position} + (near ? extent : 0);
        const std::int64_t available =
            near ? anchor - area_start : std::int64_t{area_start} + area_extent - anchor;
        int upper = static_cast<int>(std::clamp<std::int64_t>(available, 1, area_extent));
        if (maximum > 0) {
            upper = std::min(upper, maximum);
        }
        const int lower = std::clamp(minimum, 1, upper);
        const auto change = near ? -std::int64_t{delta} : std::int64_t{delta};
        const int desired =
            static_cast<int>(std::clamp<std::int64_t>(extent + change, lower, upper));
        return std::pair{near ? coordinate(anchor - desired) : position, desired};
    };
    const auto [x, width] = dx ? axis(start.x, start.width, area.x, area.width, *dx, from_left,
                                      window.limits.min_width, window.limits.max_width)
                               : std::pair{current.x, current.width};
    const auto [y, height] = dy ? axis(start.y, start.height, area.y, area.height, *dy, from_top,
                                       window.limits.min_height, window.limits.max_height)
                                : std::pair{current.y, current.height};
    window.ordinary.width = width;
    window.ordinary.height = height;
    const auto resized = geometry(id, window);
    move_window_by(id, coordinate(std::int64_t{x} - resized.x),
                   coordinate(std::int64_t{y} - resized.y));
    return geometry(id, window) != current;
}

bool WindowManager::begin_move(WindowId id) {
    const auto current = view(id);
    if (!current.visible || current.maximized || current.fullscreen || tile_move_) {
        return false;
    }
    if (!current.tile_layout) {
        return true;
    }
    auto &layout = layout_for(id);
    auto previous = layout;
    const auto tile = layout.erase(id);
    if (!tile) {
        return false;
    }
    tile_move_.emplace(TileMove{state_.window(id)->home_workspace, *tile, std::move(previous),
                                current.geometry, current.border_width, ++sequence_});
    return true;
}

bool WindowManager::finish_move(WindowId id, double x, double y) {
    if (!tile_move_ || tile_move_->tile.id != id) {
        return false;
    }
    auto &layout = layout_for(id);
    const auto drop = layout.drop_at(x, y);
    if (!drop || !layout.add(tile_move_->tile, {}, drop)) {
        cancel_tile_move();
        return false;
    }
    tile_move_.reset();
    if (focused_window_ == id) {
        reveal(id);
    }
    return true;
}

void WindowManager::cancel_move(WindowId id) {
    if (tile_move_ && tile_move_->tile.id == id) {
        cancel_tile_move();
    }
}

void WindowManager::cancel_tile_move() {
    if (tile_move_) {
        workspaces_.at(tile_move_->workspace).layout = std::move(tile_move_->previous);
        tile_move_.reset();
        if (focused_window_) {
            reveal(*focused_window_);
        }
    }
}

} // namespace seaside

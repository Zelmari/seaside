#include "window_manager.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace seaside {
namespace {

constexpr WorkspaceId max_workspace_id = 2147483647U;

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

bool contains(WindowRect rectangle, double x, double y) noexcept {
    return x >= rectangle.x && y >= rectangle.y &&
           x < static_cast<double>(rectangle.x) + rectangle.width &&
           y < static_cast<double>(rectangle.y) + rectangle.height;
}

int fit_position(std::int64_t position, int extent, int available) noexcept {
    return static_cast<int>(std::clamp<std::int64_t>(position, 0, std::max(0, available - extent)));
}

} // namespace

WindowManager::WindowManager(WorkspaceMode mode, bool tiling) noexcept
    : mode_(mode), tiling_(tiling) {}

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
    return true;
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
    if (!primary_output_ || primary) {
        primary_output_ = name;
    }
    if (!focused_output_ || primary) {
        return focus_on_output(name);
    }
    restore_focus();
    return true;
}

bool WindowManager::remove_output(std::string_view id) {
    auto position = outputs_.find(id);
    if (position == outputs_.end() || !position->second.connected) {
        return false;
    }
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
            if (entry.connected && (!fallback || entry.last_focused > last_focused)) {
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
    found->second.bounds = bounds;
    found->second.usable = usable;
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

bool WindowManager::focus_on_output(std::string_view id) {
    auto found = outputs_.find(id);
    if (found == outputs_.end() || !found->second.connected) {
        return false;
    }
    const auto previous = focused_output_;
    focused_output_ = found->first;
    found->second.last_focused = ++sequence_;
    if (previous != focused_output_) {
        restore_focus();
    }
    return true;
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

bool WindowManager::select_workspace(WorkspaceId id) {
    if (!focused_output_) {
        return false;
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
    if (!show_workspace(*output, id) || !focus_on_output(*output)) {
        return false;
    }
    restore_focus();
    return true;
}

bool WindowManager::transfer_workspace(std::string_view destination) {
    auto workspace = focused_workspace();
    if (!workspace || destination == focused_output_ || !state_.output(destination)) {
        return false;
    }
    finish_focus_cycle();
    const auto source = *focused_output_;
    if (!state_.assign_workspace(*workspace, destination) ||
        !show_workspace(destination, *workspace) || !show_fallback(source)) {
        return false;
    }
    workspaces_.at(*workspace).migration.reset();
    return focus_on_output(destination);
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

bool WindowManager::map_window(WindowId id, WindowRect committed) {
    if (!state_.map_window(id)) {
        return false;
    }
    auto &window = windows_.at(id);
    window.stacking = ++sequence_;
    if (!window.fullscreen && !window.maximized) {
        commit_geometry(id, committed);
    }
    position_window(id, window);

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
    if (!state_.unmap_window(id)) {
        return false;
    }
    forget_focus(id, state_.window(id)->home_workspace);
    auto &window = windows_.at(id);
    // The next map starts a fresh XDG toplevel state, while its ID and original
    // mapping order belong to the lifetime of the Wayland object.
    window.maximized = false;
    window.fullscreen = false;
    window.pinned = false;
    (void)state_.set_tiling_override(id, {});
    if (focused_window_ == id || (focused_window_ && !is_visible(*focused_window_))) {
        restore_focus();
    }
    return true;
}

bool WindowManager::destroy_window(WindowId id) {
    const auto *entry = state_.window(id);
    if (!entry) {
        return false;
    }
    const auto parent = windows_.at(id).parent;
    for (auto &[child, window] : windows_) {
        if (window.parent == id) {
            set_parent(child, parent);
        }
    }
    forget_focus(id, entry->home_workspace);
    if (!state_.destroy_window(id)) {
        return false;
    }
    windows_.erase(id);
    if (focused_window_ == id || (focused_window_ && !is_visible(*focused_window_))) {
        restore_focus();
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
    for (const auto &[member, window] : windows_) {
        if (member != id && !is_descendant(member, id)) {
            continue;
        }
        const auto old = state_.window(member)->home_workspace;
        if (!state_.move_window(member, workspace)) {
            return false;
        }
        if (old != workspace) {
            forget_focus(member, old);
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
    if (!parent && window.parent) {
        if (const auto output = window_output(id)) {
            const auto current = geometry(id, window);
            const auto &usable = outputs_.find(*output)->second.usable;
            window.ordinary.x = coordinate(std::int64_t{current.x} - usable.x);
            window.ordinary.y = coordinate(std::int64_t{current.y} - usable.y);
        }
    }
    if (parent) {
        if (!relocate_window(id, state_.window(*parent)->home_workspace)) {
            return false;
        }
        window.pinned = false;
        window.dialog_dx = 0;
        window.dialog_dy = 0;
    }
    window.parent = parent;
    if (focused_window_ &&
        (!is_visible(*focused_window_) || window_output(*focused_window_) != focused_output_)) {
        restore_focus();
    }
    return true;
}

bool WindowManager::commit_geometry(WindowId id, WindowRect committed) {
    auto position = windows_.find(id);
    if (position == windows_.end()) {
        return false;
    }
    auto &window = position->second;
    if (window.fullscreen || window.maximized) {
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
        return display.usable;
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
    ordinary.x = coordinate(std::int64_t{usable.x} + fit_position(x, ordinary.width, usable.width));
    ordinary.y =
        coordinate(std::int64_t{usable.y} + fit_position(y, ordinary.height, usable.height));
    return ordinary;
}

std::optional<WindowId> WindowManager::fullscreen_on(std::string_view output) const {
    const auto shown = state_.output(output)->shown;
    if (!shown) {
        return {};
    }
    // A newly focused ordinary window must remain visible when it opens over
    // a fullscreen client. Keep the client's fullscreen configure and promote
    // its stack again when the user focuses it or one of its dialogs.
    const auto valid_focus = [&](WindowId id) {
        const auto *entry = state_.window(id);
        return entry && entry->mapped && entry->home_workspace == shown;
    };
    std::optional<WindowId> local_focus;
    if (focused_output_ == output && focused_window_ && valid_focus(*focused_window_)) {
        local_focus = focused_window_;
    } else {
        const auto &history = workspaces_.at(*shown).focus_history;
        if (const auto previous = std::ranges::find_if(history, valid_focus);
            previous != history.end()) {
            local_focus = *previous;
        }
    }
    if (local_focus) {
        for (auto current = local_focus; current;) {
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

WindowView WindowManager::view(WindowId id) const {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return {};
    }
    const auto &window = found->second;
    auto stacking = window.stacking;
    if (window.fullscreen || window.parent) {
        if (const auto output = window_output(id)) {
            if (const auto fullscreen = fullscreen_on(*output);
                fullscreen && (*fullscreen == id || is_descendant(id, *fullscreen))) {
                // Scene order matters even if a fullscreen client submits a
                // smaller buffer than its requested output rectangle.
                stacking |= std::uint64_t{1} << 63;
            } else if (window.fullscreen) {
                stacking = 0;
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
        .stacking = stacking,
    };
}

void WindowManager::remember_focus(WindowId id) {
    const auto *window = state_.window(id);
    if (window && window->mapped) {
        auto &history = workspaces_.at(window->home_workspace).focus_history;
        const auto previous = std::ranges::find(history, id);
        if (previous != history.end()) {
            std::rotate(history.begin(), previous, previous + 1);
        } else {
            history.insert(history.begin(), id);
        }
    }
}

void WindowManager::forget_focus(WindowId id, WorkspaceId workspace) {
    auto &history = workspaces_.at(workspace).focus_history;
    std::erase(history, id);
}

void WindowManager::set_focus(std::optional<WindowId> id, bool remember) {
    if (id && remember && state_.window(*id)->home_workspace == focused_workspace()) {
        remember_focus(*id);
    }
    focused_window_ = id;
}

bool WindowManager::eligible(WindowId id, WorkspaceId workspace) const {
    const auto *entry = state_.window(id);
    return entry && entry->home_workspace == workspace && is_visible(id);
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
    for (const auto id : state_.workspace(*workspace)->windows) {
        if (eligible(id, *workspace) && windows_.at(id).stacking > highest) {
            candidate = id;
            highest = windows_.at(id).stacking;
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
    if (!is_tiled(id, window)) {
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
    for (const auto id : state_.workspace(*home)->windows) {
        if (id == focused_window_ || !eligible(id, *home)) {
            continue;
        }
        const auto rect = view(id).geometry;
        const auto [towards, across] = center(rect);
        const auto cross = distance(across - from_cross);
        const auto next = std::tuple(towards, cross, id);
        if (!wrap || next < *wrap) {
            wrap = next;
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
        set_focus(std::get<3>(*closest));
        return true;
    }
    if (wrap) {
        set_focus(std::get<2>(*wrap));
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
        for (const auto id : state_.workspace(*workspace)->windows) {
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
    if (focused_window_ && state_.window(*focused_window_)->home_workspace == focused_workspace()) {
        remember_focus(*focused_window_);
    }
}

bool WindowManager::move_to_workspace(WindowId id, WorkspaceId destination) {
    const auto *entry = state_.window(id);
    if (!entry || !entry->mapped || windows_.at(id).parent || !select_workspace(destination) ||
        !relocate_window(id, destination)) {
        return false;
    }
    set_focus(id);
    return true;
}

bool WindowManager::set_maximized(WindowId id, bool enabled) {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return false;
    }
    finish_focus_cycle();
    found->second.maximized = enabled;
    return true;
}

bool WindowManager::set_fullscreen(WindowId id, bool enabled) {
    auto found = windows_.find(id);
    if (found == windows_.end()) {
        return false;
    }
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
            remember_focus(id);
        }
    } else if (focused_window_ && !is_visible(*focused_window_)) {
        restore_focus();
    }
    return true;
}

bool WindowManager::toggle_pin(WindowId id) {
    auto found = windows_.find(id);
    if (found == windows_.end() || !state_.window(id)->mapped || found->second.parent) {
        return false;
    }
    finish_focus_cycle();
    found->second.pinned = !found->second.pinned;
    if (focused_window_ && !is_visible(*focused_window_)) {
        restore_focus();
    }
    return true;
}

bool WindowManager::toggle_tiling(WindowId id) {
    auto found = windows_.find(id);
    if (found == windows_.end() || !state_.window(id)->mapped || found->second.parent ||
        found->second.pinned || found->second.fullscreen || found->second.maximized) {
        return false;
    }
    finish_focus_cycle();
    return state_.set_tiling_override(id, !is_tiled(id, found->second)).has_value();
}

bool WindowManager::move_window(WindowId id, Direction direction, int step) {
    if (step <= 0) {
        return false;
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

bool WindowManager::resize_window_by(WindowId id, int dw, int dh) {
    auto found = windows_.find(id);
    const auto output = window_output(id);
    if (found == windows_.end() || !output || !state_.window(id)->mapped) {
        return false;
    }
    auto &window = found->second;
    if (window.maximized || window.fullscreen || is_tiled(id, window)) {
        return false;
    }
    finish_focus_cycle();
    const auto &area = outputs_.find(*output)->second.usable;
    if (dw) {
        window.ordinary.width = static_cast<int>(
            std::clamp<std::int64_t>(std::int64_t{window.ordinary.width} + dw, 1, area.width));
    }
    if (dh) {
        window.ordinary.height = static_cast<int>(
            std::clamp<std::int64_t>(std::int64_t{window.ordinary.height} + dh, 1, area.height));
    }
    return true;
}

} // namespace seaside

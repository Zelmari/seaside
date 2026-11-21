#include "desktop.hpp"
#include "tty_input.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>

namespace seaside {
namespace {
seaside_scene_rect to_scene(WindowRect rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

WindowRect from_scene(seaside_scene_rect rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

int motion_delta(double delta) noexcept {
    return static_cast<int>(std::clamp(std::trunc(delta), double{std::numeric_limits<int>::min()},
                                       double{std::numeric_limits<int>::max()}));
}

int resize_delta(int size, int delta, int minimum, int maximum) noexcept {
    if (delta == 0) {
        return 0;
    }
    const auto lower = std::max(1, minimum);
    const auto upper = maximum ? std::max(lower, maximum) : std::numeric_limits<int>::max();
    const auto desired = std::clamp<std::int64_t>(std::int64_t{size} + delta, lower, upper);
    return static_cast<int>(desired - size);
}
} // namespace

Desktop::Desktop(Scene &scene, const Config &config)
    : scene_(scene), windows_(config.workspaces.mode, config.window.tiling),
      bindings_(config.keybinds), primary_(config.monitors.primary) {
    scene_.set_window_handler(on_window, this);
}

Desktop::~Desktop() noexcept {
    scene_.set_window_handler(nullptr, nullptr);
}

void Desktop::set_input(seaside_tty_input *input) noexcept {
    input_ = input;
}

bool Desktop::add_output(wlr_output *output, std::string_view connector, std::string_view identity,
                         WindowRect bounds) noexcept try {
    auto [record, inserted] =
        outputs_.try_emplace(output, Output{std::string(connector), std::string(identity)});
    if (!inserted) {
        return false;
    }
    if (!scene_.add_output(output) || !scene_.place_output(output, bounds.x, bounds.y)) {
        scene_.remove_output(output);
        outputs_.erase(record);
        return false;
    }
    const bool primary = primary_ && match(*primary_) == connector;
    if (!windows_.add_output(connector, bounds, bounds, primary)) {
        scene_.remove_output(output);
        outputs_.erase(record);
        return false;
    }
    project();
    return true;
} catch (...) {
    failed_ = true;
    return false;
}

void Desktop::remove_output(wlr_output *output) noexcept try {
    auto record = outputs_.find(output);
    if (record != outputs_.end()) {
        scene_.remove_output(output);
        windows_.remove_output(record->second.connector);
        outputs_.erase(record);
        project();
    }
} catch (...) {
    failed_ = true;
}

wlr_output *Desktop::focused_output_handle() const noexcept {
    const auto focused = windows_.focused_output();
    for (const auto &[handle, output] : outputs_) {
        if (output.connector == focused) {
            return handle;
        }
    }
    return nullptr;
}

WindowRect Desktop::output_bounds(wlr_output *output) const noexcept {
    const auto entry = outputs_.find(output);
    return entry == outputs_.end()
               ? WindowRect{}
               : windows_.output_bounds(entry->second.connector).value_or(WindowRect{});
}

std::optional<std::string_view> Desktop::match(const OutputSelector &selector) const noexcept {
    std::optional<std::string_view> found;
    for (const auto &[handle, output] : outputs_) {
        const auto &value =
            selector.kind == SelectorKind::connector ? output.connector : output.identity;
        if (selector.value == value) {
            if (found) {
                return std::nullopt;
            }
            found = output.connector;
        }
    }
    return found;
}

void Desktop::project(WindowId resize) {
    const WindowId focused = windows_.focused_window().value_or(0);
    for (const auto id : views_) {
        const auto view = windows_.view(id);
        const auto output = windows_.window_output(id);
        scene_.place_window(
            id,
            {
                .geometry = to_scene(view.geometry),
                .output = output ? to_scene(windows_.output_bounds(*output).value_or(WindowRect{}))
                                 : seaside_scene_rect{},
                .stacking = view.stacking,
                .visible = view.visible,
                .activated = focused == id,
                .maximized = view.maximized,
                .fullscreen = view.fullscreen,
                .resize = id == resize,
            });
    }
    if (drag_ && !windows_.view(drag_->window).visible) {
        drag_.reset();
    }
    if (input_) {
        seaside_tty_input_set_focus(input_, scene_.window_surface(focused));
        seaside_tty_input_refresh_pointer(input_);
    }
}

std::uint64_t Desktop::on_window(void *self, const seaside_scene_window_event *event) noexcept {
    auto &desktop = *static_cast<Desktop *>(self);
    try {
        return desktop.window_event(*event);
    } catch (...) {
        desktop.failed_ = true;
        return 0;
    }
}

std::uint64_t Desktop::window_event(const seaside_scene_window_event &event) {
    const auto parent = event.parent ? std::optional{event.parent} : std::nullopt;
    switch (event.kind) {
    case SEASIDE_WINDOW_CREATED: {
        const auto id = windows_.create_window(parent);
        if (!id) {
            return 0;
        }
        views_.insert(*id);
        return *id;
    }
    case SEASIDE_WINDOW_INITIAL_COMMIT:
        windows_.initialize_window(event.id);
        break;
    case SEASIDE_WINDOW_MAPPED:
        windows_.map_window(event.id, from_scene(event.geometry));
        break;
    case SEASIDE_WINDOW_UNMAPPED:
        windows_.unmap_window(event.id);
        break;
    case SEASIDE_WINDOW_DESTROYED:
        views_.erase(event.id);
        windows_.destroy_window(event.id);
        break;
    case SEASIDE_WINDOW_COMMITTED: {
        const auto current = windows_.view(event.id);
        if (!event.ordinary_commit || current.maximized || current.fullscreen ||
            (current.geometry.width == event.geometry.width &&
             current.geometry.height == event.geometry.height)) {
            return 0;
        }
        windows_.commit_geometry(event.id, from_scene(event.geometry));
        break;
    }
    case SEASIDE_WINDOW_PARENT_CHANGED:
        windows_.set_parent(event.id, parent);
        break;
    case SEASIDE_WINDOW_MAXIMIZE_REQUESTED:
        windows_.set_maximized(event.id, event.enabled);
        break;
    case SEASIDE_WINDOW_FULLSCREEN_REQUESTED:
        windows_.set_fullscreen(event.id, event.enabled);
        break;
    }
    project();
    return 0;
}

bool Desktop::resize(WindowId id, Direction direction, int distance) {
    if (distance <= 0) {
        return false;
    }
    switch (direction) {
    case Direction::left:
        return resize_by(id, -distance, 0);
    case Direction::right:
        return resize_by(id, distance, 0);
    case Direction::up:
        return resize_by(id, 0, -distance);
    case Direction::down:
        return resize_by(id, 0, distance);
    case Direction::none:
        return false;
    }
    return false;
}

bool Desktop::resize_by(WindowId id, int dx, int dy) {
    const auto size = windows_.view(id).geometry;
    const auto limits = scene_.window_size_limits(id);
    return windows_.resize_window_by(
        id, resize_delta(size.width, dx, limits.min_width, limits.max_width),
        resize_delta(size.height, dy, limits.min_height, limits.max_height));
}

bool Desktop::action(const Action &action, std::uint8_t modifiers) noexcept try {
    if (action.kind != ActionKind::cycle_focus_mru) {
        finish_focus_cycle();
    }
    const auto id = windows_.focused_window().value_or(0);
    WindowId resized = 0;
    switch (action.kind) {
    case ActionKind::focus:
        windows_.focus_direction(action.direction);
        break;
    case ActionKind::cycle_focus_mru:
        cycle_modifiers_ = modifiers;
        windows_.cycle_focus(modifiers != 0);
        break;
    case ActionKind::workspace:
        windows_.select_workspace(static_cast<WorkspaceId>(action.argument));
        break;
    case ActionKind::move_to_workspace:
        windows_.move_to_workspace(id, static_cast<WorkspaceId>(action.argument));
        break;
    case ActionKind::transfer_workspace:
        if (auto selector = parse_output_selector(action.text)) {
            if (auto destination = match(*selector)) {
                windows_.transfer_workspace(*destination);
            }
        }
        break;
    case ActionKind::toggle_window_pin:
        windows_.toggle_pin(id);
        break;
    case ActionKind::toggle_window_tiling:
        windows_.toggle_tiling(id);
        break;
    case ActionKind::toggle_window_maximize:
        windows_.set_maximized(id, !windows_.view(id).maximized);
        break;
    case ActionKind::toggle_window_fullscreen:
        windows_.set_fullscreen(id, !windows_.view(id).fullscreen);
        break;
    case ActionKind::close_window:
        scene_.close_window(id);
        return true;
    case ActionKind::move_window:
        windows_.move_window(id, action.direction);
        break;
    case ActionKind::resize_window:
        if (resize(id, action.direction, static_cast<int>(action.argument))) {
            resized = id;
        }
        break;
    case ActionKind::swap_window:
    case ActionKind::night_light:
    case ActionKind::exit:
    case ActionKind::exec:
        return false;
    }
    project(resized);
    return true;
} catch (...) {
    failed_ = true;
    return true;
}

void Desktop::modifiers_changed(std::uint8_t modifiers) noexcept {
    if ((modifiers & cycle_modifiers_) != cycle_modifiers_) {
        finish_focus_cycle();
    }
}

void Desktop::finish_focus_cycle() noexcept try {
    cycle_modifiers_ = 0;
    windows_.finish_focus_cycle();
} catch (...) {
    failed_ = true;
}

bool Desktop::button(WindowId id, std::uint32_t button, bool pressed, std::uint8_t modifiers,
                     double x, double y) noexcept try {
    if (!pressed) {
        if (drag_ && drag_->button == button) {
            drag_.reset();
        }
        return false;
    }
    if (id) {
        windows_.click(id);
    } else {
        windows_.focus_output_at(x, y);
    }
    project();
    if (!id || drag_ || (button != BTN_LEFT && button != BTN_RIGHT)) {
        return false;
    }
    const auto chord =
        button == BTN_LEFT ? ChordKind::mouse_left_drag : ChordKind::mouse_right_drag;
    const auto binding = bindings_.find({modifiers, chord, 0});
    const auto view = windows_.view(id);
    if (binding == bindings_.end() || !view.visible || view.tiled || view.maximized ||
        view.fullscreen) {
        return false;
    }
    drag_ = Drag{id, button, binding->second.kind == ActionKind::resize_window, x, y};
    return true;
} catch (...) {
    failed_ = true;
    return false;
}

void Desktop::motion(double x, double y) noexcept try {
    if (!drag_ || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto dx = motion_delta(x - drag_->x);
    const auto dy = motion_delta(y - drag_->y);
    if (!dx && !dy) {
        return;
    }
    drag_->x += dx;
    drag_->y += dy;
    const WindowId id = drag_->window;
    if (drag_->resize) {
        if (resize_by(id, dx, dy)) {
            project(id);
        }
    } else if (windows_.move_window_by(id, dx, dy)) {
        project();
    }
} catch (...) {
    failed_ = true;
}

} // namespace seaside

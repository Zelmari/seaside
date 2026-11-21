#include "desktop.hpp"
#include "tty_input.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <utility>

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
    : scene_(scene), windows_(config), bindings_(config.keybinds),
      primary_(config.monitors.primary) {
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
        cancel_drag();
        return false;
    }
    discard_drag();
    project();
    return true;
} catch (...) {
    failed_ = true;
    return false;
}

void Desktop::remove_output(wlr_output *output) noexcept try {
    auto record = outputs_.find(output);
    if (record != outputs_.end()) {
        discard_drag();
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
    if (drag_ && !windows_.view(drag_->window).visible) {
        discard_drag();
    }
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
                .clip = to_scene(view.clip),
                .stacking = view.stacking,
                .border_width = view.border_width,
                .visible = view.visible,
                .activated = focused == id,
                .maximized = view.maximized,
                .fullscreen = view.fullscreen,
                .tiled = view.tile_layout,
                .resize = id == resize,
                .resizing = drag_ && drag_->started && drag_->resize && drag_->window == id,
            });
    }
    if (input_) {
        seaside_tty_input_set_focus(input_, scene_.window_surface(focused));
        seaside_tty_input_refresh_pointer(input_);
    }
}

bool Desktop::update_size_limits(WindowId id) {
    const auto limits = scene_.window_size_limits(id);
    return windows_.set_size_limits(id, limits.min_width, limits.min_height, limits.max_width,
                                    limits.max_height);
}

void Desktop::discard_drag() {
    if (const auto drag = std::exchange(drag_, std::nullopt);
        drag && drag->started && !drag->resize) {
        windows_.cancel_move(drag->window);
    }
}

void Desktop::cancel_drag() noexcept try {
    if (drag_) {
        discard_drag();
        project();
    }
} catch (...) {
    failed_ = true;
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
        update_size_limits(event.id);
        windows_.initialize_window(event.id);
        break;
    case SEASIDE_WINDOW_MAPPED:
        update_size_limits(event.id);
        windows_.map_window(event.id, from_scene(event.geometry));
        break;
    case SEASIDE_WINDOW_UNMAPPED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        windows_.unmap_window(event.id);
        break;
    case SEASIDE_WINDOW_DESTROYED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        views_.erase(event.id);
        windows_.destroy_window(event.id);
        break;
    case SEASIDE_WINDOW_COMMITTED: {
        const bool limits_changed = update_size_limits(event.id);
        const auto current = windows_.view(event.id);
        if (!event.ordinary_commit || current.tile_layout || current.maximized ||
            current.fullscreen ||
            (current.tiled && drag_ && drag_->started && !drag_->resize &&
             drag_->window == event.id) ||
            (current.geometry.width == event.geometry.width &&
             current.geometry.height == event.geometry.height)) {
            if (limits_changed) {
                project();
            }
            return 0;
        }
        windows_.commit_geometry(event.id, from_scene(event.geometry));
        break;
    }
    case SEASIDE_WINDOW_PARENT_CHANGED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        windows_.set_parent(event.id, parent);
        break;
    case SEASIDE_WINDOW_MAXIMIZE_REQUESTED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        windows_.set_maximized(event.id, event.enabled);
        break;
    case SEASIDE_WINDOW_FULLSCREEN_REQUESTED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
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
    if (windows_.view(id).tile_layout) {
        return windows_.resize_window(id, direction, distance);
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
    const auto view = windows_.view(id);
    if (view.tile_layout) {
        return windows_.resize_window_by(id, dx, dy);
    }
    const auto size = view.geometry;
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
        discard_drag();
        windows_.select_workspace(static_cast<WorkspaceId>(action.argument));
        break;
    case ActionKind::move_to_workspace:
        discard_drag();
        windows_.move_to_workspace(id, static_cast<WorkspaceId>(action.argument));
        break;
    case ActionKind::transfer_workspace:
        if (auto selector = parse_output_selector(action.text)) {
            if (auto destination = match(*selector)) {
                discard_drag();
                windows_.transfer_workspace(*destination);
            }
        }
        break;
    case ActionKind::toggle_window_pin:
        discard_drag();
        windows_.toggle_pin(id);
        break;
    case ActionKind::toggle_window_tiling:
        discard_drag();
        windows_.toggle_tiling(id);
        break;
    case ActionKind::toggle_window_maximize:
        discard_drag();
        windows_.set_maximized(id, !windows_.view(id).maximized);
        break;
    case ActionKind::toggle_window_fullscreen:
        discard_drag();
        windows_.set_fullscreen(id, !windows_.view(id).fullscreen);
        break;
    case ActionKind::close_window:
        scene_.close_window(id);
        return true;
    case ActionKind::move_window:
        discard_drag();
        windows_.move_window(id, action.direction);
        break;
    case ActionKind::resize_window:
        discard_drag();
        if (resize(id, action.direction, static_cast<int>(action.argument))) {
            resized = id;
        }
        break;
    case ActionKind::swap_window:
        discard_drag();
        windows_.swap_window(id, action.direction);
        break;
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
            const auto drag = *drag_;
            drag_.reset();
            if (drag.started && !drag.resize) {
                windows_.finish_move(drag.window, x, y);
            }
            if (drag.started) {
                project();
            }
        }
        return false;
    }
    if (drag_) {
        return false;
    }
    if (id) {
        windows_.click(id);
    } else {
        windows_.focus_output_at(x, y);
    }
    if (!id || (button != BTN_LEFT && button != BTN_RIGHT) || !std::isfinite(x) ||
        !std::isfinite(y)) {
        project();
        return false;
    }
    const auto chord =
        button == BTN_LEFT ? ChordKind::mouse_left_drag : ChordKind::mouse_right_drag;
    const auto binding = bindings_.find({modifiers, chord, 0});
    const auto view = windows_.view(id);
    if (binding == bindings_.end() || !view.visible || view.maximized || view.fullscreen) {
        project();
        return false;
    }
    drag_ = Drag{id, button, binding->second.kind == ActionKind::resize_window, x, y};
    project();
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
            drag_->started = true;
            project(id);
        }
    } else {
        if (!drag_->started) {
            if (!windows_.begin_move(id)) {
                discard_drag();
                return;
            }
            drag_->started = true;
        }
        if (windows_.move_window_by(id, dx, dy)) {
            project();
        }
    }
} catch (...) {
    failed_ = true;
}

} // namespace seaside

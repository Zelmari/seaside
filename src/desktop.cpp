#include "desktop.hpp"
#include "tty_input.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <linux/input-event-codes.h>
#include <utility>
#include <wayland-server-core.h>

namespace seaside {
namespace {
constexpr int opening_timeout_ms = 90;

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

bool same_size(WindowRect first, WindowRect second) noexcept {
    return first.width == second.width && first.height == second.height;
}

bool valid_size(WindowRect rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

bool includes_serial(std::uint32_t committed, std::uint32_t requested) noexcept {
    return requested == 0 || (committed && committed - requested < 0x80000000U);
}
} // namespace

Desktop::Desktop(Scene &scene, const Config &config)
    : scene_(scene), windows_(config), bindings_(config.keybinds),
      fullscreen_aspect_(config.render.fullscreen_aspect) {
    scene_.set_window_handler(on_window, this);
    scene_.set_layer_handler(
        [](void *self) { return static_cast<Desktop *>(self)->focused_output_handle(); },
        on_layer_changed,
        [](void *self, wlr_surface *root) {
            seaside_tty_input_cancel_drag_from(static_cast<Desktop *>(self)->input_, root);
        },
        this);
}

Desktop::~Desktop() noexcept {
    if (opening_deadline_) {
        wl_event_source_remove(opening_deadline_);
    }
    if (frame_paced_resize_) {
        scene_.set_before_frame(nullptr, nullptr);
    }
    scene_.set_window_handler(nullptr, nullptr);
    scene_.set_layer_handler(nullptr, nullptr, nullptr, nullptr);
}

void Desktop::set_input(seaside_tty_input *input) noexcept {
    input_ = input;
}

void Desktop::enable_frame_paced_resize() noexcept {
    frame_paced_resize_ = true;
    scene_.set_before_frame(before_output_frame, this);
}

wlr_output *Desktop::window_output_handle(WindowId id) const noexcept {
    const auto output = windows_.window_output(id);
    const auto found = std::ranges::find_if(
        outputs_, [&](const auto &entry) { return entry.second.id == output; });
    return found == outputs_.end() ? nullptr : found->first;
}

bool Desktop::add_output(wlr_output *output, std::string_view connector, std::string_view identity,
                         WindowRect bounds, std::string_view id) noexcept try {
    auto [record, inserted] =
        outputs_.try_emplace(output, Output{std::string(id.empty() ? connector : id),
                                            std::string(connector), std::string(identity), bounds});
    if (!inserted) {
        return false;
    }
    if (!scene_.add_output(output) || !scene_.place_output(output, bounds.x, bounds.y)) {
        scene_.remove_output(output);
        outputs_.erase(record);
        return false;
    }
    if (!windows_.add_output(record->second.id, bounds, bounds)) {
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

void Desktop::remove_output(wlr_output *output, std::string_view ranked_fallback) noexcept try {
    auto record = outputs_.find(output);
    if (record != outputs_.end()) {
        discard_drag();
        scene_.remove_output(output);
        windows_.remove_output(record->second.id, ranked_fallback);
        outputs_.erase(record);
        project();
    }
} catch (...) {
    failed_ = true;
}

bool Desktop::update_output(wlr_output *output, WindowRect bounds) noexcept try {
    const auto found = outputs_.find(output);
    if (found == outputs_.end()) {
        return false;
    }
    discard_drag();
    if (!scene_.place_output(output, bounds.x, bounds.y)) {
        return false;
    }
    const auto work = from_scene(scene_.layer_work_area(output, to_scene(bounds),
                                                        windows_.has_fullscreen(found->second.id)));
    if (!windows_.update_output(found->second.id, bounds, work)) {
        return false;
    }
    found->second.work = work;
    project();
    return true;
} catch (...) {
    failed_ = true;
    return false;
}

void Desktop::apply_initial_config(const Config &config) noexcept {
    windows_.set_initial_workspace_mode(config.workspaces.mode);
    apply_config(config);
}

void Desktop::apply_config(const Config &config) noexcept try {
    auto bindings = config.keybinds;
    if (bindings != bindings_) {
        discard_drag();
        finish_focus_cycle();
        bindings_.swap(bindings);
    }
    fullscreen_aspect_ = config.render.fullscreen_aspect;
    windows_.apply_layout_config(config);
    project();
} catch (...) {
    failed_ = true;
}

wlr_output *Desktop::focused_output_handle() const noexcept {
    const auto focused = windows_.focused_output();
    for (const auto &[handle, output] : outputs_) {
        if (output.id == focused) {
            return handle;
        }
    }
    return nullptr;
}

WindowRect Desktop::output_bounds(wlr_output *output) const noexcept {
    const auto entry = outputs_.find(output);
    return entry == outputs_.end()
               ? WindowRect{}
               : windows_.output_bounds(entry->second.id).value_or(WindowRect{});
}

bool Desktop::has_fullscreen(wlr_output *output) const noexcept {
    const auto entry = outputs_.find(output);
    return entry != outputs_.end() && windows_.has_fullscreen(entry->second.id);
}

void Desktop::on_layer_changed(void *self) noexcept {
    auto &desktop = *static_cast<Desktop *>(self);
    try {
        desktop.project();
    } catch (...) {
        desktop.failed_ = true;
    }
}

void Desktop::project(WindowId resize) {
    if (!opening_.empty()) {
        opening_.clear();
        wl_event_source_timer_update(opening_deadline_, 0);
    }
    if (drag_) {
        const auto view = windows_.view(drag_->window);
        if (!view.visible || (drag_->client && view.tiled)) {
            discard_drag();
        }
    }
    for (auto &[handle, record] : outputs_) {
        const auto bounds = windows_.output_bounds(record.id);
        if (!bounds) {
            continue;
        }
        const auto work = from_scene(
            scene_.layer_work_area(handle, to_scene(*bounds), windows_.has_fullscreen(record.id)));
        if (record.work != work) {
            discard_drag();
            if (!windows_.update_output(record.id, *bounds, work)) {
                failed_ = true;
                return;
            }
            record.work = work;
        }
    }
    const WindowId focused = windows_.focused_window().value_or(0);
    const auto *resized = windows_.state().window(resize);
    const auto *dragged =
        drag_ && drag_->started && drag_->resize ? windows_.state().window(drag_->window) : nullptr;
    auto *layer_focus = scene_.layer_keyboard_focus(focused_output_handle());
    layer_has_focus_ = layer_focus != nullptr;
    for (const auto id : views_) {
        const auto view = windows_.view(id);
        const auto output = windows_.window_output(id);
        auto geometry = view.geometry;
        const auto shown = presented_.find(id);
        // On its first transition out of tiling the client may have no saved
        // ordinary size. Keep it in place while asking it for that size.
        const bool choose_floating_size =
            view.visible && !view.tile_layout && !valid_size(geometry);
        if (choose_floating_size) {
            if (shown != presented_.end()) {
                geometry = shown->second;
            } else {
                // A fullscreen shortcut or tiling toggle can arrive before
                // the first tile placement has been presented.
                const auto content = scene_.window_content_geometry(id);
                geometry.x = motion_delta(static_cast<double>(geometry.x) - content.width / 2);
                geometry.y = motion_delta(static_cast<double>(geometry.y) - content.height / 2);
                geometry.width = content.width;
                geometry.height = content.height;
            }
        }
        if (resized && view.visible &&
            (id == resize || (view.tile_layout && windows_.state().window(id)->home_workspace ==
                                                      resized->home_workspace)) &&
            (shown == presented_.end() || !same_size(geometry, shown->second))) {
            fitting_.insert(id);
        }
        if (fitting_.contains(id) &&
            (view.maximized || view.fullscreen ||
             same_size(geometry, from_scene(scene_.window_content_geometry(id))))) {
            fitting_.erase(id);
        }
        const bool fitted = view.visible && fitting_.contains(id);
        scene_.place_window(
            id,
            {
                .geometry = to_scene(geometry),
                .output = output ? to_scene(windows_.output_bounds(*output).value_or(WindowRect{}))
                                 : seaside_scene_rect{},
                .clip = to_scene(view.clip),
                .stacking = view.stacking,
                .border_width = view.border_width,
                .visible = view.visible,
                .activated = !layer_focus && focused == id,
                .maximized = view.maximized,
                .fullscreen = view.fullscreen,
                .stretch_fullscreen = fullscreen_aspect_ == FullscreenAspect::stretch,
                .tiled = view.tile_layout,
                .resize = id == resize,
                .resizing = drag_ && drag_->started && drag_->resize && drag_->window == id,
                .fit_resize_buffer = fitted,
                .unconstrained_restore = choose_floating_size,
                .interactive_resize = dragged && (drag_->window == id ||
                                                  (fitted && view.tile_layout &&
                                                   windows_.state().window(id)->home_workspace ==
                                                       dragged->home_workspace)),
            });
        if (view.visible) {
            presented_[id] = geometry;
        } else {
            presented_.erase(id);
            fitting_.erase(id);
        }
    }
    if (input_) {
        seaside_tty_input_set_focus(input_,
                                    layer_focus ? layer_focus : scene_.window_surface(focused));
        seaside_tty_input_refresh_pointer(input_);
    }
    if (projected_) {
        projected_();
    }
}

bool Desktop::stage_opening(WindowId id) {
    const auto view = windows_.view(id);
    if (!view.visible || !view.tile_layout || !valid_size(view.geometry)) {
        return false;
    }
    const auto workspace = windows_.state().window(id)->home_workspace;
    std::map<WindowId, OpeningWindow> pending;
    bool has_previous_tile = false;
    for (const auto candidate : views_) {
        if (windows_.state().window(candidate)->home_workspace != workspace) {
            continue;
        }
        const auto tile = windows_.view(candidate);
        if (!tile.visible || !tile.tile_layout || !valid_size(tile.geometry)) {
            continue;
        }
        if (const auto shown = presented_.find(candidate); shown != presented_.end()) {
            has_previous_tile = true;
            if (same_size(shown->second, tile.geometry)) {
                continue;
            }
            // A fast neighbor might commit before a slower one. Continue
            // painting it into the old tile until the new layout is ready.
            scene_.set_fit_resize_buffer(candidate, true);
        }
        fitting_.insert(candidate);
        if (!same_size(tile.geometry, from_scene(scene_.window_content_geometry(candidate)))) {
            pending.emplace(candidate,
                            OpeningWindow{tile.geometry, scene_.configure_tiled_window(
                                                             candidate, to_scene(tile.geometry))});
        }
    }
    if (pending.empty() || !has_previous_tile) {
        return false;
    }
    if (!opening_deadline_) {
        opening_deadline_ = wl_event_loop_add_timer(scene_.event_loop(), on_opening_deadline, this);
        if (!opening_deadline_) {
            return false;
        }
    }
    // One stalled client must not delay a series of new windows indefinitely.
    if (opening_.empty() &&
        wl_event_source_timer_update(opening_deadline_, opening_timeout_ms) < 0) {
        return false;
    }
    opening_ = std::move(pending);
    return true;
}

bool Desktop::opening_ready() const {
    return std::ranges::all_of(opening_, [&](const auto &entry) {
        const auto &[id, target] = entry;
        return includes_serial(scene_.window_commit_serial(id), target.serial) &&
               same_size(from_scene(scene_.window_content_geometry(id)), target.geometry);
    });
}

void Desktop::project_if_opening_ready() {
    if (!opening_.empty() && opening_ready()) {
        project();
    }
}

int Desktop::on_opening_deadline(void *self) noexcept {
    auto &desktop = *static_cast<Desktop *>(self);
    try {
        if (!desktop.opening_.empty()) {
            desktop.project();
        }
    } catch (...) {
        desktop.failed_ = true;
    }
    return 0;
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
        if (projected_) {
            projected_();
        }
        return *id;
    }
    case SEASIDE_WINDOW_INITIAL_COMMIT:
        update_size_limits(event.id);
        windows_.initialize_window(event.id);
        if (const auto geometry = windows_.initial_tile_geometry(event.id)) {
            scene_.configure_tiled_window(event.id, to_scene(*geometry));
        } else if (opening_.empty()) {
            project();
        }
        return 0;
    case SEASIDE_WINDOW_MAPPED:
        update_size_limits(event.id);
        windows_.map_window(event.id, from_scene(event.geometry), event.ordinary_commit);
        if (stage_opening(event.id)) {
            return 0;
        }
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
        presented_.erase(event.id);
        fitting_.erase(event.id);
        windows_.destroy_window(event.id);
        break;
    case SEASIDE_WINDOW_COMMITTED: {
        const bool limits_changed = update_size_limits(event.id);
        const auto current = windows_.view(event.id);
        if (!opening_.empty() && !limits_changed) {
            project_if_opening_ready();
            if (current.tile_layout) {
                return 0;
            }
        }
        if (const auto shown = presented_.find(event.id);
            fitting_.contains(event.id) && shown != presented_.end() &&
            same_size(shown->second, from_scene(event.geometry))) {
            fitting_.erase(event.id);
            scene_.set_fit_resize_buffer(event.id, false);
        }
        if (!event.ordinary_commit || current.tile_layout || current.maximized ||
            current.fullscreen || (drag_ && drag_->window == event.id && drag_->resize_edges) ||
            (current.tiled && drag_ && drag_->started && !drag_->resize &&
             drag_->window == event.id) ||
            (current.geometry.width == event.geometry.width &&
             current.geometry.height == event.geometry.height)) {
            if (limits_changed) {
                project();
            } else if (input_ && (current.fullscreen || fitting_.contains(event.id))) {
                seaside_tty_input_refresh_pointer(input_);
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
    case SEASIDE_WINDOW_ATTENTION_REQUESTED:
        if (const auto *window = windows_.state().window(event.id);
            attention_ && window && window->mapped && windows_.focused_window() != event.id) {
            attention_(event.id, window->home_workspace);
        }
        return 0;
    case SEASIDE_WINDOW_FULLSCREEN_REQUESTED:
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        windows_.set_fullscreen(event.id, event.enabled);
        break;
    case SEASIDE_WINDOW_MOVE_REQUESTED:
    case SEASIDE_WINDOW_RESIZE_REQUESTED:
        if (!begin_client_drag(event)) {
            return 0;
        }
        break;
    }
    project(event.kind == SEASIDE_WINDOW_RESIZE_REQUESTED ? event.id : 0);
    return 0;
}

bool Desktop::begin_client_drag(const seaside_scene_window_event &event) {
    if (drag_ || !pointer_ || windows_.focused_window() != event.id || layer_has_focus_) {
        return false;
    }
    const auto view = windows_.view(event.id);
    const bool resize = event.kind == SEASIDE_WINDOW_RESIZE_REQUESTED;
    if (!view.visible || view.tiled || view.maximized || view.fullscreen ||
        (resize && !event.resize_edges)) {
        return false;
    }
    const auto button = seaside_tty_input_begin_client_gesture(
        input_, event.seat_client, scene_.window_surface(event.id), event.serial);
    if (!button) {
        return false;
    }
    drag_ = Drag{.window = event.id,
                 .button = button,
                 .resize = resize,
                 .x = pointer_->x,
                 .y = pointer_->y,
                 .started = resize,
                 .client = true,
                 .resize_edges = event.resize_edges,
                 .initial_geometry = view.geometry};
    return true;
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

bool Desktop::action(const Action &action, std::uint8_t modifiers) noexcept {
    if (action.kind == ActionKind::night_light || action.kind == ActionKind::exit ||
        action.kind == ActionKind::exec) {
        return false;
    }
    (void)dispatch(action, modifiers);
    return true;
}

std::expected<bool, DesktopActionError>
Desktop::dispatch(const Action &action, std::uint8_t modifiers, std::optional<WindowId> target,
                  std::optional<WorkspaceId> workspace) noexcept try {
    using Error = DesktopActionError;
    const auto id = target.value_or(windows_.focused_window().value_or(0));
    const auto before = windows_.view(id);
    const auto focused = windows_.focused_window();
    const auto current_workspace = windows_.focused_workspace();
    const auto *entry = windows_.state().window(id);
    auto require_window = [&]() -> std::expected<void, Error> {
        if (!entry) {
            return std::unexpected(target ? Error::not_found : Error::no_target);
        }
        if (!entry->mapped) {
            return std::unexpected(Error::invalid_state);
        }
        return {};
    };
    WindowId resized = 0;
    bool changed = false;
    if (action.kind != ActionKind::cycle_focus_mru) {
        finish_focus_cycle();
    }
    switch (action.kind) {
    case ActionKind::focus:
        if (!windows_.focus_direction(action.direction)) {
            return std::unexpected(Error::no_target);
        }
        changed = focused != windows_.focused_window();
        break;
    case ActionKind::cycle_focus_mru:
        cycle_modifiers_ = modifiers;
        if (!windows_.cycle_focus(modifiers != 0)) {
            return std::unexpected(Error::no_target);
        }
        changed = focused != windows_.focused_window();
        break;
    case ActionKind::workspace: {
        const auto destination = static_cast<WorkspaceId>(action.argument);
        if (!current_workspace) {
            return std::unexpected(Error::no_target);
        }
        if (current_workspace != destination) {
            discard_drag();
            changed = windows_.select_workspace(destination);
        }
        break;
    }
    case ActionKind::transfer_workspace: {
        const auto selected = workspace ? workspace : current_workspace;
        if (!selected || !windows_.state().workspace(*selected)) {
            return std::unexpected(workspace ? Error::not_found : Error::no_target);
        }
        const auto selector = parse_output_selector(action.text);
        if (!selector) {
            return std::unexpected(Error::not_found);
        }
        std::optional<std::string_view> destination;
        for (const auto &[handle, output] : outputs_) {
            const auto &value =
                selector->kind == SelectorKind::connector ? output.connector : output.identity;
            if (selector->value == value) {
                if (destination) {
                    return std::unexpected(Error::ambiguous_output);
                }
                destination = output.id;
            }
        }
        if (!destination) {
            return std::unexpected(Error::not_found);
        }
        if (windows_.workspace_output(*selected) != destination) {
            discard_drag();
            changed = windows_.transfer_workspace(*destination, selected);
        }
        break;
    }
    case ActionKind::night_light:
    case ActionKind::exit:
    case ActionKind::exec:
        return std::unexpected(Error::invalid_state);
    default:
        if (const auto valid = require_window(); !valid) {
            return std::unexpected(valid.error());
        }
        if (action.kind == ActionKind::close_window) {
            if (!scene_.close_window(id)) {
                return std::unexpected(Error::invalid_state);
            }
            return false;
        }
        discard_drag();
        switch (action.kind) {
        case ActionKind::move_to_workspace: {
            const auto destination = static_cast<WorkspaceId>(action.argument);
            const bool would_change = entry->home_workspace != destination || id != focused ||
                                      current_workspace != destination;
            if (!windows_.move_to_workspace(id, destination)) {
                return std::unexpected(Error::invalid_state);
            }
            changed = would_change;
            break;
        }
        case ActionKind::toggle_window_pin:
            changed = windows_.toggle_pin(id);
            break;
        case ActionKind::toggle_window_tiling:
            changed = windows_.toggle_tiling(id);
            break;
        case ActionKind::toggle_window_maximize:
            changed = windows_.set_maximized(id, !before.maximized);
            break;
        case ActionKind::toggle_window_fullscreen:
            changed = windows_.set_fullscreen(id, !before.fullscreen);
            break;
        case ActionKind::move_window:
            if (!windows_.move_window(id, action.direction)) {
                return std::unexpected(Error::invalid_state);
            }
            changed = before.tile_layout || windows_.view(id).geometry != before.geometry;
            break;
        case ActionKind::resize_window:
            if (!resize(id, action.direction, static_cast<int>(action.argument))) {
                return std::unexpected(Error::invalid_state);
            }
            changed = before.tile_layout || windows_.view(id).geometry != before.geometry;
            if (changed) {
                resized = id;
            }
            break;
        case ActionKind::swap_window:
            changed = windows_.swap_window(id, action.direction);
            break;
        default:
            break;
        }
        if (!changed && action.kind != ActionKind::move_to_workspace &&
            action.kind != ActionKind::move_window && action.kind != ActionKind::resize_window) {
            project();
            return std::unexpected(Error::invalid_state);
        }
        break;
    }
    if (focused != windows_.focused_window() || current_workspace != windows_.focused_workspace()) {
        scene_.select_layer(nullptr);
    }
    project(resized);
    return changed;
} catch (...) {
    failed_ = true;
    return std::unexpected(DesktopActionError::invalid_state);
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
    if (std::isfinite(x) && std::isfinite(y)) {
        pointer_ = PointerPosition{x, y};
    }
    if (!pressed) {
        if (drag_ && drag_->button == button) {
            if (const auto last = std::exchange(drag_->pending, std::nullopt)) {
                apply_drag_motion(last->x, last->y);
            }
        }
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
        scene_.select_layer(nullptr);
        windows_.click(id);
    } else {
        seaside_scene_target target{};
        if (scene_.window_at(x, y, target)) {
            scene_.select_layer(target.surface);
        } else {
            scene_.select_layer(nullptr);
        }
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
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    if (pointer_) {
        pointer_ = PointerPosition{x, y};
    }
    if (!drag_) {
        return;
    }
    if (frame_paced_resize_ && drag_->resize) {
        if (auto *output = window_output_handle(drag_->window)) {
            drag_->pending = PointerPosition{x, y};
            scene_.schedule_frame(output);
            return;
        }
    }
    apply_drag_motion(x, y);
} catch (...) {
    failed_ = true;
}

void Desktop::before_output_frame(void *self, wlr_output *output) noexcept {
    auto &desktop = *static_cast<Desktop *>(self);
    try {
        if (desktop.drag_ && desktop.drag_->pending &&
            desktop.window_output_handle(desktop.drag_->window) == output) {
            const auto latest = *std::exchange(desktop.drag_->pending, std::nullopt);
            desktop.apply_drag_motion(latest.x, latest.y);
        }
    } catch (...) {
        desktop.failed_ = true;
    }
}

void Desktop::apply_drag_motion(double x, double y) {
    const auto dx = motion_delta(x - drag_->x);
    const auto dy = motion_delta(y - drag_->y);
    const WindowId id = drag_->window;
    if (const auto edges = drag_->resize_edges) {
        if (windows_.resize_floating(
                id, drag_->initial_geometry,
                edges & (SEASIDE_RESIZE_LEFT | SEASIDE_RESIZE_RIGHT) ? std::optional{dx}
                                                                     : std::nullopt,
                edges & (SEASIDE_RESIZE_TOP | SEASIDE_RESIZE_BOTTOM) ? std::optional{dy}
                                                                     : std::nullopt,
                (edges & SEASIDE_RESIZE_LEFT) != 0, (edges & SEASIDE_RESIZE_TOP) != 0)) {
            project(id);
        }
        return;
    }
    if (!dx && !dy) {
        return;
    }
    drag_->x += dx;
    drag_->y += dy;
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
}

} // namespace seaside

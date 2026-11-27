#include "desktop.hpp"
#include "tty_input.hpp"

#include <algorithm>
#include <array>
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

bool same_size(WindowRect first, WindowRect second) noexcept {
    return first.width == second.width && first.height == second.height;
}

bool valid_size(WindowRect rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

bool includes_serial(std::uint32_t committed, std::uint32_t requested) noexcept {
    return requested == 0 || (committed && committed - requested < 0x80000000U);
}

constexpr std::array resize_edges{std::pair{Direction::left, SEASIDE_RESIZE_LEFT},
                                  std::pair{Direction::right, SEASIDE_RESIZE_RIGHT},
                                  std::pair{Direction::up, SEASIDE_RESIZE_TOP},
                                  std::pair{Direction::down, SEASIDE_RESIZE_BOTTOM}};

std::uint32_t resize_edges_at(WindowRect box, double x, double y) noexcept {
    const auto near_edge = [](double fraction, std::uint32_t near, std::uint32_t far) {
        return fraction < 0.25 ? near : fraction > 0.75 ? far : 0U;
    };
    return near_edge((x - box.x) / box.width, SEASIDE_RESIZE_LEFT, SEASIDE_RESIZE_RIGHT) |
           near_edge((y - box.y) / box.height, SEASIDE_RESIZE_TOP, SEASIDE_RESIZE_BOTTOM);
}

std::uint32_t resize_edges_toward(int dx, int dy, std::uint32_t available) noexcept {
    const auto pick = [available](int delta, std::uint32_t near, std::uint32_t far) {
        const auto preferred = delta < 0 ? near : far;
        return available & preferred ? preferred : available & (near | far);
    };
    const auto horizontal = std::abs(std::int64_t{dx});
    const auto vertical = std::abs(std::int64_t{dy});
    std::uint32_t edges = 0;
    if (dx && horizontal * 2 >= vertical) {
        edges |= pick(dx, SEASIDE_RESIZE_LEFT, SEASIDE_RESIZE_RIGHT);
    }
    if (dy && vertical * 2 >= horizontal) {
        edges |= pick(dy, SEASIDE_RESIZE_TOP, SEASIDE_RESIZE_BOTTOM);
    }
    return edges;
}
} // namespace

Desktop::Desktop(Scene &scene, const Config &config)
    : scene_(scene), windows_(config), bindings_(config.keybinds),
      fullscreen_aspect_(config.render.fullscreen_aspect) {
    scene_.set_titlebars(config.window.titlebars);
    scene_.set_border_colors(config.window.active_border_color,
                             config.window.inactive_border_color);
    scene_.set_dark_appearance(config.appearance.color_scheme == ColorScheme::dark);
    scene_.set_window_handler(on_window, this);
    scene_.set_layer_handler(
        [](void *self) { return static_cast<Desktop *>(self)->focused_output_handle(); },
        on_layer_changed,
        [](void *self, wlr_surface *root) {
            if (auto *input = static_cast<Desktop *>(self)->input_) {
                input->cancel_drag_from(root);
            }
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

void Desktop::set_input(TtyInput *input) noexcept {
    if (input_ && input_ != input) {
        // Release the client-owned popup roles while their seat still exists.
        scene_.dismiss_unfocused_grabs(nullptr);
    }
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

bool Desktop::add_output(wlr_output *output, std::string_view id, WindowRect bounds) noexcept try {
    auto [record, inserted] = outputs_.try_emplace(output, Output{std::string(id), bounds});
    if (!inserted) {
        return false;
    }
    if (!scene_.add_output(output) || !scene_.place_output(output, bounds.x, bounds.y)) {
        scene_.remove_output(output);
        outputs_.erase(record);
        return false;
    }
    discard_drag();
    if (!windows_.add_output(record->second.id, bounds, bounds)) {
        scene_.remove_output(output);
        outputs_.erase(record);
        cancel_drag();
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
        discard_drag();
        scene_.remove_output(output);
        if (!windows_.remove_output(record->second.id)) {
            failed_ = true;
        }
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

void Desktop::set_primary_output(std::string_view id) noexcept try {
    if (windows_.primary_output() == id) {
        return;
    }
    discard_drag();
    if (!windows_.set_primary_output(id)) {
        failed_ = true;
        return;
    }
    scene_.select_layer(nullptr);
    project();
} catch (...) {
    failed_ = true;
}

void Desktop::apply_config(const Config &config) noexcept try {
    auto bindings = config.keybinds;
    if (config.window.allow_edge_drag != windows_.allow_edge_drag()) {
        discard_drag();
    }
    if (bindings != bindings_) {
        discard_drag();
        finish_focus_cycle();
        bindings_.swap(bindings);
    }
    fullscreen_aspect_ = config.render.fullscreen_aspect;
    scene_.set_titlebars(config.window.titlebars);
    scene_.set_border_colors(config.window.active_border_color,
                             config.window.inactive_border_color);
    scene_.set_dark_appearance(config.appearance.color_scheme == ColorScheme::dark);
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

bool Desktop::fullscreen_inhibits_idle(wlr_output *output) const noexcept {
    const auto entry = outputs_.find(output);
    if (entry == outputs_.end()) {
        return false;
    }
    const auto &id = entry->second.id;
    // A pending tile can take policy focus before the scene and keyboard
    // switch away from the displayed fullscreen window.
    if (!opening_.empty() && keyboard_window_ && windows_.window_output(*keyboard_window_) == id) {
        const auto focused = windows_.view(*keyboard_window_);
        if (focused.visible && focused.fullscreen) {
            return true;
        }
    }
    return windows_.has_fullscreen(id);
}

void Desktop::on_layer_changed(void *self) noexcept {
    auto &desktop = *static_cast<Desktop *>(self);
    try {
        desktop.project();
    } catch (...) {
        desktop.failed_ = true;
    }
}

void Desktop::project(WindowId resize, bool geometry_only) {
    if (!opening_.empty()) {
        geometry_only = false;
        opening_.clear();
        wl_event_source_timer_update(opening_deadline_, 0);
    }
    if (drag_) {
        const auto view = windows_.view(drag_->window);
        if (!view.visible || view.maximized || view.fullscreen || (drag_->client && view.tiled) ||
            (drag_->resize && drag_->tiled != view.tile_layout)) {
            geometry_only = false;
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
            geometry_only = false;
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
    // Resizing an ordinary floating window can also move dialogs anchored
    // to it (including nested dialogs). Tiled resizes redistribute a layout.
    const bool floating_resize = geometry_only && resized && !windows_.view(resize).tiled;
    for (const auto &[id, state] : windows_.state().windows()) {
        if (floating_resize && id != resize && !windows_.is_descendant(id, resize)) {
            continue;
        }
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
            (id == resize ||
             (view.tile_layout && state.home_workspace == resized->home_workspace)) &&
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
                .interactive_resize =
                    dragged &&
                    (drag_->window == id || (fitted && view.tile_layout &&
                                             state.home_workspace == dragged->home_workspace)),
                .unclipped_move = view.unclipped_move,
            });
        if (view.visible) {
            presented_[id] = geometry;
        } else {
            presented_.erase(id);
            fitting_.erase(id);
        }
    }
    update_keyboard_focus(layer_focus, focused);
    if (input_) {
        input_->refresh_pointer();
    }
    if (projected_) {
        projected_();
    }
}

void Desktop::update_keyboard_focus(wlr_surface *layer_focus, WindowId focused) noexcept {
    auto *surface = layer_focus ? layer_focus : scene_.window_surface(focused);
    keyboard_window_ = surface && !layer_focus ? std::optional{focused} : std::nullopt;
    if (input_) {
        scene_.dismiss_unfocused_grabs(surface);
        input_->set_focus(surface);
    }
}

bool Desktop::stage_opening(WorkspaceId workspace) {
    std::map<WindowId, OpeningWindow> pending;
    bool has_previous_tile = false;
    for (const auto &[candidate, state] : windows_.state().windows()) {
        if (state.home_workspace != workspace) {
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
    if (!has_previous_tile) {
        pending.clear();
    }
    // A full projection publishes every output together. Keep the exact
    // configure serials from other workspaces under the original deadline.
    for (const auto &[other, target] : opening_) {
        const auto *window = windows_.state().window(other);
        if (window && window->mapped && window->home_workspace != workspace) {
            pending.emplace(other, target);
        }
    }
    if (pending.empty()) {
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
    return !opening_ready();
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
    case SEASIDE_WINDOW_MAPPED: {
        update_size_limits(event.id);
        windows_.map_window(event.id, from_scene(event.geometry), event.ordinary_commit);
        const auto view = windows_.view(event.id);
        if (view.visible && view.tile_layout && valid_size(view.geometry) &&
            stage_opening(windows_.state().window(event.id)->home_workspace)) {
            return 0;
        }
        break;
    }
    case SEASIDE_WINDOW_UNMAPPED:
    case SEASIDE_WINDOW_DESTROYED: {
        const auto *window = windows_.state().window(event.id);
        if (!window) {
            return 0;
        }
        const auto workspace = window->home_workspace;
        if (drag_ && drag_->window == event.id) {
            discard_drag();
        }
        presented_.erase(event.id);
        fitting_.erase(event.id);
        if (event.kind == SEASIDE_WINDOW_DESTROYED) {
            windows_.destroy_window(event.id);
        } else {
            windows_.unmap_window(event.id);
        }
        if (!opening_.empty() && stage_opening(workspace)) {
            if (keyboard_window_ == event.id) {
                const auto selected = windows_.focused_window().value_or(0);
                update_keyboard_focus(scene_.layer_keyboard_focus(focused_output_handle()),
                                      presented_.contains(selected) ? selected : 0);
            }
            if (input_) {
                input_->refresh_pointer();
            }
            if (projected_) {
                projected_();
            }
            return 0;
        }
        break;
    }
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
            current.fullscreen ||
            (drag_ && drag_->window == event.id && drag_->resize && drag_->started) ||
            (current.tiled && drag_ && drag_->started && !drag_->resize &&
             drag_->window == event.id) ||
            (current.geometry.width == event.geometry.width &&
             current.geometry.height == event.geometry.height)) {
            if (limits_changed) {
                project();
            } else if (input_ && (current.fullscreen || fitting_.contains(event.id))) {
                input_->refresh_pointer();
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
            attention_ && window && window->mapped && keyboard_window_ != event.id) {
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
    if (!input_ || drag_ || !pointer_ || keyboard_window_ != event.id) {
        return false;
    }
    const auto view = windows_.view(event.id);
    const bool resize = event.kind == SEASIDE_WINDOW_RESIZE_REQUESTED;
    if (!view.visible || view.tiled || view.maximized || view.fullscreen ||
        (resize && !event.resize_edges)) {
        return false;
    }
    const auto button = input_->begin_client_gesture(event.seat_client,
                                                     scene_.window_surface(event.id), event.serial);
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
                 .resize_edges_locked = resize,
                 .resize_edges = event.resize_edges,
                 .initial_geometry = view.geometry};
    return true;
}

ResizeResult Desktop::resize(WindowId id, Direction direction, int distance) {
    if (distance <= 0) {
        return ResizeResult::invalid;
    }
    if (windows_.view(id).tile_layout) {
        return windows_.resize_window(id, direction, distance) ? ResizeResult::changed
                                                               : ResizeResult::invalid;
    }
    switch (direction) {
    case Direction::left:
        return windows_.resize_window_by(id, -distance, 0);
    case Direction::right:
        return windows_.resize_window_by(id, distance, 0);
    case Direction::up:
        return windows_.resize_window_by(id, 0, -distance);
    case Direction::down:
        return windows_.resize_window_by(id, 0, distance);
    case Direction::none:
        return ResizeResult::invalid;
    }
    return ResizeResult::invalid;
}

bool Desktop::action(const Action &action, std::uint8_t modifiers) noexcept {
    if (action.kind == ActionKind::night_light || action.kind == ActionKind::exit ||
        action.kind == ActionKind::exec) {
        return false;
    }
    (void)dispatch(action, modifiers);
    return true;
}

std::expected<bool, DesktopActionError> Desktop::dispatch(const Action &action,
                                                          std::uint8_t modifiers,
                                                          std::optional<WindowId> target) noexcept
    try {
    using Error = DesktopActionError;
    const auto focused = windows_.focused_window();
    const auto focused_output = windows_.focused_output();
    const auto id = target.value_or(keyboard_window_.value_or(0));
    const auto before = windows_.view(id);
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
    const bool had_drag = drag_.has_value();
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
        discard_drag();
        changed = true;
        break;
    case ActionKind::workspace: {
        if (action.argument <= 0 || action.argument > workspace_count) {
            return std::unexpected(Error::invalid_state);
        }
        const auto destination = static_cast<WorkspaceId>(action.argument);
        if (!current_workspace) {
            return std::unexpected(Error::no_target);
        }
        if (current_workspace == destination) {
            return false;
        }
        discard_drag();
        changed = windows_.select_workspace(destination);
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
            if (action.argument <= 0 || action.argument > workspace_count) {
                return std::unexpected(Error::invalid_state);
            }
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
        case ActionKind::resize_window: {
            const auto result = resize(id, action.direction, static_cast<int>(action.argument));
            if (result == ResizeResult::invalid) {
                if (had_drag) {
                    project();
                }
                return std::unexpected(Error::invalid_state);
            }
            changed = result == ResizeResult::changed;
            if (!changed && !had_drag) {
                return false;
            }
            resized = changed ? id : 0;
            break;
        }
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
    const bool focus_changed = focused != windows_.focused_window() ||
                               focused_output != windows_.focused_output() ||
                               current_workspace != windows_.focused_workspace();
    if (focus_changed) {
        scene_.select_layer(nullptr);
    }
    project(resized, !had_drag && !focus_changed);
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
    if (id && (button == BTN_LEFT || button == BTN_RIGHT) && std::isfinite(x) && std::isfinite(y)) {
        const auto chord =
            button == BTN_LEFT ? ChordKind::mouse_left_drag : ChordKind::mouse_right_drag;
        const auto binding = bindings_.find({modifiers, chord, 0});
        const auto view = windows_.view(id);
        if (binding != bindings_.end() && view.visible && !view.maximized && !view.fullscreen) {
            const bool resize = binding->second.kind == ActionKind::resize_window;
            if (!resize || valid_size(view.geometry)) {
                std::uint32_t available = 0;
                for (const auto &[direction, edge] : resize_edges) {
                    if (!view.tile_layout || windows_.has_tile_resize_edge(id, direction)) {
                        available |= edge;
                    }
                }
                const auto edges = resize ? resize_edges_at(view.geometry, x, y) : 0U;
                drag_ = Drag{.window = id,
                             .button = button,
                             .resize = resize,
                             .x = x,
                             .y = y,
                             .tiled = view.tile_layout,
                             .resize_edges_locked = edges != 0,
                             .available_resize_edges = available,
                             .resize_edges = edges & available,
                             .initial_geometry = view.geometry};
            }
        }
    }
    // Focusing by click or arming a drag must not publish an unfinished tiled opening.
    if (opening_.empty()) {
        project();
    } else {
        auto *layer_focus = scene_.layer_keyboard_focus(focused_output_handle());
        const auto focused = windows_.focused_window().value_or(0);
        // Deliver the next key to the clicked scene without exposing an
        // unpresented toplevel selected by the pending layout.
        if (layer_focus || !focused || presented_.contains(focused)) {
            update_keyboard_focus(layer_focus, focused);
            if (projected_) {
                projected_();
            }
        }
    }
    return drag_.has_value();
} catch (...) {
    failed_ = true;
    return false;
}

std::pair<int, int> Desktop::Drag::delta(double target_x, double target_y) const noexcept {
    const bool horizontal =
        !resize_edges || (resize_edges & (SEASIDE_RESIZE_LEFT | SEASIDE_RESIZE_RIGHT));
    const bool vertical =
        !resize_edges || (resize_edges & (SEASIDE_RESIZE_TOP | SEASIDE_RESIZE_BOTTOM));
    return {horizontal ? motion_delta(target_x - x) : 0, vertical ? motion_delta(target_y - y) : 0};
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
    if (drag_->resize) {
        if (!drag_->resize_edges_locked) {
            const auto [dx, dy] = drag_->delta(x, y);
            drag_->resize_edges = resize_edges_toward(dx, dy, drag_->available_resize_edges);
            drag_->resize_edges_locked = drag_->resize_edges != 0;
        }
        if (!drag_->resize_edges) {
            return;
        }
    }
    if (frame_paced_resize_ && drag_->resize) {
        if (auto *output = window_output_handle(drag_->window)) {
            if (drag_->pending) {
                // The pointer can return to the applied target before this frame.
                drag_->pending = PointerPosition{x, y};
            } else if (drag_->delta(x, y) != drag_->applied_edge_delta) {
                drag_->pending = PointerPosition{x, y};
                scene_.schedule_frame(output);
            }
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
    const auto [dx, dy] = drag_->delta(x, y);
    const WindowId id = drag_->window;
    if (drag_->resize) {
        const auto edges = drag_->resize_edges;
        if (!edges) {
            return;
        }
        const auto horizontal = edges & SEASIDE_RESIZE_LEFT    ? Direction::left
                                : edges & SEASIDE_RESIZE_RIGHT ? Direction::right
                                                               : Direction::none;
        const auto vertical = edges & SEASIDE_RESIZE_TOP      ? Direction::up
                              : edges & SEASIDE_RESIZE_BOTTOM ? Direction::down
                                                              : Direction::none;
        drag_->applied_edge_delta = {dx, dy};
        const bool changed =
            drag_->tiled ? windows_.resize_tile_edges(id, drag_->initial_geometry, horizontal, dx,
                                                      vertical, dy)
                         : windows_.resize_floating(
                               id, drag_->initial_geometry,
                               horizontal != Direction::none ? std::optional{dx} : std::nullopt,
                               vertical != Direction::none ? std::optional{dy} : std::nullopt,
                               horizontal == Direction::left, vertical == Direction::up);
        if (changed) {
            drag_->started = true;
            project(id, true);
        }
        return;
    }
    if (!dx && !dy) {
        return;
    }
    drag_->x += dx;
    drag_->y += dy;
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

} // namespace seaside

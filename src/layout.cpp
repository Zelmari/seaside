#include "layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace seaside {
namespace {

constexpr int max_extent = std::numeric_limits<int>::max();
constexpr std::int64_t max_strip = std::numeric_limits<std::int64_t>::max();

int coordinate(std::int64_t value) noexcept {
    return static_cast<int>(
        std::clamp<std::int64_t>(value, std::numeric_limits<int>::min(), max_extent));
}

int shift(int origin, std::int64_t delta) noexcept {
    const auto bounded = std::clamp(delta, std::int64_t{std::numeric_limits<int>::min()} - origin,
                                    std::int64_t{max_extent} - origin);
    return static_cast<int>(origin + bounded);
}

std::int64_t strip_add(std::int64_t position, int extent) noexcept {
    return std::min(max_strip - extent, position) + extent;
}

bool horizontal(Direction direction) noexcept {
    return direction == Direction::left || direction == Direction::right;
}

bool before(Direction direction) noexcept {
    return direction == Direction::left || direction == Direction::up;
}

int inset_amount(int size, int inset) noexcept {
    return std::min(inset, std::max(0, (size - 1) / 2));
}

WindowRect inset(WindowRect box, int margin) noexcept {
    const auto x = inset_amount(box.width, margin);
    const auto y = inset_amount(box.height, margin);
    return {shift(box.x, x), shift(box.y, y), box.width - 2 * x, box.height - 2 * y};
}

bool contains_point(WindowRect box, double x, double y) noexcept {
    return x >= box.x && x < static_cast<double>(box.x) + box.width && y >= box.y &&
           y < static_cast<double>(box.y) + box.height;
}

int outer_minimum(int minimum, int border) noexcept {
    return coordinate(std::int64_t{std::max(1, minimum)} + 2 * border);
}

int outer_maximum(int maximum, int border) noexcept {
    return maximum > 0 ? coordinate(std::int64_t{maximum} + 2 * border) : max_extent;
}

int usable_gap(int extent, int gap) noexcept {
    return std::min(gap, std::max(0, extent - 2));
}

int first_extent(int available, int requested, int first_min, int second_min) noexcept {
    const int minimum = std::min(1, available);
    if (std::int64_t{first_min} + second_min <= available) {
        return std::clamp(requested, first_min, available - second_min);
    }
    // If both minima cannot fit, maintain the desired ratio and leave each
    // child a positive extent whenever the span can represent both tiles.
    return std::clamp(requested, minimum, std::max(minimum, available - minimum));
}

double checked_fraction(double value, double low, double high) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : 0.5;
}

LayoutSettings checked_settings(LayoutSettings settings) noexcept {
    settings.split_ratio = checked_fraction(settings.split_ratio, 0.05, 0.95);
    settings.default_column_width = checked_fraction(settings.default_column_width, 0.10, 4.0);
    settings.outer_gap = std::clamp(settings.outer_gap, 0, 512);
    settings.inner_gap = std::clamp(settings.inner_gap, 0, 512);
    settings.border_width = std::clamp(settings.border_width, 0, 64);
    return settings;
}

} // namespace

WorkspaceLayout::WorkspaceLayout(LayoutSettings settings) : settings_(checked_settings(settings)) {}

void WorkspaceLayout::set_settings(LayoutSettings settings) {
    settings = checked_settings(settings);
    if (settings_ == settings) {
        return;
    }
    if (settings.kind != settings_.kind) {
        tiles_.clear();
        nodes_.clear();
        columns_.clear();
        root_ = 0;
        free_node_ = 0;
        offset_ = 0;
        strip_width_ = 0;
    }
    settings_ = settings;
    reflow();
}

void WorkspaceLayout::set_area(WindowRect area) {
    if (area_ != area) {
        area.width = std::max(0, area.width);
        area.height = std::max(0, area.height);
        area_ = area;
        reflow();
    }
}

WindowRect WorkspaceLayout::tiling_area() const noexcept {
    return inset(area_, settings_.outer_gap);
}

bool WorkspaceLayout::contains(WindowId id) const noexcept {
    return tiles_.contains(id);
}

bool WorkspaceLayout::add(TiledWindow window, std::optional<WindowId> focused,
                          std::optional<TileDrop> drop) {
    if (!window.id || contains(window.id) ||
        (drop && (drop->side == Direction::none || !contains(drop->target)))) {
        return false;
    }
    if (settings_.kind == Layout::scrolling && !window.column_fraction) {
        window.column_fraction = settings_.default_column_width;
    }
    const auto [tile, inserted] = tiles_.try_emplace(window.id, Tile{.window = window});
    if (!inserted) {
        return false;
    }
    bool added;
    try {
        added = settings_.kind == Layout::dwindle ? insert_split(window, focused, drop)
                                                  : insert_column(window, focused, drop);
    } catch (...) {
        tiles_.erase(tile);
        throw;
    }
    if (!added) {
        tiles_.erase(tile);
        return false;
    }
    reflow();
    return true;
}

WorkspaceLayout::NodeId WorkspaceLayout::allocate_node() {
    if (free_node_) {
        const auto id = free_node_;
        free_node_ = nodes_[id].parent;
        nodes_[id] = {};
        return id;
    }
    if (nodes_.empty()) {
        nodes_.emplace_back();
    }
    nodes_.emplace_back();
    return nodes_.size() - 1;
}

void WorkspaceLayout::release_node(NodeId id) noexcept {
    nodes_[id] = {.parent = free_node_};
    free_node_ = id;
}

bool WorkspaceLayout::insert_split(TiledWindow window, std::optional<WindowId> focused,
                                   std::optional<TileDrop> drop) {
    if (root_ == 0) {
        root_ = allocate_node();
        nodes_[root_].window = window.id;
        tiles_.at(window.id).position = root_;
        return true;
    }

    NodeId target = 0;
    if (drop) {
        target = tiles_.at(drop->target).position;
    } else if (settings_.insertion == Insertion::focused && focused && contains(*focused)) {
        target = tiles_.at(*focused).position;
    } else {
        std::optional<std::tuple<std::int64_t, std::uint64_t, WindowId>> best;
        for (const auto &[id, tile] : tiles_) {
            if (id == window.id) {
                continue;
            }
            const auto box = nodes_[tile.position].box;
            const auto rank =
                std::tuple{-std::int64_t{box.width} * box.height, tile.window.opening_order, id};
            if (!best || rank < *best) {
                best = rank;
                target = tile.position;
            }
        }
    }
    if (!target) {
        return false;
    }

    const auto old_leaf = allocate_node();
    NodeId new_leaf;
    try {
        new_leaf = allocate_node();
    } catch (...) {
        release_node(old_leaf);
        throw;
    }
    nodes_[old_leaf] = nodes_[target];
    nodes_[old_leaf].parent = target;
    nodes_[new_leaf] = {.parent = target, .window = window.id};
    tiles_.at(nodes_[old_leaf].window).position = old_leaf;
    tiles_.at(window.id).position = new_leaf;
    const bool new_first = drop && before(drop->side);
    auto &split = nodes_[target];
    split.first = new_first ? new_leaf : old_leaf;
    split.second = new_first ? old_leaf : new_leaf;
    split.window = 0;
    split.ratio = settings_.split_ratio;
    if (drop) {
        split.drop_horizontal = horizontal(drop->side);
    }
    return true;
}

bool WorkspaceLayout::insert_column(TiledWindow window, std::optional<WindowId> focused,
                                    std::optional<TileDrop> drop) {
    if (drop && !horizontal(drop->side)) {
        return false;
    }
    auto position = columns_.end();
    const auto anchor = drop ? std::optional{drop->target} : focused;
    if (anchor) {
        const auto found = std::ranges::find(columns_, *anchor);
        if (found != columns_.end()) {
            position = drop && drop->side == Direction::left ? found : std::next(found);
        }
    }
    columns_.insert(position, window.id);
    return true;
}

void WorkspaceLayout::erase_split(WindowId id) noexcept {
    const auto leaf = tiles_.at(id).position;
    const auto parent = nodes_[leaf].parent;
    if (!parent) {
        root_ = 0;
    } else {
        const auto sibling =
            nodes_[parent].first == leaf ? nodes_[parent].second : nodes_[parent].first;
        const auto grandparent = nodes_[parent].parent;
        nodes_[sibling].parent = grandparent;
        if (!grandparent) {
            root_ = sibling;
        } else if (nodes_[grandparent].first == parent) {
            nodes_[grandparent].first = sibling;
        } else {
            nodes_[grandparent].second = sibling;
        }
        release_node(parent);
    }
    release_node(leaf);
}

std::optional<TiledWindow> WorkspaceLayout::erase(WindowId id) {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end()) {
        return {};
    }
    if (settings_.kind == Layout::dwindle) {
        erase_split(id);
    } else {
        std::erase(columns_, id);
    }
    const auto window = tile->second.window;
    tiles_.erase(tile);
    reflow();
    return window;
}

bool WorkspaceLayout::update_limits(WindowId id, TileLimits limits) {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || tile->second.window.limits == limits) {
        return false;
    }
    tile->second.window.limits = limits;
    reflow();
    return true;
}

std::optional<WindowRect> WorkspaceLayout::geometry(WindowId id) const noexcept {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || area_.width == 0 || area_.height == 0) {
        return {};
    }
    const auto &limits = tile->second.window.limits;
    auto box = inset(tile->second.box, border_width(id));
    if (limits.max_width > 0) {
        box.width = std::min(box.width, std::max(limits.min_width, limits.max_width));
    }
    if (limits.max_height > 0) {
        box.height = std::min(box.height, std::max(limits.min_height, limits.max_height));
    }
    return box;
}

int WorkspaceLayout::border_width(WindowId id) const noexcept {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || area_.width == 0 || area_.height == 0) {
        return 0;
    }
    const auto &box = tile->second.box;
    const auto &limits = tile->second.window.limits;
    const int slack_width = box.width - std::max(1, limits.min_width);
    const int slack_height = box.height - std::max(1, limits.min_height);
    return std::clamp(std::min(slack_width, slack_height) / 2, 0, settings_.border_width);
}

// Traverse using parent links so deeply nested focused splits never consume
// the C++ call stack. An absent parent terminates the traversal.
WorkspaceLayout::NodeId WorkspaceLayout::next_subtree(NodeId id) const noexcept {
    while (const auto parent = nodes_[id].parent) {
        if (nodes_[parent].first == id) {
            return nodes_[parent].second;
        }
        id = parent;
    }
    return 0;
}

WorkspaceLayout::NodeId WorkspaceLayout::first_leaf(NodeId id) const noexcept {
    while (nodes_[id].first) {
        id = nodes_[id].first;
    }
    return id;
}

bool WorkspaceLayout::project_tree(bool constrain) {
    bool axis_changed = false;
    nodes_[root_].box = tiling_area();
    for (auto id = root_; id;) {
        auto &node = nodes_[id];
        if (node.window) {
            tiles_.at(node.window).box = node.box;
            id = next_subtree(id);
            continue;
        }
        // A drop honors the selected edge at its original region size.
        // Split orientation follows the longest axis when that region changes.
        const auto box = node.box;
        const bool had_drop_size = node.drop_region.width > 0 && node.drop_region.height > 0;
        const bool original_size = !had_drop_size || (node.drop_region.width == box.width &&
                                                      node.drop_region.height == box.height);
        const bool axis =
            node.drop_horizontal && original_size ? *node.drop_horizontal : box.width >= box.height;
        axis_changed = axis_changed || node.horizontal != axis;
        node.horizontal = axis;
        const int extent = axis ? box.width : box.height;
        auto &first = nodes_[node.first];
        auto &second = nodes_[node.second];
        const auto [first_min, second_min] =
            constrain ? split_minima(node, extent) : std::pair{1, 1};
        // Honor the configured gap in the remaining span. A large gap must
        // not consume space the two subtrees need for feasible client minima.
        const auto slack = std::max<std::int64_t>(0, std::int64_t{extent} - first_min - second_min);
        int gap = usable_gap(extent, std::min(settings_.inner_gap, coordinate(slack)));
        int available = extent - gap;
        const double requested_extent = std::nextafter(available * node.ratio, max_extent);
        const int requested = static_cast<int>(std::floor(requested_extent));
        int split = first_extent(available, requested, first_min, second_min);
        if (constrain && (first.first || second.first)) {
            if (const auto fit = fit_children(node, available, split)) {
                split = *fit;
            } else if (gap) {
                // Check the full span once before falling back to the ratio.
                const int requested_without_gap = static_cast<int>(std::floor(extent * node.ratio));
                if (const auto full = fit_children(node, extent, requested_without_gap)) {
                    split = *full;
                    available = extent;
                    gap = 0;
                }
            }
        }
        if (axis) {
            first.box = {box.x, box.y, split, box.height};
            second.box = {shift(box.x, split + gap), box.y, available - split, box.height};
        } else {
            first.box = {box.x, box.y, box.width, split};
            second.box = {box.x, shift(box.y, split + gap), box.width, available - split};
        }
        id = node.first;
    }
    return axis_changed;
}

void WorkspaceLayout::measure_tree() {
    for (auto id = first_leaf(root_); id;) {
        auto &node = nodes_[id];
        if (node.window) {
            const auto limits = tiles_.at(node.window).window.limits;
            node.min_width = outer_minimum(limits.min_width, settings_.border_width);
            node.min_height = outer_minimum(limits.min_height, settings_.border_width);
            node.content_min_width = std::max(1, limits.min_width);
            node.content_min_height = std::max(1, limits.min_height);
        } else {
            const auto &first = nodes_[node.first];
            const auto &second = nodes_[node.second];
            const auto joined = [](int a, int b) { return coordinate(std::int64_t{a} + b); };
            node.min_width = node.horizontal ? joined(first.min_width, second.min_width)
                                             : std::max(first.min_width, second.min_width);
            node.min_height = node.horizontal ? std::max(first.min_height, second.min_height)
                                              : joined(first.min_height, second.min_height);
            node.content_min_width =
                node.horizontal ? joined(first.content_min_width, second.content_min_width)
                                : std::max(first.content_min_width, second.content_min_width);
            node.content_min_height =
                node.horizontal ? std::max(first.content_min_height, second.content_min_height)
                                : joined(first.content_min_height, second.content_min_height);
        }
        const auto parent = node.parent;
        id = parent && nodes_[parent].first == id ? first_leaf(nodes_[parent].second) : parent;
    }
}

std::pair<int, int> WorkspaceLayout::split_minima(const Node &node, int extent) const noexcept {
    const auto &first = nodes_[node.first];
    const auto &second = nodes_[node.second];
    const int first_min = node.horizontal ? first.min_width : first.min_height;
    const int second_min = node.horizontal ? second.min_width : second.min_height;
    if (std::int64_t{first_min} + second_min <= extent) {
        return {first_min, second_min};
    }
    return node.horizontal ? std::pair{first.content_min_width, second.content_min_width}
                           : std::pair{first.content_min_height, second.content_min_height};
}

std::optional<int> WorkspaceLayout::fit_children(const Node &node, int span,
                                                 int requested) const noexcept {
    // A child's minimum along this split can change when its own dynamic axis
    // flips. Check the feasible size range on each side of that threshold.
    using Range = std::pair<std::int64_t, std::int64_t>;
    constexpr Range empty{1, 0};
    const int cross = node.horizontal ? node.box.height : node.box.width;
    const auto ranges = [&](const Node &child) {
        const auto physical = [&](std::int64_t width, std::int64_t height) {
            const auto along = node.horizontal ? width : height;
            const auto across = node.horizontal ? height : width;
            return across <= cross ? Range{along, span} : empty;
        };
        if (!child.first || child.drop_horizontal) {
            return std::array{physical(child.content_min_width, child.content_min_height), empty};
        }
        const auto &first = nodes_[child.first];
        const auto &second = nodes_[child.second];
        auto wide = physical(std::int64_t{first.content_min_width} + second.content_min_width,
                             std::max(first.content_min_height, second.content_min_height));
        auto tall = physical(std::max(first.content_min_width, second.content_min_width),
                             std::int64_t{first.content_min_height} + second.content_min_height);
        if (node.horizontal) {
            wide.first = std::max<std::int64_t>(wide.first, cross);
            tall.second = std::min<std::int64_t>(tall.second, std::int64_t{cross} - 1);
        } else {
            wide.second = std::min<std::int64_t>(wide.second, cross);
            tall.first = std::max<std::int64_t>(tall.first, std::int64_t{cross} + 1);
        }
        return std::array{wide, tall};
    };

    std::optional<std::pair<std::int64_t, std::int64_t>> best;
    for (const auto &[min_first, max_first] : ranges(nodes_[node.first])) {
        for (const auto &[min_second, max_second] : ranges(nodes_[node.second])) {
            const auto low = std::max(min_first, span - max_second);
            const auto high = std::min(max_first, span - min_second);
            if (low <= high) {
                const auto position = std::clamp<std::int64_t>(requested, low, high);
                const auto rank = std::pair{std::abs(position - requested), position};
                if (!best || rank < *best) {
                    best = rank;
                }
            }
        }
    }
    return best ? std::optional{coordinate(best->second)} : std::nullopt;
}

void WorkspaceLayout::reflow() {
    // The last offset and shape remain useful after the last output is lost.
    if (area_.width == 0 || area_.height == 0) {
        return;
    }
    if (settings_.kind == Layout::scrolling) {
        project_columns();
        return;
    }
    if (!root_) {
        return;
    }
    project_tree(false);
    // Constraints can change a descendant's longest axis. Recompute its minima
    // before projecting again and bound the passes for conflicting constraints.
    constexpr int max_constraint_passes = 8;
    for (int pass = 0; pass < max_constraint_passes; ++pass) {
        measure_tree();
        if (!project_tree(true)) {
            break;
        }
    }
    for (auto id = root_; id;) {
        auto &node = nodes_[id];
        if (node.drop_horizontal) {
            if (node.drop_region.width == 0 && node.drop_region.height == 0) {
                node.drop_region = node.box;
            } else if (node.drop_region.width != node.box.width ||
                       node.drop_region.height != node.box.height) {
                node.drop_horizontal.reset();
            }
        }
        id = node.first ? node.first : next_subtree(id);
    }
}

void WorkspaceLayout::project_columns() {
    const auto area = tiling_area();
    std::int64_t width = 0;
    for (const auto id : columns_) {
        auto &tile = tiles_.at(id);
        const auto &window = tile.window;
        const int default_width =
            coordinate(std::lround(static_cast<double>(area_.width) * *window.column_fraction));
        const int minimum = outer_minimum(window.limits.min_width, settings_.border_width);
        const int maximum = outer_maximum(window.limits.max_width, settings_.border_width);
        tile.box.width = std::clamp(window.column_width.value_or(default_width), minimum,
                                    std::max(minimum, maximum));
        if (id != columns_.front()) {
            width = strip_add(width, settings_.inner_gap);
        }
        tile.strip_start = width;
        width = strip_add(width, tile.box.width);
    }
    strip_width_ = width;
    offset_ = std::clamp<std::int64_t>(offset_, 0, std::max<std::int64_t>(0, width - area.width));
    for (const auto id : columns_) {
        auto &tile = tiles_.at(id);
        tile.box.x = shift(area.x, tile.strip_start - offset_);
        tile.box.y = area.y;
        tile.box.height = area.height;
    }
}

std::optional<TileDrop> WorkspaceLayout::drop_at(double x, double y) const noexcept {
    if (!contains_point(tiling_area(), x, y)) {
        return {};
    }
    for (const auto &[id, tile] : tiles_) {
        const auto box = tile.box;
        if (!contains_point(box, x, y)) {
            continue;
        }
        const double left = (x - box.x) / box.width;
        const double top = (y - box.y) / box.height;
        const auto side = left < 0.5 ? Direction::left : Direction::right;
        if (settings_.kind == Layout::dwindle &&
            std::min(top, 1 - top) < std::min(left, 1 - left)) {
            return TileDrop{id, top < 0.5 ? Direction::up : Direction::down};
        }
        return TileDrop{id, side};
    }
    return {};
}

bool WorkspaceLayout::reveal(WindowId id, std::optional<Direction> from) {
    const auto tile = tiles_.find(id);
    const int available = tiling_area().width;
    if (settings_.kind != Layout::scrolling || tile == tiles_.end() || available == 0 ||
        area_.height == 0) {
        return false;
    }
    const auto start = tile->second.strip_start;
    const auto end = strip_add(start, tile->second.box.width);
    std::int64_t next;
    if (tile->second.box.width <= available) {
        next = std::clamp(offset_, end - available, start);
    } else {
        const auto right_aligned = end - available;
        if (from == Direction::right) {
            next = start;
        } else if (from == Direction::left) {
            next = right_aligned;
        } else {
            next = std::abs(offset_ - start) <= std::abs(offset_ - right_aligned) ? start
                                                                                  : right_aligned;
        }
    }
    next = std::clamp<std::int64_t>(next, 0, std::max<std::int64_t>(0, strip_width_ - available));
    if (offset_ == next) {
        return false;
    }
    offset_ = next;
    project_columns();
    return true;
}

std::optional<WindowId> WorkspaceLayout::neighbor(WindowId id, Direction direction) const noexcept {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || direction == Direction::none) {
        return {};
    }
    if (settings_.kind == Layout::scrolling) {
        if (!horizontal(direction) || columns_.size() < 2) {
            return {};
        }
        auto at = std::ranges::find(columns_, id);
        if (direction == Direction::left) {
            if (at != columns_.begin()) {
                return *std::prev(at);
            }
            return {};
        }
        ++at;
        if (at != columns_.end()) {
            return *at;
        }
        return {};
    }

    const bool axis = horizontal(direction);
    const int sign = before(direction) ? -1 : 1;
    const auto center = [axis, sign](WindowRect box) {
        const auto x = std::int64_t{2} * box.x + box.width;
        const auto y = std::int64_t{2} * box.y + box.height;
        return axis ? std::pair{x * sign, y} : std::pair{y * sign, x};
    };
    const auto origin = tile->second.box;
    const auto [from_axis, from_cross] = center(origin);
    using Rank = std::tuple<bool, std::int64_t, std::int64_t, WindowId>;
    std::optional<Rank> closest;
    for (const auto &[candidate, position] : tiles_) {
        if (candidate == id) {
            continue;
        }
        const auto [towards, across] = center(position.box);
        const auto cross = std::abs(across - from_cross);
        if (towards > from_axis) {
            const auto spans = axis ? std::int64_t{origin.height} + position.box.height
                                    : std::int64_t{origin.width} + position.box.width;
            const auto rank = Rank{cross >= spans, towards - from_axis + cross, cross, candidate};
            if (!closest || rank < *closest) {
                closest = rank;
            }
        }
    }
    return closest ? std::optional{std::get<3>(*closest)} : std::nullopt;
}

std::optional<WindowId> WorkspaceLayout::close_focus(WindowId id) const noexcept {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end()) {
        return {};
    }
    if (settings_.kind == Layout::scrolling) {
        const auto right = neighbor(id, Direction::right);
        return right ? right : neighbor(id, Direction::left);
    }
    const auto leaf = tile->second.position;
    const auto parent = nodes_[leaf].parent;
    if (!parent) {
        return {};
    }
    const auto &split = nodes_[parent];
    const auto sibling = split.first == leaf ? split.second : split.first;
    const auto end = next_subtree(sibling);
    const auto from = tile->second.box;
    std::optional<std::pair<std::int64_t, WindowId>> nearest;
    for (auto at = sibling; at != end;) {
        const auto &node = nodes_[at];
        if (node.window) {
            const auto box = node.box;
            const auto distance =
                std::abs((std::int64_t{box.x} - from.x) * 2 + box.width - from.width) +
                std::abs((std::int64_t{box.y} - from.y) * 2 + box.height - from.height);
            const auto rank = std::pair{distance, node.window};
            if (!nearest || rank < *nearest) {
                nearest = rank;
            }
        }
        at = node.first ? node.first : next_subtree(at);
    }
    return nearest ? std::optional{nearest->second} : std::nullopt;
}

bool WorkspaceLayout::swap(WindowId first, WindowId second) {
    if (first == second || !contains(first) || !contains(second)) {
        return false;
    }
    if (settings_.kind == Layout::scrolling) {
        std::iter_swap(std::ranges::find(columns_, first), std::ranges::find(columns_, second));
    } else {
        auto &left = tiles_.at(first).position;
        auto &right = tiles_.at(second).position;
        std::swap(nodes_[left].window, nodes_[right].window);
        std::swap(left, right);
    }
    reflow();
    return true;
}

bool WorkspaceLayout::move(WindowId id, Direction direction) {
    const auto target = neighbor(id, direction);
    if (!target) {
        return false;
    }
    if (settings_.kind == Layout::scrolling) {
        return swap(id, *target);
    }
    const auto window = tiles_.at(id).window;
    // The collapse releases the two nodes needed for the new split. Keep the
    // tile record and reuse those nodes so the move needs no new allocation.
    erase_split(id);
    insert_split(window, {}, TileDrop{*target, direction});
    reflow();
    return true;
}

bool WorkspaceLayout::resize_column(WindowId id, int delta) {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || delta == 0 || area_.width == 0 || area_.height == 0) {
        return false;
    }
    auto &window = tile->second.window;
    const int minimum = outer_minimum(window.limits.min_width, settings_.border_width);
    const int maximum = outer_maximum(window.limits.max_width, settings_.border_width);
    const auto width = tile->second.box.width;
    const auto next =
        std::clamp(coordinate(std::int64_t{width} + delta), minimum, std::max(minimum, maximum));
    if (width == next) {
        return false;
    }
    window.column_width = next;
    reflow();
    return true;
}

bool WorkspaceLayout::resize_split(WindowId id, Direction direction, int distance,
                                   bool allow_opposite) {
    const auto tile = tiles_.find(id);
    if (tile == tiles_.end() || distance <= 0 || area_.width == 0 || area_.height == 0) {
        return false;
    }
    const bool axis = horizontal(direction);
    const int delta = before(direction) ? -distance : distance;
    auto child = tile->second.position;
    while (const auto parent = nodes_[child].parent) {
        auto &split = nodes_[parent];
        if (split.horizontal == axis &&
            (allow_opposite || (split.second == child) == before(direction))) {
            const auto &first = nodes_[split.first];
            const auto &second = nodes_[split.second];
            const int current = axis ? first.box.width : first.box.height;
            const int available = current + (axis ? second.box.width : second.box.height);
            const int requested = coordinate(std::int64_t{current} + delta);
            const auto [first_min, second_min] = split_minima(split, available);
            const int preferred = first_extent(available, requested, first_min, second_min);
            const int next = first.first || second.first
                                 ? fit_children(split, available, preferred).value_or(preferred)
                                 : preferred;
            if (next == current || available <= 1) {
                return false;
            }
            split.ratio = static_cast<double>(next) / available;
            reflow();
            return true;
        }
        child = parent;
    }
    return false;
}

bool WorkspaceLayout::resize(WindowId id, Direction direction, int distance) {
    if (direction == Direction::none || distance <= 0) {
        return false;
    }
    if (settings_.kind == Layout::scrolling) {
        return horizontal(direction) && resize_column(id, before(direction) ? -distance : distance);
    }
    return resize_split(id, direction, distance, false);
}

bool WorkspaceLayout::resize_by(WindowId id, int dx, int dy) {
    if (settings_.kind == Layout::scrolling) {
        return resize_column(id, dx);
    }
    const auto magnitude = [](int delta) { return coordinate(std::abs(std::int64_t{delta})); };
    const bool x =
        dx && resize_split(id, dx < 0 ? Direction::left : Direction::right, magnitude(dx), true);
    const bool y =
        dy && resize_split(id, dy < 0 ? Direction::up : Direction::down, magnitude(dy), true);
    return x || y;
}

} // namespace seaside

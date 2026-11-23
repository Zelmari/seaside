#ifndef SEASIDE_LAYOUT_HPP
#define SEASIDE_LAYOUT_HPP

#include "config.hpp"
#include "state.hpp"

#include <map>
#include <utility>
#include <vector>

namespace seaside {

struct WindowRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const WindowRect &) const = default;
};

WindowRect inset_window_rect(WindowRect box, int margin) noexcept;

struct TileLimits {
    int min_width = 0;
    int min_height = 0;
    int max_width = 0;
    int max_height = 0;
    bool operator==(const TileLimits &) const = default;
};

struct TiledWindow {
    WindowId id;
    std::uint64_t opening_order;
    TileLimits limits{};
    std::optional<int> column_width{};
    std::optional<double> column_fraction{};
};

struct TileDrop {
    WindowId target;
    Direction side;
    bool operator==(const TileDrop &) const = default;
};

struct LayoutSettings {
    Layout kind = Layout::dwindle;
    Insertion insertion = Insertion::focused;
    double split_ratio = 0.5;
    double default_column_width = 0.5;
    int outer_gap = 10;
    int inner_gap = 5;
    int border_width = 2;
    bool operator==(const LayoutSettings &) const = default;
};

// Policy and geometry only. Membership and focus belong to WindowManager.
class WorkspaceLayout {
  public:
    explicit WorkspaceLayout(LayoutSettings settings = {});
    void set_settings(LayoutSettings settings);
    void set_area(WindowRect area);
    const LayoutSettings &settings() const noexcept { return settings_; }
    WindowRect viewport() const noexcept { return area_; }

    bool add(TiledWindow window, std::optional<WindowId> focused = {},
             std::optional<TileDrop> drop = {});
    std::optional<TiledWindow> erase(WindowId id);
    bool contains(WindowId id) const noexcept;
    bool update_limits(WindowId id, TileLimits limits);
    std::optional<WindowRect> geometry(WindowId id) const noexcept;
    int border_width(WindowId id) const noexcept;
    std::optional<TileDrop> drop_at(double x, double y) const noexcept;

    bool reveal(WindowId id, std::optional<Direction> from = {});
    std::optional<WindowId> neighbor(WindowId id, Direction direction) const noexcept;
    std::optional<WindowId> close_focus(WindowId id) const noexcept;
    bool move(WindowId id, Direction direction);
    bool swap(WindowId first, WindowId second);
    bool resize(WindowId id, Direction direction, int distance);
    bool resize_by(WindowId id, int dx, int dy);
    bool has_resize_edge(WindowId id, Direction edge) const noexcept;
    bool resize_edge(WindowId id, Direction edge, int delta);

  private:
    // Index zero is the null link. Freed nodes form a list through `parent`.
    using NodeId = std::size_t;
    struct Tile {
        TiledWindow window{};
        WindowRect box{};
        std::int64_t strip_start = 0;
        std::size_t position = 0;
    };
    struct Node {
        NodeId parent = 0;
        NodeId first = 0;
        NodeId second = 0;
        WindowId window = 0;
        WindowRect box{};
        double ratio = 0.5;
        int min_width = 1;
        int min_height = 1;
        int content_min_width = 1;
        int content_min_height = 1;
        bool horizontal = true;
        std::optional<bool> drop_horizontal{};
        WindowRect drop_region{};
    };

    NodeId allocate_node();
    void release_node(NodeId id) noexcept;
    bool insert_split(TiledWindow window, std::optional<WindowId> focused,
                      std::optional<TileDrop> drop);
    bool insert_column(TiledWindow window, std::optional<WindowId> focused,
                       std::optional<TileDrop> drop);
    void erase_split(WindowId id) noexcept;
    void reflow();
    NodeId next_subtree(NodeId id) const noexcept;
    NodeId first_leaf(NodeId id) const noexcept;
    bool project_tree(bool constrain);
    void measure_tree();
    std::pair<int, int> split_minima(const Node &node, int extent) const noexcept;
    std::optional<int> fit_children(const Node &node, int span, int requested) const noexcept;
    void project_columns();
    NodeId split_for(WindowId id, Direction direction, bool shared_edge) const noexcept;
    bool resize_split(NodeId id, int delta);
    bool resize_column(WindowId id, int delta);
    bool resize_column_divider(WindowId left, WindowId right, int delta);
    WindowRect tiling_area() const noexcept;

    LayoutSettings settings_;
    WindowRect area_;
    std::map<WindowId, Tile> tiles_;
    std::vector<Node> nodes_;
    NodeId root_ = 0;
    NodeId free_node_ = 0;
    std::vector<WindowId> columns_;
    std::int64_t offset_ = 0;
    std::int64_t strip_width_ = 0;
};

} // namespace seaside
#endif

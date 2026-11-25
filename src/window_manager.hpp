#ifndef SEASIDE_WINDOW_MANAGER_HPP
#define SEASIDE_WINDOW_MANAGER_HPP

#include "layout.hpp"

namespace seaside {

enum class ResizeResult { invalid, unchanged, changed };

struct WindowView {
    WindowRect geometry;
    bool visible = false;
    bool maximized = false;
    bool fullscreen = false;
    bool pinned = false;
    bool tiled = false;
    bool tile_layout = false;
    WindowRect clip;
    int border_width = 0;
    std::uint64_t stacking = 0;
    bool unclipped_move = false;
};

// Event-loop policy. The caller owns Wayland objects, configure sequencing,
// output placement, and the projection of these decisions into the scene.
class WindowManager final {
  public:
    explicit WindowManager(bool tiling = true);
    explicit WindowManager(const Config &config);
    WindowManager(const WindowManager &) = delete;
    WindowManager &operator=(const WindowManager &) = delete;

    bool add_output(std::string_view id, WindowRect bounds, WindowRect usable,
                    bool primary = false);
    bool remove_output(std::string_view id);
    bool update_output(std::string_view id, WindowRect bounds, WindowRect usable);
    bool focus_output(std::string_view id);
    bool set_primary_output(std::string_view id);
    std::optional<std::string_view> primary_output() const noexcept { return primary_output_; }
    bool has_fullscreen(std::string_view id) const;
    bool focus_output_at(double x, double y);
    std::optional<std::string_view> focused_output() const noexcept;
    std::optional<WindowRect> output_bounds(std::string_view id) const noexcept;
    std::optional<WorkspaceId> focused_workspace() const noexcept;
    std::optional<std::string_view> workspace_output(WorkspaceId id) const noexcept;
    std::optional<std::string_view> workspace_owner(WorkspaceId id) const noexcept;
    std::optional<std::string_view> window_output(WindowId id) const noexcept;

    std::optional<WindowId> create_window(std::optional<WindowId> parent = {});
    bool initialize_window(WindowId id);
    std::optional<WindowRect> initial_tile_geometry(WindowId id) const;
    bool map_window(WindowId id, WindowRect committed, bool ordinary_buffer = true);
    bool unmap_window(WindowId id);
    bool destroy_window(WindowId id);
    bool set_parent(WindowId id, std::optional<WindowId> parent);
    bool commit_geometry(WindowId id, WindowRect committed);
    bool set_size_limits(WindowId id, int min_width, int min_height, int max_width, int max_height);
    WindowView view(WindowId id) const;
    bool is_descendant(WindowId id, WindowId ancestor) const;

    std::optional<WindowId> focused_window() const noexcept { return focused_window_; }
    bool click(WindowId id);
    bool focus_direction(Direction direction);
    bool cycle_focus(bool held);
    void finish_focus_cycle();

    bool select_workspace(WorkspaceId id);
    bool move_to_workspace(WindowId id, WorkspaceId destination);

    bool set_maximized(WindowId id, bool enabled);
    bool set_fullscreen(WindowId id, bool enabled);
    bool toggle_pin(WindowId id);
    bool toggle_tiling(WindowId id);
    void apply_layout_config(const Config &config);
    bool allow_edge_drag() const noexcept { return allow_edge_drag_; }
    bool move_window(WindowId id, Direction direction, int distance = 32);
    bool move_window_by(WindowId id, int dx, int dy);
    bool swap_window(WindowId id, Direction direction);
    bool resize_window(WindowId id, Direction direction, int distance);
    ResizeResult resize_window_by(WindowId id, int dw, int dh);
    bool has_tile_resize_edge(WindowId id, Direction edge) const noexcept;
    bool resize_tile_edges(WindowId id, WindowRect start, Direction horizontal, int dx,
                           Direction vertical, int dy);
    bool resize_floating(WindowId id, WindowRect start, std::optional<int> dx,
                         std::optional<int> dy, bool from_left, bool from_top);
    bool begin_move(WindowId id);
    bool finish_move(WindowId id, double x, double y);
    void cancel_move(WindowId id);

    const State &state() const noexcept { return state_; }

  private:
    struct Output {
        WindowRect bounds;
        WindowRect usable;
        bool connected = false;
    };

    struct Workspace {
        std::vector<WindowId> focus_history;
        WorkspaceLayout layout;
        std::uint64_t last_shown = 0;
    };

    struct Window {
        // Ordinary coordinates are local to the work area. Dialog movement
        // records an offset from the centered parent position instead.
        WindowRect ordinary;
        TileLimits limits;
        int dialog_dx = 0;
        int dialog_dy = 0;
        std::optional<WindowId> parent;
        std::uint64_t stacking = 0;
        bool positioned = false;
        bool edge_positioned = false;
        bool maximized = false;
        bool fullscreen = false;
        bool pinned = false;
    };

    struct FocusCycle {
        std::vector<WindowId> windows;
        std::size_t next = 0;
    };

    struct TileMove {
        WorkspaceId workspace;
        TiledWindow tile;
        WorkspaceLayout previous;
        WindowRect geometry;
        int border_width;
        std::uint64_t stacking;
    };

    struct FloatingMove {
        WindowId id;
        WindowRect geometry;
    };

    bool show_workspace(std::string_view output, WorkspaceId workspace);
    bool reconcile_workspaces(WorkspaceId previous_workspace);
    bool focus_on_output(std::string_view id, bool restore_workspace_focus = false);
    bool relocate_window(WindowId id, WorkspaceId workspace, WorkspaceLayout *prepared = nullptr);
    void remember_focus(WindowId id, WorkspaceId workspace);
    void forget_focus(WindowId id);
    void set_focus(std::optional<WindowId> id, bool remember = true,
                   std::optional<Direction> direction = {});
    void restore_focus();
    bool eligible(WindowId id, WorkspaceId workspace) const;
    bool is_visible(WindowId id) const;
    bool is_tiled(WindowId id, const Window &window) const;
    bool has_tile_ancestor(WindowId id) const;
    bool unclipped_move(WindowId id) const;
    std::optional<WindowId> local_focus(std::string_view output, WorkspaceId workspace) const;
    std::optional<WindowId> fullscreen_on(std::string_view output) const;
    WindowRect geometry(WindowId id, const Window &window) const;
    void place_floating(WindowId id, WindowRect area, std::int64_t x, std::int64_t y,
                        bool allow_overhang);
    void position_window(WindowId id, Window &window);
    WorkspaceLayout &layout_for(WindowId id);
    void update_layout_area(WorkspaceId id);
    void update_layout_areas();
    std::optional<WindowId> layout_focus(WorkspaceId id) const;
    void attach_tile(WindowId id);
    void remove_tile(WindowId id);
    void restore_after_removal(std::optional<WindowId> preferred, WorkspaceId workspace);
    void reveal(WindowId id, std::optional<Direction> direction = {});
    void cancel_active_move();

    State state_;
    bool tiling_;
    bool allow_edge_drag_ = false;
    LayoutSettings layout_settings_;
    std::map<OutputId, Output, std::less<>> outputs_;
    std::map<WorkspaceId, Workspace> workspaces_;
    std::map<WindowId, Window> windows_;
    std::optional<std::string_view> focused_output_;
    std::optional<std::string_view> primary_output_;
    WorkspaceId disconnected_workspace_ = 1;
    std::optional<WindowId> focused_window_;
    std::optional<FocusCycle> cycle_;
    std::optional<TileMove> tile_move_;
    std::optional<FloatingMove> floating_move_;
    std::uint64_t sequence_ = 0;
};

} // namespace seaside
#endif

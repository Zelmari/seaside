#ifndef SEASIDE_WINDOW_MANAGER_HPP
#define SEASIDE_WINDOW_MANAGER_HPP

#include "config.hpp"
#include "state.hpp"

namespace seaside {

struct WindowRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const WindowRect &) const = default;
};

struct WindowView {
    WindowRect geometry;
    bool visible = false;
    bool maximized = false;
    bool fullscreen = false;
    bool pinned = false;
    bool tiled = false;
    std::uint64_t stacking = 0;
};

// Event-loop policy. The caller owns Wayland objects, configure sequencing,
// output placement, and the projection of these decisions into the scene.
class WindowManager final {
  public:
    explicit WindowManager(WorkspaceMode mode, bool tiling = true) noexcept;
    WindowManager(const WindowManager &) = delete;
    WindowManager &operator=(const WindowManager &) = delete;

    bool add_output(std::string_view id, WindowRect bounds, WindowRect usable,
                    bool primary = false);
    bool remove_output(std::string_view id);
    bool update_output(std::string_view id, WindowRect bounds, WindowRect usable);
    bool focus_output(std::string_view id);
    bool focus_output_at(double x, double y);
    std::optional<std::string_view> focused_output() const noexcept;
    std::optional<WindowRect> output_bounds(std::string_view id) const noexcept;
    std::optional<WorkspaceId> focused_workspace() const noexcept;
    std::optional<std::string_view> window_output(WindowId id) const noexcept;

    std::optional<WindowId> create_window(std::optional<WindowId> parent = {});
    bool initialize_window(WindowId id);
    bool map_window(WindowId id, WindowRect committed);
    bool unmap_window(WindowId id);
    bool destroy_window(WindowId id);
    bool set_parent(WindowId id, std::optional<WindowId> parent);
    bool commit_geometry(WindowId id, WindowRect committed);
    WindowView view(WindowId id) const;

    std::optional<WindowId> focused_window() const noexcept { return focused_window_; }
    bool click(WindowId id);
    bool focus_direction(Direction direction);
    bool cycle_focus(bool held);
    void finish_focus_cycle();

    bool select_workspace(WorkspaceId id);
    bool move_to_workspace(WindowId id, WorkspaceId destination);
    bool transfer_workspace(std::string_view destination);

    bool set_maximized(WindowId id, bool enabled);
    bool set_fullscreen(WindowId id, bool enabled);
    bool toggle_pin(WindowId id);
    bool toggle_tiling(WindowId id);
    bool move_window(WindowId id, Direction direction, int distance = 32);
    bool move_window_by(WindowId id, int dx, int dy);
    bool resize_window_by(WindowId id, int dw, int dh);

    const State &state() const noexcept { return state_; }
    WorkspaceMode workspace_mode() const noexcept { return mode_; }

  private:
    struct Output {
        WindowRect bounds;
        WindowRect usable;
        std::optional<WorkspaceId> last_workspace;
        std::uint64_t last_focused = 0;
        bool connected = false;
    };

    struct Migration {
        OutputId origin;
        bool was_shown;
    };

    struct Workspace {
        std::vector<WindowId> focus_history;
        std::optional<Migration> migration;
        std::uint64_t last_shown = 0;
    };

    struct Window {
        // Ordinary coordinates are local to the work area. Dialog movement
        // records an offset from the centered parent position instead.
        WindowRect ordinary;
        int dialog_dx = 0;
        int dialog_dy = 0;
        std::optional<WindowId> parent;
        std::uint64_t stacking = 0;
        bool positioned = false;
        bool maximized = false;
        bool fullscreen = false;
        bool pinned = false;
    };

    struct FocusCycle {
        std::vector<WindowId> windows;
        std::size_t next = 0;
    };

    bool create_workspace(WorkspaceId id);
    std::optional<WorkspaceId> fresh_workspace();
    bool show_workspace(std::string_view output, WorkspaceId workspace);
    bool show_fallback(std::string_view output);
    std::optional<std::string_view> workspace_output(WorkspaceId workspace) const noexcept;
    bool focus_on_output(std::string_view id);
    bool relocate_window(WindowId id, WorkspaceId workspace);
    void remember_focus(WindowId id);
    void forget_focus(WindowId id, WorkspaceId workspace);
    void set_focus(std::optional<WindowId> id, bool remember = true);
    void restore_focus();
    bool eligible(WindowId id, WorkspaceId workspace) const;
    bool is_visible(WindowId id) const;
    bool is_tiled(WindowId id, const Window &window) const;
    bool is_descendant(WindowId id, WindowId ancestor) const;
    std::optional<WindowId> fullscreen_on(std::string_view output) const;
    WindowRect geometry(WindowId id, const Window &window) const;
    void position_window(WindowId id, Window &window);

    State state_;
    WorkspaceMode mode_;
    bool tiling_;
    std::map<OutputId, Output, std::less<>> outputs_;
    std::map<WorkspaceId, Workspace> workspaces_;
    std::map<WindowId, Window> windows_;
    std::optional<std::string_view> focused_output_;
    std::optional<std::string_view> primary_output_;
    std::optional<WindowId> focused_window_;
    std::optional<FocusCycle> cycle_;
    std::uint64_t sequence_ = 0;
};

} // namespace seaside
#endif

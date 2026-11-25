#ifndef SEASIDE_DESKTOP_HPP
#define SEASIDE_DESKTOP_HPP

#include "scene.hpp"
#include "window_manager.hpp"

#include <functional>
#include <set>
#include <utility>

struct seaside_tty_input;
struct wl_event_source;

namespace seaside {

enum class DesktopActionError { no_target, not_found, invalid_state };

// Connects the event-loop window policy to the production scene and input seat.
class Desktop final {
  public:
    Desktop(Scene &scene, const Config &config);
    ~Desktop() noexcept;
    Desktop(const Desktop &) = delete;
    Desktop &operator=(const Desktop &) = delete;

    void set_input(seaside_tty_input *input) noexcept;
    void enable_frame_paced_resize() noexcept;
    bool add_output(wlr_output *output, std::string_view id, WindowRect bounds) noexcept;
    void remove_output(wlr_output *output) noexcept;
    bool update_output(wlr_output *output, WindowRect bounds) noexcept;
    void set_primary_output(std::string_view id) noexcept;
    void apply_config(const Config &config) noexcept;
    void on_projection(std::function<void()> notify) { projected_ = std::move(notify); }
    void on_attention(std::function<void(WindowId, WorkspaceId)> notify) {
        attention_ = std::move(notify);
    }
    wlr_output *focused_output_handle() const noexcept;
    WindowRect output_bounds(wlr_output *output) const noexcept;
    bool fullscreen_inhibits_idle(wlr_output *output) const noexcept;
    std::optional<WindowId> keyboard_window() const noexcept { return keyboard_window_; }
    bool action(const Action &action, std::uint8_t modifiers = 0) noexcept;
    std::expected<bool, DesktopActionError> dispatch(const Action &action,
                                                     std::uint8_t modifiers = 0,
                                                     std::optional<WindowId> window = {}) noexcept;
    void modifiers_changed(std::uint8_t modifiers) noexcept;
    void finish_focus_cycle() noexcept;
    bool button(WindowId id, std::uint32_t button, bool pressed, std::uint8_t modifiers, double x,
                double y) noexcept;
    void motion(double x, double y) noexcept;
    void cancel_drag() noexcept;

    const WindowManager &windows() const noexcept { return windows_; }
    bool failed() const noexcept { return failed_; }

  private:
    struct Output {
        std::string id;
        WindowRect work;
    };
    struct PointerPosition {
        double x, y;
    };
    struct Drag {
        WindowId window;
        std::uint32_t button;
        bool resize;
        double x, y;
        bool started = false;
        bool client = false;
        bool tiled = false;
        bool resize_edges_locked = false;
        std::uint32_t available_resize_edges = 0;
        std::uint32_t resize_edges = 0;
        WindowRect initial_geometry{};
        std::optional<PointerPosition> pending{};
        std::pair<int, int> applied_edge_delta{};
        std::pair<int, int> delta(double target_x, double target_y) const noexcept;
    };
    struct OpeningWindow {
        WindowRect geometry;
        std::uint32_t serial;
    };

    static std::uint64_t on_window(void *self, const seaside_scene_window_event *event) noexcept;
    static void on_layer_changed(void *self) noexcept;
    static int on_opening_deadline(void *self) noexcept;
    static void before_output_frame(void *self, wlr_output *output) noexcept;
    std::uint64_t window_event(const seaside_scene_window_event &event);
    void project(WindowId resize = 0, bool geometry_only = false);
    void update_keyboard_focus(wlr_surface *layer_focus, WindowId focused) noexcept;
    bool stage_opening(WorkspaceId workspace);
    bool opening_ready() const;
    void project_if_opening_ready();
    void apply_drag_motion(double x, double y);
    wlr_output *window_output_handle(WindowId id) const noexcept;
    bool update_size_limits(WindowId id);
    void discard_drag();
    bool begin_client_drag(const seaside_scene_window_event &event);
    ResizeResult resize(WindowId id, Direction direction, int distance);

    Scene &scene_;
    WindowManager windows_;
    BindingMap bindings_;
    std::function<void()> projected_;
    std::function<void(WindowId, WorkspaceId)> attention_;
    FullscreenAspect fullscreen_aspect_ = FullscreenAspect::preserve;
    std::map<wlr_output *, Output> outputs_;
    std::map<WindowId, WindowRect> presented_;
    std::map<WindowId, OpeningWindow> opening_;
    std::set<WindowId> fitting_;
    wl_event_source *opening_deadline_ = nullptr;
    seaside_tty_input *input_ = nullptr;
    std::optional<Drag> drag_;
    std::optional<PointerPosition> pointer_;
    std::uint8_t cycle_modifiers_ = 0;
    bool frame_paced_resize_ = false;
    bool failed_ = false;
    std::optional<WindowId> keyboard_window_;
};

} // namespace seaside
#endif

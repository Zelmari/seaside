#ifndef SEASIDE_DESKTOP_HPP
#define SEASIDE_DESKTOP_HPP

#include "scene.hpp"
#include "window_manager.hpp"

#include <functional>
#include <set>

struct seaside_tty_input;

namespace seaside {

enum class DesktopActionError { no_target, not_found, invalid_state, ambiguous_output };

// Connects the event-loop window policy to the production scene and input seat.
class Desktop final {
  public:
    Desktop(Scene &scene, const Config &config);
    ~Desktop() noexcept;
    Desktop(const Desktop &) = delete;
    Desktop &operator=(const Desktop &) = delete;

    void set_input(seaside_tty_input *input) noexcept;
    bool add_output(wlr_output *output, std::string_view connector, std::string_view identity,
                    WindowRect bounds, std::string_view id = {}) noexcept;
    void remove_output(wlr_output *output, std::string_view ranked_fallback = {}) noexcept;
    bool update_output(wlr_output *output, WindowRect bounds) noexcept;
    void set_primary_output(std::string_view id) noexcept { windows_.set_primary_output(id); }
    void apply_config(const Config &config) noexcept;
    void apply_initial_config(const Config &config) noexcept;
    void on_projection(std::function<void()> notify) { projected_ = std::move(notify); }
    void on_attention(std::function<void(WindowId, WorkspaceId)> notify) {
        attention_ = std::move(notify);
    }
    wlr_output *focused_output_handle() const noexcept;
    WindowRect output_bounds(wlr_output *output) const noexcept;
    bool has_fullscreen(wlr_output *output) const noexcept;
    bool layer_has_keyboard_focus() const noexcept { return layer_has_focus_; }
    bool action(const Action &action, std::uint8_t modifiers = 0) noexcept;
    std::expected<bool, DesktopActionError>
    dispatch(const Action &action, std::uint8_t modifiers = 0, std::optional<WindowId> window = {},
             std::optional<WorkspaceId> workspace = {}) noexcept;
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
        std::string id, connector, identity;
        WindowRect work;
    };
    struct Drag {
        WindowId window;
        std::uint32_t button;
        bool resize;
        double x, y;
        bool started = false;
        bool client = false;
        std::uint32_t resize_edges = 0;
        WindowRect initial_geometry{};
    };

    static std::uint64_t on_window(void *self, const seaside_scene_window_event *event) noexcept;
    static void on_layer_changed(void *self) noexcept;
    std::uint64_t window_event(const seaside_scene_window_event &event);
    void project(WindowId resize = 0);
    bool update_size_limits(WindowId id);
    void discard_drag();
    bool begin_client_drag(const seaside_scene_window_event &event);
    bool resize(WindowId id, Direction direction, int distance);
    bool resize_by(WindowId id, int dx, int dy);

    Scene &scene_;
    WindowManager windows_;
    BindingMap bindings_;
    std::function<void()> projected_;
    std::function<void(WindowId, WorkspaceId)> attention_;
    FullscreenAspect fullscreen_aspect_ = FullscreenAspect::preserve;
    std::map<wlr_output *, Output> outputs_;
    std::set<WindowId> views_;
    seaside_tty_input *input_ = nullptr;
    std::optional<Drag> drag_;
    struct PointerPosition {
        double x, y;
    };
    std::optional<PointerPosition> pointer_;
    std::uint8_t cycle_modifiers_ = 0;
    bool failed_ = false;
    bool layer_has_focus_ = false;
};

} // namespace seaside
#endif

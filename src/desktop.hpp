#ifndef SEASIDE_DESKTOP_HPP
#define SEASIDE_DESKTOP_HPP

#include "scene.hpp"
#include "window_manager.hpp"

#include <set>

struct seaside_tty_input;

namespace seaside {

// Connects the event-loop window policy to the production scene and input seat.
class Desktop final {
  public:
    Desktop(Scene &scene, const Config &config);
    ~Desktop() noexcept;
    Desktop(const Desktop &) = delete;
    Desktop &operator=(const Desktop &) = delete;

    void set_input(seaside_tty_input *input) noexcept;
    bool add_output(wlr_output *output, std::string_view connector, std::string_view identity,
                    WindowRect bounds) noexcept;
    void remove_output(wlr_output *output) noexcept;
    wlr_output *focused_output_handle() const noexcept;
    WindowRect output_bounds(wlr_output *output) const noexcept;
    bool action(const Action &action, std::uint8_t modifiers = 0) noexcept;
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
        std::string connector, identity;
    };
    struct Drag {
        WindowId window;
        std::uint32_t button;
        bool resize;
        double x, y;
        bool started = false;
    };

    static std::uint64_t on_window(void *self, const seaside_scene_window_event *event) noexcept;
    std::uint64_t window_event(const seaside_scene_window_event &event);
    void project(WindowId resize = 0);
    bool update_size_limits(WindowId id);
    void discard_drag();
    std::optional<std::string_view> match(const OutputSelector &selector) const noexcept;
    bool resize(WindowId id, Direction direction, int distance);
    bool resize_by(WindowId id, int dx, int dy);

    Scene &scene_;
    WindowManager windows_;
    BindingMap bindings_;
    std::optional<OutputSelector> primary_;
    std::map<wlr_output *, Output> outputs_;
    std::set<WindowId> views_;
    seaside_tty_input *input_ = nullptr;
    std::optional<Drag> drag_;
    std::uint8_t cycle_modifiers_ = 0;
    bool failed_ = false;
};

} // namespace seaside
#endif

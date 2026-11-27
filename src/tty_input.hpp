#pragma once

#include "cursor_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>

struct timespec;
struct wl_display;
struct wlr_backend;
struct wlr_box;
struct wlr_drag_icon;
struct wlr_input_popup_surface_v2;
struct wlr_output;
struct wlr_seat;
struct wlr_seat_client;
struct wlr_surface;

namespace seaside {

struct TtyPointerTarget {
    wlr_surface *surface;
    double sx, sy;
    std::uint64_t window_id;
    double lx = 0, ly = 0;
};

struct TtyInputSnapshot {
    std::size_t pointers;
    std::size_t keyboards;
    bool failed;
    bool has_keyboard_focus;
    std::uint64_t forwarded_key_events;
    std::uint64_t forwarded_key_presses;
    std::uint64_t launch_requests;
};

class TtyInputObserver {
  public:
    virtual ~TtyInputObserver() = default;

    // Absolute coordinates are normalized. Relative and confined values are deltas.
    virtual void motion(seaside_pointer_motion, double, double) noexcept {}
    virtual void exit_requested() noexcept {}
    virtual void launch_requested() noexcept {}
    // Modifier bits match the binding table (Super, Ctrl, Alt, Shift).
    // Returning true consumes both the press and release.
    virtual bool shortcut_pressed(std::uint8_t, std::uint32_t) noexcept { return false; }
    virtual void modifiers_changed(std::uint8_t) noexcept {}
    // Hit-test for a null surface. Otherwise locate the exact surface even outside its bounds.
    virtual bool pointer_target(TtyPointerTarget &) noexcept { return false; }
    virtual bool pointer_target_at(double, double, TtyPointerTarget &) noexcept { return false; }
    virtual bool place_pointer(double, double) noexcept { return false; }
    // A zero-size box is an unclipped 1:1 target for a synthetic input observer.
    virtual bool pointer_content(wlr_surface *, seaside_pointer_content &content) noexcept {
        content = {{}, 1, 1, false, false};
        return true;
    }
    // A press may be consumed with its release. The window ID is zero over empty space.
    virtual bool pointer_button(std::uint64_t, std::uint32_t, bool, std::uint8_t) noexcept {
        return false;
    }
    virtual bool pointer_absolute_delta(double, double, double &, double &) noexcept {
        return false;
    }
    virtual void pointer_locked(bool) noexcept {}
    // A null shape restores the desktop arrow. A null cursor surface hides it.
    virtual void cursor_shape(const char *) noexcept {}
    virtual void cursor_surface(wlr_surface *, std::int32_t, std::int32_t) noexcept {}
    virtual void pointer_cancelled() noexcept {}
    virtual void drag_icon(wlr_drag_icon *) noexcept {}
    virtual void input_popup(wlr_input_popup_surface_v2 *, wlr_surface *,
                             const wlr_box *) noexcept {}
    // Motion reports after routing so it can wake the destination output.
    virtual void input_activity(bool) noexcept {}
};

struct TtyInputOptions {
    bool track_modifiers = false;
    bool pointer_targets = false;
    bool client_cursor = false;
    bool fixture_launch = false;
};

enum class TtyInputError { invalid_backend, allocation };

struct TtyInputState;

// The observer outlives the input. Attach on the Wayland thread before backend
// startup, which can discover devices synchronously. Destroy after dispatch stops.
class TtyInput final {
  public:
    static std::expected<std::unique_ptr<TtyInput>, TtyInputError>
    create(wlr_backend *backend, TtyInputObserver &observer, TtyInputOptions options = {}) noexcept;
    ~TtyInput() noexcept;
    TtyInput(const TtyInput &) = delete;
    TtyInput &operator=(const TtyInput &) = delete;

    bool enable_keyboard(wl_display *display) noexcept;
    void set_sensitivity(double sensitivity) noexcept;
    void set_inverse_scroll(bool inverse) noexcept;
    void set_auto_center_on_lock(bool enabled) noexcept;
    void set_focus(wlr_surface *surface) noexcept;
    wlr_seat *seat() const noexcept;
    void refresh_pointer() noexcept;
    void cursor_presented(wlr_output *output, const timespec *when) noexcept;
    // Take over a live client grab and return its initiating button, or zero.
    std::uint32_t begin_client_gesture(wlr_seat_client *client, wlr_surface *root,
                                       std::uint32_t serial) noexcept;
    void cancel_drag() noexcept;
    void cancel_drag_from(wlr_surface *root) noexcept;
    TtyInputSnapshot snapshot() const noexcept;

  private:
    explicit TtyInput(std::unique_ptr<TtyInputState> state) noexcept;
    std::unique_ptr<TtyInputState> state_;
};

} // namespace seaside

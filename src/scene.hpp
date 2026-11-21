#ifndef SEASIDE_SCENE_HPP
#define SEASIDE_SCENE_HPP

#include "render_assets.hpp"
#include "scene_bridge.h"

#include <cstdint>
#include <expected>
#include <memory>

namespace seaside {

enum class SceneError { invalid_arguments, allocation, output_attachment };
struct SceneImageRelease {
    void operator()(wlr_buffer *buffer) const noexcept;
};
using SceneImage = std::unique_ptr<wlr_buffer, SceneImageRelease>;

// XDG and scene mechanism. Handles and callbacks belong to the display thread.
// Keep the owner alive until dispatch stops or the display is destroyed: the
// shell global itself is display-owned. Destruction after display teardown is safe.
class Scene final {
  public:
    static std::expected<std::unique_ptr<Scene>, SceneError> create(wl_display *display) noexcept;
    ~Scene() noexcept;
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    std::expected<void, SceneError> add_output(wlr_output *output) noexcept;
    bool place_output(wlr_output *output, int x, int y) noexcept;
    void remove_output(wlr_output *output) noexcept;
    static std::expected<SceneImage, SceneError> prepare_wallpaper(Wallpaper wallpaper) noexcept;
    void set_wallpaper(SceneImage image) noexcept;
    void set_config_error(const std::optional<Diagnostic> &error) noexcept;
    seaside_scene_snapshot snapshot() const noexcept;
    void set_window_handler(seaside_scene_window_fn handler, void *data) noexcept;
    bool enable_window_capture(wlr_allocator *allocator, wlr_renderer *renderer,
                               wlr_ext_image_copy_capture_manager_v1 *copy) noexcept;
    bool window_capture_client_ready(pid_t pid, uid_t uid) const noexcept;
    void capture_pointer(double x, double y) noexcept;
    void place_window(std::uint64_t id, const seaside_scene_window_placement &placement) noexcept;
    wlr_surface *window_surface(std::uint64_t id) noexcept;
    bool close_window(std::uint64_t id) noexcept;
    seaside_scene_size_limits window_size_limits(std::uint64_t id) noexcept;
    seaside_scene_window_metadata window_metadata(std::uint64_t id) noexcept;
    bool window_at(double x, double y, seaside_scene_target &target) noexcept;
    void set_drag_icon(wlr_drag_icon *icon) noexcept;
    void move_drag_icon(int x, int y) noexcept;
    bool watch_client(pid_t pid, seaside_scene_focus_fn focus, void *data) noexcept;
    void clear_client_watch() noexcept;

  private:
    struct Appearance;
    Scene() noexcept;
    seaside_scene_bridge *bridge_;
    std::unique_ptr<Appearance> appearance_;
};

} // namespace seaside

#endif

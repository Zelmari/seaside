#ifndef SEASIDE_SCENE_HPP
#define SEASIDE_SCENE_HPP

#include "scene_bridge.h"

#include <expected>
#include <memory>

namespace seaside {

enum class SceneError { invalid_arguments, allocation, output_attachment };

// Minimal XDG/scene mechanism. Placement, focus, seat and output policy remain
// separate. Handles and callbacks belong to the display's owning thread.
// Keep the owner alive until dispatch stops or the display is destroyed: the
// shell global itself is display-owned. Destruction after display teardown is safe.
class Scene final {
  public:
    static std::expected<std::unique_ptr<Scene>, SceneError> create(wl_display *display) noexcept;
    ~Scene() noexcept;
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    std::expected<void, SceneError> add_output(wlr_output *output) noexcept;
    seaside_scene_snapshot snapshot() const noexcept;

  private:
    explicit Scene(seaside_scene_bridge *bridge) noexcept : bridge_(bridge) {}
    seaside_scene_bridge *bridge_;
};

} // namespace seaside

#endif

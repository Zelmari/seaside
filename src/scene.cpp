#include "scene.hpp"

#include <new>

namespace seaside {

std::expected<std::unique_ptr<Scene>, SceneError> Scene::create(wl_display *display) noexcept {
    if (!display) {
        return std::unexpected(SceneError::invalid_arguments);
    }
    std::unique_ptr<Scene> owner(new (std::nothrow) Scene(nullptr));
    if (!owner) {
        return std::unexpected(SceneError::allocation);
    }
    owner->bridge_ = seaside_scene_create(display);
    if (!owner->bridge_) {
        return std::unexpected(SceneError::allocation);
    }
    return owner;
}

Scene::~Scene() noexcept {
    seaside_scene_destroy(bridge_);
}

std::expected<void, SceneError> Scene::add_output(wlr_output *output) noexcept {
    if (!output) {
        return std::unexpected(SceneError::invalid_arguments);
    }
    if (!seaside_scene_add_output(bridge_, output)) {
        return std::unexpected(SceneError::output_attachment);
    }
    return {};
}

seaside_scene_snapshot Scene::snapshot() const noexcept {
    return seaside_scene_get_snapshot(bridge_);
}

} // namespace seaside

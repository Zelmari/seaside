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

bool Scene::place_output(wlr_output *output, int x, int y) noexcept {
    return seaside_scene_place_output(bridge_, output, x, y);
}

void Scene::remove_output(wlr_output *output) noexcept {
    seaside_scene_remove_output(bridge_, output);
}

void Scene::set_window_handler(seaside_scene_window_fn handler, void *data) noexcept {
    seaside_scene_set_window_handler(bridge_, handler, data);
}

void Scene::place_window(std::uint64_t id,
                         const seaside_scene_window_placement &placement) noexcept {
    seaside_scene_place_window(bridge_, id, &placement);
}

wlr_surface *Scene::window_surface(std::uint64_t id) noexcept {
    return seaside_scene_window_surface(bridge_, id);
}

bool Scene::close_window(std::uint64_t id) noexcept {
    return seaside_scene_close_window(bridge_, id);
}

seaside_scene_size_limits Scene::window_size_limits(std::uint64_t id) noexcept {
    return seaside_scene_window_size_limits(bridge_, id);
}

bool Scene::window_at(double x, double y, seaside_scene_target &target) noexcept {
    return seaside_scene_window_at(bridge_, x, y, &target);
}

bool Scene::watch_client(pid_t pid, seaside_scene_focus_fn focus, void *data) noexcept {
    return seaside_scene_watch_client(bridge_, pid, focus, data);
}

void Scene::clear_client_watch() noexcept {
    seaside_scene_clear_client_watch(bridge_);
}

} // namespace seaside

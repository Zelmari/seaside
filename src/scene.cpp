#include "scene.hpp"

#include "work_queue.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <new>
#include <wayland-server-core.h>

namespace seaside {

namespace {
void release_pixels(void *data) {
    delete static_cast<Wallpaper *>(data);
}

std::string banner_text(const Diagnostic &error) {
    std::string text = "Configuration error";
    if (!error.path.empty()) {
        text += " in ";
        text.append(error.path, 0, 4096);
    }
    if (error.line) {
        text += ':';
        text += std::to_string(error.line);
    }
    text += '\n';
    if (!error.field.empty()) {
        text.append(error.field, 0, 1024);
        text += ": ";
    }
    text.append(error.message, 0, 4096);
    return text;
}
} // namespace

struct Scene::Appearance {
    struct Viewport {
        int width;
        float scale;
        std::uint64_t revision;
        bool pending = true;
    };

    Appearance(wl_display *display, seaside_scene_bridge *bridge)
        : loop(wl_display_get_event_loop(display)), bridge(bridge) {
        display_destroy.listener.notify = on_display_destroy;
        display_destroy.owner = this;
        wl_display_add_destroy_listener(display, &display_destroy.listener);
    }

    ~Appearance() { wl_list_remove(&display_destroy.listener.link); }

    static void on_display_destroy(wl_listener *listener, void *) noexcept {
        auto &appearance = *reinterpret_cast<DisplayListener *>(listener)->owner;
        appearance.rasterizer.reset();
        appearance.loop = nullptr;
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }

    static void resize(void *data, wlr_output *output, int width, float scale) noexcept {
        auto &appearance = *static_cast<Appearance *>(data);
        try {
            if (width == 0) {
                appearance.viewports.erase(output);
            } else {
                appearance.viewports.insert_or_assign(
                    output, Viewport{width, scale, ++appearance.revision});
                appearance.rasterize_next();
            }
        } catch (...) {
            std::fputs("seaside: could not prepare config error banner viewport\n", stderr);
        }
    }

    void set_error(std::string text) {
        if (message == text) {
            return;
        }
        message = std::move(text);
        ++revision;
        for (auto &[output, viewport] : viewports) {
            viewport.revision = revision;
            viewport.pending = !message.empty();
            if (message.empty()) {
                seaside_scene_set_banner(bridge, output, nullptr, 0);
            }
        }
        rasterize_next();
    }

    void rasterize_next() {
        if (!loop || busy || message.empty()) {
            return;
        }
        auto next =
            std::ranges::find_if(viewports, [](const auto &item) { return item.second.pending; });
        if (next == viewports.end()) {
            return;
        }
        if (!rasterizer) {
            auto worker = WorkQueue::create(
                loop, {.workers = 1, .max_tasks = 2, .max_retained_bytes = 32 * 1024 * 1024});
            if (!worker) {
                std::fputs("seaside: could not create config error banner worker\n", stderr);
                return;
            }
            rasterizer = std::move(*worker);
        }
        auto &[output, viewport] = *next;
        viewport.pending = false;
        auto queued = rasterizer->submit(
            0, lifetime, max_config_banner_bytes,
            [text = message, width = viewport.width, scale = viewport.scale](std::stop_token stop) {
                if (stop.stop_requested()) {
                    return WorkResult{.value = {}, .error = WorkError::cancelled};
                }
                auto image = render_config_banner(text, width, scale);
                const auto bytes = image ? image->pixels.size() : 0;
                return WorkResult{.value = std::move(image), .retained_bytes = bytes};
            },
            [this, output, ticket = viewport.revision](WorkResult result) {
                busy = false;
                const auto found = viewports.find(output);
                if (found != viewports.end() && found->second.revision == ticket) {
                    auto image =
                        std::static_pointer_cast<const PixelImage>(std::move(result.value));
                    const int height = image ? image->logical_height : 0;
                    auto buffer = Scene::prepare_wallpaper(std::move(image));
                    if (buffer && height) {
                        seaside_scene_set_banner(bridge, output, buffer->release(), height);
                    } else {
                        std::fputs("seaside: could not render config error banner\n", stderr);
                    }
                }
                rasterize_next();
            });
        busy = queued.has_value();
        if (!busy) {
            viewport.pending = true;
            std::fputs("seaside: could not queue config error banner\n", stderr);
        }
    }

    wl_event_loop *loop;
    seaside_scene_bridge *bridge;
    struct DisplayListener {
        wl_listener listener{};
        Appearance *owner;
    } display_destroy;
    std::map<wlr_output *, Viewport> viewports;
    std::string message;
    std::uint64_t revision = 0;
    bool busy = false;
    std::shared_ptr<WorkLifetime> lifetime = std::make_shared<WorkLifetime>();
    std::unique_ptr<WorkQueue> rasterizer;
};

Scene::Scene() noexcept : bridge_(nullptr) {}

void SceneImageRelease::operator()(wlr_buffer *buffer) const noexcept {
    seaside_scene_drop_image(buffer);
}

std::expected<std::unique_ptr<Scene>, SceneError> Scene::create(wl_display *display) noexcept {
    if (!display) {
        return std::unexpected(SceneError::invalid_arguments);
    }
    std::unique_ptr<Scene> owner(new (std::nothrow) Scene());
    if (!owner) {
        return std::unexpected(SceneError::allocation);
    }
    owner->bridge_ = seaside_scene_create(display);
    if (!owner->bridge_) {
        return std::unexpected(SceneError::allocation);
    }
    try {
        owner->appearance_ = std::make_unique<Appearance>(display, owner->bridge_);
    } catch (...) {
        return std::unexpected(SceneError::allocation);
    }
    seaside_scene_set_appearance_handler(owner->bridge_, Appearance::resize,
                                         owner->appearance_.get());
    return owner;
}

Scene::~Scene() noexcept {
    seaside_scene_set_appearance_handler(bridge_, nullptr, nullptr);
    appearance_.reset();
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

void Scene::set_layer_handler(seaside_layer_output_fn focused_output,
                              seaside_layer_changed_fn changed, seaside_layer_hidden_fn hidden,
                              void *data) noexcept {
    seaside_scene_set_layer_handler(bridge_, focused_output, changed, hidden, data);
}

seaside_scene_rect Scene::layer_work_area(wlr_output *output, seaside_scene_rect bounds,
                                          bool fullscreen) noexcept {
    return seaside_scene_layer_work_area(bridge_, output, bounds, fullscreen);
}

void Scene::select_layer(wlr_surface *surface) noexcept {
    seaside_scene_select_layer(bridge_, surface);
}

wlr_surface *Scene::layer_keyboard_focus(wlr_output *focused_output) noexcept {
    return seaside_scene_layer_keyboard_focus(bridge_, focused_output);
}

wlr_output *Scene::visible_output(wlr_surface *surface) const noexcept {
    return seaside_scene_visible_output(bridge_, surface);
}

void Scene::set_linux_dmabuf(wlr_linux_dmabuf_v1 *dmabuf) noexcept {
    seaside_scene_set_linux_dmabuf(bridge_, dmabuf);
}

void Scene::set_render_policy(bool direct_scanout, bool allow_tearing, bool night_light,
                              int temperature_kelvin) noexcept {
    seaside_scene_set_render_policy(bridge_, direct_scanout, allow_tearing, night_light,
                                    temperature_kelvin);
}

void Scene::set_frame_observer(seaside_scene_frame_fn callback, void *data) noexcept {
    seaside_scene_set_frame_observer(bridge_, callback, data);
}

void Scene::set_before_frame(seaside_scene_before_frame_fn callback, void *data) noexcept {
    seaside_scene_set_before_frame(bridge_, callback, data);
}

void Scene::schedule_frame(wlr_output *output) noexcept {
    seaside_scene_schedule_frame(output);
}

wl_event_loop *Scene::event_loop() const noexcept {
    return seaside_scene_event_loop(bridge_);
}

seaside_scene_render_status Scene::render_status(wlr_output *output) const noexcept {
    return seaside_scene_get_render_status(bridge_, output);
}

std::expected<SceneImage, SceneError> Scene::prepare_wallpaper(Wallpaper wallpaper) noexcept {
    if (!wallpaper) {
        return SceneImage{};
    }
    if (wallpaper->width <= 0 || wallpaper->height <= 0 ||
        static_cast<std::uint64_t>(wallpaper->width) * wallpaper->height * 4 !=
            wallpaper->pixels.size()) {
        return std::unexpected(SceneError::invalid_arguments);
    }
    std::unique_ptr<Wallpaper> owner(new (std::nothrow) Wallpaper(std::move(wallpaper)));
    if (!owner) {
        return std::unexpected(SceneError::allocation);
    }
    SceneImage image(seaside_scene_create_image(
        (*owner)->width, (*owner)->height, (*owner)->pixels.data(), release_pixels, owner.get()));
    if (!image) {
        return std::unexpected(SceneError::allocation);
    }
    (void)owner.release();
    return image;
}

void Scene::set_wallpaper(SceneImage image) noexcept {
    seaside_scene_set_wallpaper(bridge_, image.release());
}

void Scene::set_config_error(const std::optional<Diagnostic> &error) noexcept {
    try {
        appearance_->set_error(error ? banner_text(*error) : std::string{});
    } catch (...) {
        std::fputs("seaside: could not prepare config error banner\n", stderr);
    }
}

void Scene::set_window_handler(seaside_scene_window_fn handler, void *data) noexcept {
    seaside_scene_set_window_handler(bridge_, handler, data);
}

bool Scene::enable_window_capture(wlr_allocator *allocator, wlr_renderer *renderer,
                                  wlr_ext_image_copy_capture_manager_v1 *copy) noexcept {
    return seaside_scene_enable_window_capture(bridge_, allocator, renderer, copy);
}

bool Scene::window_capture_client_ready(pid_t pid, uid_t uid) const noexcept {
    return seaside_scene_window_capture_client_ready(bridge_, pid, uid);
}

void Scene::capture_pointer(double x, double y) noexcept {
    seaside_scene_capture_pointer(bridge_, x, y);
}

void Scene::place_window(std::uint64_t id,
                         const seaside_scene_window_placement &placement) noexcept {
    seaside_scene_place_window(bridge_, id, &placement);
}

std::uint32_t Scene::configure_tiled_window(std::uint64_t id,
                                            seaside_scene_rect geometry) noexcept {
    return seaside_scene_configure_tiled_window(bridge_, id, geometry);
}

std::uint32_t Scene::window_commit_serial(std::uint64_t id) const noexcept {
    return seaside_scene_window_commit_serial(bridge_, id);
}

seaside_scene_rect Scene::window_content_geometry(std::uint64_t id) const noexcept {
    return seaside_scene_window_content_geometry(bridge_, id);
}

void Scene::set_fit_resize_buffer(std::uint64_t id, bool fit) noexcept {
    seaside_scene_set_fit_resize_buffer(bridge_, id, fit);
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

seaside_scene_window_metadata Scene::window_metadata(std::uint64_t id) noexcept {
    return seaside_scene_get_window_metadata(bridge_, id);
}

bool Scene::window_at(double x, double y, seaside_scene_target &target) noexcept {
    return seaside_scene_window_at(bridge_, x, y, &target);
}

void Scene::set_drag_icon(wlr_drag_icon *icon) noexcept {
    seaside_scene_set_drag_icon(bridge_, icon);
}

void Scene::move_drag_icon(int x, int y) noexcept {
    seaside_scene_move_drag_icon(bridge_, x, y);
}

bool Scene::watch_client(pid_t pid, seaside_scene_focus_fn focus, void *data) noexcept {
    return seaside_scene_watch_client(bridge_, pid, focus, data);
}

void Scene::clear_client_watch() noexcept {
    seaside_scene_clear_client_watch(bridge_);
}

} // namespace seaside

#include "scene_bridge.hpp"
#include "layer_scene.hpp"
#include "popup_grab.hpp"
#include "scene_blur.hpp"
#include "window_capture.hpp"
#include "wlr_compat.hpp"
#include "wlr_input_method_compat.hpp"
#include "wlr_scene_compat.hpp"

#include <drm_fourcc.h>
#include <glib.h>
#include <limits.h>
#include <math.h>
#include <memory>
#include <new>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <type_traits>
#include <unordered_map>
#include <wayland-server-core.h>
#include <xf86drm.h>

extern "C" {
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/transform.h>
}

static_assert(std::is_standard_layout_v<wlr_scene_node>);
static_assert(std::is_standard_layout_v<wlr_xdg_popup>);
static_assert(std::is_standard_layout_v<wlr_xdg_toplevel_icon_v1_buffer>);

struct SceneListener {
    wl_listener listener;
    seaside_scene_bridge *scene;

    void connect(wl_signal &signal, seaside_scene_bridge &owner,
                 void (*notify)(wl_listener *, void *) noexcept) noexcept {
        scene = &owner;
        listener.notify = notify;
        wl_signal_add(&signal, &listener);
    }

    static seaside_scene_bridge *owner(wl_listener *listener) noexcept {
        return reinterpret_cast<SceneListener *>(listener)->scene;
    }
};
static_assert(std::is_standard_layout_v<SceneListener>);

struct seaside_scene_bridge {
    struct wl_display *display;
    struct wlr_scene *scene;
    struct wlr_scene_tree *backgrounds, *windows, *drag_icons, *decorations;
    struct wlr_scene_tree *input_popups;
    struct wlr_buffer *wallpaper;
    struct wlr_xdg_shell *shell;
    struct wlr_xdg_activation_v1 *activation;
    SceneListener new_decoration;
    SceneListener set_icon, icon_manager_destroy;
    struct wlr_tearing_control_manager_v1 *tearing;
    struct seaside_window_capture *capture;
    struct seaside_layer_scene *layers;
    double cursor_x, cursor_y;
    bool cursor_known;
    struct wl_list views;
    std::unordered_map<uint64_t, struct scene_view *> views_by_id;
    struct wl_list popups;
    struct wl_list input_popup_views;
    struct wl_list outputs;
    SceneListener new_toplevel;
    SceneListener new_popup;
    SceneListener shell_destroy;
    SceneListener activation_destroy, request_activation;
    struct seaside_scene_snapshot snapshot;
    seaside_scene_window_fn window;
    void *window_data;
    seaside::SceneAppearanceObserver *appearance;
    struct scene_view *focused_view;
    pid_t watched_pid;
    seaside_scene_focus_fn focus;
    void *focus_data;
    bool attached;
    bool titlebars;
    uint32_t active_border_color, inactive_border_color;
    bool direct_scanout, allow_tearing, night_light;
    int temperature_kelvin;
    double blur_strength;
    int blur_passes;
    struct wl_list blur_surfaces;
    SceneListener blur_new_surface, blur_compositor_destroy;
    seaside_scene_frame_fn frame_observer;
    void *frame_observer_data;
    seaside_scene_before_frame_fn before_frame;
    void *before_frame_data;
};

struct window_projection {
    double scale_x, scale_y;
    int x, y;
};

struct scene_view {
    struct seaside_scene_bridge *owner;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_xdg_toplevel_icon_v1 *pending_icon;
    std::string icon_name, icon_png_base64;
    bool icon_dirty;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wl_listener request_decoration, decoration_destroy;
    struct seaside_window_source *capture;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *xdg_tree, *popup_tree;
    struct seaside_scene_blur *blur;
    struct wlr_scene_rect *fullscreen_background;
    struct wlr_scene_rect *borders[4];
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener set_parent;
    struct seaside_scene_window_placement placement;
    struct window_projection projection;
    struct wlr_box source_clip;
    struct wlr_box resize_clip;
    struct wl_list projected_surfaces;
    uint64_t id;
    uint32_t restore_serial, size_configure_serial;
    bool mapped;
};
static_assert(std::is_standard_layout_v<scene_view>);

struct scene_popup {
    struct scene_view *view;
    struct wlr_xdg_popup *popup;
    struct seaside_popup_focus focus;
    struct wlr_scene_tree *tree;
    struct wlr_scene_tree *surface_tree;
    struct wlr_box source_clip;
    struct wl_list link;
    struct wl_listener commit;
    struct wl_listener reposition;
    struct wl_listener destroy;
    struct wl_listener tree_destroy;
};
static_assert(std::is_standard_layout_v<scene_popup>);

enum class LutRequest { clean, pending, retry };

struct scene_output {
    struct seaside_scene_bridge *owner;
    struct wlr_scene_output *viewport;
    struct wlr_scene_buffer *wallpaper, *banner;
    struct wl_list link;
    struct wl_listener frame;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener present;
    int width, height;
    int banner_width, banner_height, banner_logical_height;
    bool banner_fullscreen;
    float scale;
    struct seaside_scene_render_status render_status;
    uint32_t pending_commit;
    const char *pending_mode, *pending_fallback;
    bool pending_tearing;
    struct wlr_drm_syncobj_timeline *unsubmitted_scanout;
    uint64_t unsubmitted_point;
    uint64_t tearing_window;
    bool scanout_tearing_rejected, composed_tearing_rejected;
    bool retry_composed, retry_frame;
    bool blur_active;
    LutRequest lut_request;
};
static_assert(std::is_standard_layout_v<scene_output>);

struct scene_input_popup {
    struct seaside_scene_bridge *scene;
    struct wlr_input_popup_surface_v2 *role;
    struct wlr_surface *focus;
    struct wlr_scene_tree *tree;
    struct wl_list link;
    struct wl_listener destroy, tree_destroy, commit, focus_destroy;
    struct wlr_box cursor, last_relative;
    bool has_cursor, sent_rectangle;
};
static_assert(std::is_standard_layout_v<scene_input_popup>);

static void update_input_popups(struct seaside_scene_bridge *scene);

struct scene_image {
    struct wlr_buffer base;
    const uint8_t *pixels;
    seaside_scene_image_release_fn release;
    void *owner;
};
static_assert(std::is_standard_layout_v<scene_image>);

struct blur_surface {
    struct seaside_scene_bridge *scene;
    struct wl_list link;
    struct wl_listener commit, destroy;
};
static_assert(std::is_standard_layout_v<blur_surface>);

static void invalidate_active_blur(struct seaside_scene_bridge *scene) {
    if (scene->blur_strength > 0) {
        struct scene_output *output;
        wl_list_for_each(output, &scene->outputs, link) {
            if (output->blur_active && output->viewport->output->enabled) {
                // wlroots may cull pixels hidden directly by an opaque node,
                // but they can still contribute to a neighboring blur sample.
                wlr_output_schedule_frame(output->viewport->output);
            }
        }
    }
}

static void blur_surface_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct blur_surface *surface = wl_container_of(listener, surface, commit);
    invalidate_active_blur(surface->scene);
}

static void remove_blur_surface(struct blur_surface *surface) {
    wl_list_remove(&surface->link);
    wl_list_remove(&surface->commit.link);
    wl_list_remove(&surface->destroy.link);
    free(surface);
}

static void blur_surface_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct blur_surface *surface = wl_container_of(listener, surface, destroy);
    invalidate_active_blur(surface->scene);
    remove_blur_surface(surface);
}

static void blur_new_surface(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    struct wlr_surface *surface = static_cast<wlr_surface *>(data);
    struct blur_surface *watched = static_cast<blur_surface *>(calloc(1, sizeof(*watched)));
    if (!watched) {
        return;
    }
    watched->scene = scene;
    watched->commit.notify = blur_surface_commit;
    watched->destroy.notify = blur_surface_destroy;
    wl_list_insert(&scene->blur_surfaces, &watched->link);
    wl_signal_add(&surface->events.commit, &watched->commit);
    wl_signal_add(&surface->events.destroy, &watched->destroy);
}

static void remove_blur_watches(struct seaside_scene_bridge *scene) {
    wl_list_remove(&scene->blur_new_surface.listener.link);
    wl_list_init(&scene->blur_new_surface.listener.link);
    wl_list_remove(&scene->blur_compositor_destroy.listener.link);
    wl_list_init(&scene->blur_compositor_destroy.listener.link);
    struct blur_surface *surface, *next;
    wl_list_for_each_safe(surface, next, &scene->blur_surfaces, link) {
        remove_blur_surface(surface);
    }
}

static void blur_compositor_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    auto *scene = SceneListener::owner(listener);
    remove_blur_watches(scene);
}

static void destroy_scene_image(struct wlr_buffer *base) noexcept {
    struct scene_image *image = wl_container_of(base, image, base);
    wlr_buffer_finish(base);
    image->release(image->owner);
    free(image);
}

static bool scene_image_data(struct wlr_buffer *base, uint32_t flags, void **data, uint32_t *format,
                             size_t *stride) noexcept {
    struct scene_image *image = wl_container_of(base, image, base);
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    *data = (void *)image->pixels;
    *format = DRM_FORMAT_XRGB8888;
    *stride = (size_t)base->width * 4;
    return true;
}

static void end_scene_image_data(struct wlr_buffer *base) noexcept {
    (void)base;
}

static const struct wlr_buffer_impl scene_image_impl = {
    .destroy = destroy_scene_image,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = scene_image_data,
    .end_data_ptr_access = end_scene_image_data,
};

struct wlr_buffer *seaside_scene_create_image(int width, int height, const uint8_t *pixels,
                                              seaside_scene_image_release_fn release, void *owner) {
    struct scene_image *image = static_cast<scene_image *>(calloc(1, sizeof(*image)));
    if (!image) {
        return nullptr;
    }
    image->pixels = pixels;
    image->release = release;
    image->owner = owner;
    wlr_buffer_init(&image->base, &scene_image_impl, width, height);
    return &image->base;
}

void seaside_scene_drop_image(struct wlr_buffer *buffer) {
    if (buffer) {
        wlr_buffer_drop(buffer);
    }
}

static struct scene_view *find_view(const struct seaside_scene_bridge *scene, uint64_t id) {
    if (!scene || !id) {
        return nullptr;
    }
    const auto view = scene->views_by_id.find(id);
    return view == scene->views_by_id.end() ? nullptr : view->second;
}

static struct scene_view *toplevel_view(const struct seaside_scene_bridge *scene,
                                        const struct wlr_xdg_toplevel *toplevel) {
    struct scene_view *view;
    wl_list_for_each(view, &scene->views, link) {
        if (view->toplevel == toplevel) {
            return view;
        }
    }
    return nullptr;
}

static struct scene_popup *surface_popup(const struct seaside_scene_bridge *scene,
                                         const struct wlr_surface *surface) {
    struct scene_popup *popup;
    wl_list_for_each(popup, &scene->popups, link) {
        if (popup->popup->base->surface == surface) {
            return popup;
        }
    }
    return nullptr;
}

static int clamp_coordinate(int64_t value) {
    return (int)(value < INT_MIN ? INT_MIN : value > INT_MAX ? INT_MAX : value);
}

/* Clip in layout coordinates first so very distant scrolling columns do not
 * create oversized surface-local coordinates or off-output scene rectangles. */
static struct wlr_box intersection(struct seaside_scene_rect clip, int64_t x, int64_t y,
                                   int64_t width, int64_t height) {
    int64_t right = x + width, bottom = y + height;
    const int64_t clip_right = (int64_t)clip.x + clip.width;
    const int64_t clip_bottom = (int64_t)clip.y + clip.height;
    x = x > clip.x ? x : clip.x;
    y = y > clip.y ? y : clip.y;
    right = right < clip_right ? right : clip_right;
    bottom = bottom < clip_bottom ? bottom : clip_bottom;
    if (right <= x || bottom <= y) {
        return wlr_box{};
    }
    return wlr_box{clamp_coordinate(x), clamp_coordinate(y), (int)(right - x), (int)(bottom - y)};
}

static bool has_viewport(struct seaside_scene_rect rect) {
    return rect.width > 0 && rect.height > 0;
}

static int clamp_rounded_coordinate(double value) {
    return (int)fmax(INT_MIN, fmin(INT_MAX, value));
}

static bool is_projected(const struct scene_view *view) {
    return view->projection.scale_x > 0;
}

static void update_window_projection(struct scene_view *view) {
    const struct seaside_scene_rect box = view->placement.geometry;
    const struct wlr_box source = view->toplevel->base->geometry;
    view->projection = window_projection{};
    if ((!view->placement.fullscreen && !view->placement.fit_resize_buffer) || !has_viewport(box) ||
        wlr_box_empty(&source) || (box.width == source.width && box.height == source.height)) {
        return;
    }

    int width = box.width, height = box.height;
    if (view->placement.fullscreen && !view->placement.stretch_fullscreen) {
        const int64_t width_by_height = (int64_t)width * source.height;
        const int64_t height_by_width = (int64_t)height * source.width;
        if (width_by_height > height_by_width) {
            width = (int)fmax(1, round((double)height_by_width / source.height));
        } else {
            height = (int)fmax(1, round((double)width_by_height / source.width));
        }
    }
    view->projection = window_projection{
        .scale_x = (double)width / source.width,
        .scale_y = (double)height / source.height,
        .x = (box.width - width) / 2,
        .y = (box.height - height) / 2,
    };
}

/* Source geometry is relative to the window's own XDG geometry, regardless
 * of the current placement or the crop for a particular surface. */
static void view_source_position(const struct scene_view *view, double *x, double *y) {
    *x -= view->placement.geometry.x;
    *y -= view->placement.geometry.y;
    if (is_projected(view)) {
        *x = (*x - view->projection.x) / view->projection.scale_x;
        *y = (*y - view->projection.y) / view->projection.scale_y;
    }
}

static void source_node_position(const struct scene_view *view, const struct wlr_scene_node *node,
                                 int64_t *x, int64_t *y) {
    *x = 0;
    *y = 0;
    /* Only buffer nodes are projected. Their tree ancestors retain the
     * unscaled positions managed by the wlroots XDG/subsurface helpers. */
    while (node != &view->tree->node && !(view->popup_tree && node == &view->popup_tree->node)) {
        *x += node->x;
        *y += node->y;
        node = &node->parent->node;
    }
}

static void clip_toplevel(struct scene_view *view) {
    const struct seaside_scene_window_placement *placement = &view->placement;
    const struct seaside_scene_rect viewport = placement->clip;
    const struct seaside_scene_rect box = placement->geometry;
    const struct wlr_box geometry = view->toplevel->base->geometry;
    view->source_clip = wlr_box{};
    view->resize_clip = wlr_box{};
    if (placement->fullscreen && has_viewport(box) && !wlr_box_empty(&geometry)) {
        view->source_clip = wlr_box{0, 0, geometry.width, geometry.height};
        wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node,
                                           is_projected(view) ? nullptr : &geometry);
        wlr_scene_node_set_enabled(&view->xdg_tree->node, true);
        return;
    }
    if (!has_viewport(viewport)) {
        wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node, nullptr);
        wlr_scene_node_set_enabled(&view->xdg_tree->node, !placement->tiled);
        return;
    }
    struct wlr_box clip = placement->tiled
                              ? intersection(viewport, box.x, box.y, box.width, box.height)
                              : wlr_box{viewport.x, viewport.y, viewport.width, viewport.height};
    if (clip.width <= 0 || clip.height <= 0) {
        wlr_scene_node_set_enabled(&view->xdg_tree->node, false);
        return;
    }
    const int64_t source_x = (int64_t)clip.x - box.x, source_y = (int64_t)clip.y - box.y;
    if (is_projected(view)) {
        /* Crop after scaling. Integer source-space clipping would otherwise
         * underfill or bleed past a partly visible scrolling tile. */
        view->resize_clip = wlr_box{clamp_coordinate(source_x), clamp_coordinate(source_y),
                                    clip.width, clip.height};
        if (placement->tiled) {
            view->source_clip = wlr_box{0, 0, geometry.width, geometry.height};
        }
        wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node, nullptr);
        wlr_scene_node_set_enabled(&view->xdg_tree->node, true);
        return;
    }
    view->source_clip =
        wlr_box{clamp_coordinate(source_x), clamp_coordinate(source_y), clip.width, clip.height};
    clip.x = clamp_coordinate(source_x + geometry.x);
    clip.y = clamp_coordinate(source_y + geometry.y);
    wlr_scene_subsurface_tree_set_clip(&view->xdg_tree->node, &clip);
    wlr_scene_node_set_enabled(&view->xdg_tree->node, true);
}

static void clip_view_popups(struct scene_view *view);
static void project_view_surfaces(struct scene_view *view);

struct projected_surface {
    struct scene_view *view;
    struct wlr_scene_surface *scene_surface;
    wlr_scene_buffer_point_accepts_input_func_t original_point_accepts_input;
    struct wlr_fbox source;
    int width, height;
    struct wl_listener commit, destroy;
    struct wl_list link;
};
static_assert(std::is_standard_layout_v<projected_surface>);

static void projected_surface_coords(const struct projected_surface *projected, double *x,
                                     double *y) {
    *x = projected->source.x + *x * projected->source.width / projected->width;
    *y = projected->source.y + *y * projected->source.height / projected->height;
}

static bool projected_point_accepts_input(struct wlr_scene_buffer *buffer, double *x,
                                          double *y) noexcept {
    const struct projected_surface *projected =
        static_cast<const projected_surface *>(buffer->node.data);
    projected_surface_coords(projected, x, y);
    return wlr_surface_point_accepts_input(projected->scene_surface->surface, *x, *y);
}

static void destroy_projected_surface(struct projected_surface *projected) {
    struct wlr_scene_buffer *buffer = projected->scene_surface->buffer;
    buffer->point_accepts_input = projected->original_point_accepts_input;
    buffer->node.data = nullptr;
    wl_list_remove(&projected->commit.link);
    wl_list_remove(&projected->destroy.link);
    wl_list_remove(&projected->link);
    free(projected);
}

static void projected_node_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct projected_surface *projected = wl_container_of(listener, projected, destroy);
    destroy_projected_surface(projected);
}

static void projected_surface_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct projected_surface *projected = wl_container_of(listener, projected, commit);
    project_view_surfaces(projected->view);
}

static struct projected_surface *get_projected_surface(struct scene_view *view,
                                                       struct wlr_scene_surface *surface) {
    struct wlr_scene_buffer *buffer = surface->buffer;
    if (buffer->point_accepts_input == projected_point_accepts_input) {
        return static_cast<projected_surface *>(buffer->node.data);
    }
    struct projected_surface *projected =
        static_cast<projected_surface *>(calloc(1, sizeof(*projected)));
    if (!projected) {
        view->owner->snapshot.failed = true;
        return nullptr;
    }
    projected->view = view;
    projected->scene_surface = surface;
    projected->original_point_accepts_input = buffer->point_accepts_input;
    projected->commit.notify = projected_surface_commit;
    projected->destroy.notify = projected_node_destroy;
    wl_list_init(&projected->commit.link);
    if (surface->surface != view->toplevel->base->surface &&
        !surface_popup(view->owner, surface->surface)) {
        wl_signal_add(&surface->surface->events.commit, &projected->commit);
    }
    wl_signal_add(&buffer->node.events.destroy, &projected->destroy);
    wl_list_insert(&view->projected_surfaces, &projected->link);
    buffer->node.data = projected;
    buffer->point_accepts_input = projected_point_accepts_input;
    return projected;
}

static struct wlr_fbox cropped_source(struct wlr_surface *surface, const struct wlr_fbox *crop) {
    struct wlr_fbox source;
    wlr_surface_get_buffer_source_box(surface, &source);
    if (crop->x == 0 && crop->y == 0 && crop->width == surface->current.width &&
        crop->height == surface->current.height) {
        return source;
    }
    const enum wl_output_transform transform = surface->current.transform;
    int width = surface->current.buffer_width, height = surface->current.buffer_height;
    wlr_fbox_transform(&source, &source, transform, width, height);
    wlr_output_transform_coords(transform, &width, &height);
    source.x += crop->x * source.width / surface->current.width;
    source.y += crop->y * source.height / surface->current.height;
    source.width *= crop->width / surface->current.width;
    source.height *= crop->height / surface->current.height;
    wlr_fbox_transform(&source, &source, wlr_output_transform_invert(transform), width, height);
    source.x = fmax(0, source.x);
    source.y = fmax(0, source.y);
    return source;
}

static void clip_projected_destination(struct wlr_box *destination, struct wlr_fbox *source,
                                       const struct wlr_box *clip) {
    if (!clip || wlr_box_empty(destination)) {
        return;
    }
    const struct wlr_box original = *destination;
    const struct seaside_scene_rect viewport = {clip->x, clip->y, clip->width, clip->height};
    *destination = intersection(viewport, original.x, original.y, original.width, original.height);
    if (wlr_box_empty(destination)) {
        return;
    }
    const double scale_x = source->width / original.width;
    const double scale_y = source->height / original.height;
    source->x += ((double)destination->x - original.x) * scale_x;
    source->y += ((double)destination->y - original.y) * scale_y;
    source->width = destination->width * scale_x;
    source->height = destination->height * scale_y;
}

static void project_buffer(struct scene_view *view, struct wlr_scene_buffer *buffer,
                           struct wlr_box clip, const struct wlr_box *destination_clip, int64_t x,
                           int64_t y) {
    const bool scaling = is_projected(view);
    struct projected_surface *projected =
        buffer->point_accepts_input == projected_point_accepts_input
            ? static_cast<projected_surface *>(buffer->node.data)
            : nullptr;
    if (!scaling && !projected) {
        return;
    }
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(buffer);
    if (!scene_surface) {
        return;
    }
    struct wlr_surface *surface = scene_surface->surface;
    const int width = surface->current.width, height = surface->current.height;
    struct seaside_scene_rect viewport = {clip.x, clip.y, clip.width, clip.height};
    struct wlr_box visible = !wlr_box_empty(&clip)
                                 ? intersection(viewport, x, y, width, height)
                                 : wlr_box{clamp_coordinate(x), clamp_coordinate(y), width, height};
    if (scaling) {
        projected = get_projected_surface(view, scene_surface);
        if (!projected) {
            return;
        }
    } else {
        destroy_projected_surface(projected);
    }
    if (wlr_box_empty(&visible)) {
        if (scaling) {
            projected->width = 0;
            projected->height = 0;
        }
        wlr_scene_node_set_enabled(&buffer->node, false);
        return;
    }

    const struct wlr_box pixels = {clamp_coordinate((int64_t)visible.x - x),
                                   clamp_coordinate((int64_t)visible.y - y), visible.width,
                                   visible.height};
    wlr_fbox crop{static_cast<double>(pixels.x), static_cast<double>(pixels.y),
                  static_cast<double>(pixels.width), static_cast<double>(pixels.height)};
    struct wlr_box destination = visible;
    pixman_region32_t opaque;
    pixman_region32_init(&opaque);
    if (scaling) {
        const struct window_projection mapping = view->projection;
        destination.x = clamp_rounded_coordinate(mapping.x + round(visible.x * mapping.scale_x));
        destination.y = clamp_rounded_coordinate(mapping.y + round(visible.y * mapping.scale_y));
        destination.width = clamp_rounded_coordinate(
            mapping.x + round(((double)visible.x + visible.width) * mapping.scale_x) -
            destination.x);
        destination.height = clamp_rounded_coordinate(
            mapping.y + round(((double)visible.y + visible.height) * mapping.scale_y) -
            destination.y);
        clip_projected_destination(&destination, &crop, destination_clip);
        projected->source = crop;
        projected->width = destination.width;
        projected->height = destination.height;
        /* Fractional projection can put a filtering footprint across an
         * opaque-region edge. Only mark a wholly opaque crop as opaque. */
        const pixman_box32_t limits = {clamp_rounded_coordinate(floor(crop.x)),
                                       clamp_rounded_coordinate(floor(crop.y)),
                                       clamp_rounded_coordinate(ceil(crop.x + crop.width)),
                                       clamp_rounded_coordinate(ceil(crop.y + crop.height))};
        if (pixman_region32_contains_rectangle(&surface->opaque_region, &limits) ==
                PIXMAN_REGION_IN &&
            !wlr_box_empty(&destination)) {
            pixman_region32_union_rect(&opaque, &opaque, 0, 0, destination.width,
                                       destination.height);
        }
    } else {
        pixman_region32_intersect_rect(&opaque, &surface->opaque_region, pixels.x, pixels.y,
                                       pixels.width, pixels.height);
        pixman_region32_translate(&opaque, -pixels.x, -pixels.y);
    }

    const bool visible_pixels = !wlr_box_empty(&destination);
    wlr_scene_node_set_enabled(&buffer->node, visible_pixels);
    if (visible_pixels) {
        const struct wlr_fbox source = cropped_source(surface, &crop);
        wlr_scene_buffer_set_source_box(buffer, &source);
        wlr_scene_buffer_set_opaque_region(buffer, &opaque);
        wlr_scene_buffer_set_dest_size(buffer, destination.width, destination.height);
        wlr_scene_node_set_position(&buffer->node, clamp_coordinate((int64_t)destination.x - x),
                                    clamp_coordinate((int64_t)destination.y - y));
    }
    pixman_region32_fini(&opaque);
}

static void project_surface_tree(struct scene_view *view, struct wlr_scene_node *node,
                                 struct wlr_box clip, const struct wlr_box *destination_clip,
                                 int64_t x, int64_t y) {
    if (node->type == WLR_SCENE_NODE_BUFFER) {
        project_buffer(view, wlr_scene_buffer_from_node(node), clip, destination_clip, x, y);
    } else if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
            project_surface_tree(view, child, clip, destination_clip, x + node->x, y + node->y);
        }
    }
}

static void project_tree(struct scene_view *view, struct wlr_scene_node *node, struct wlr_box clip,
                         const struct wlr_box *destination_clip) {
    int64_t x, y;
    source_node_position(view, &node->parent->node, &x, &y);
    project_surface_tree(view, node, clip, destination_clip, x, y);
}

static void project_view_surfaces(struct scene_view *view) {
    if (!is_projected(view) && wl_list_empty(&view->projected_surfaces)) {
        return;
    }
    const struct wlr_box *resize_clip =
        wlr_box_empty(&view->resize_clip) ? nullptr : &view->resize_clip;
    project_tree(view, &view->xdg_tree->node, view->source_clip, resize_clip);
    struct scene_popup *popup;
    wl_list_for_each(popup, &view->owner->popups, link) {
        if (popup->view == view) {
            project_tree(view, &popup->surface_tree->node, popup->source_clip, nullptr);
        }
    }
}

static void project_view_content(struct scene_view *view) {
    update_window_projection(view);
    clip_toplevel(view);
    clip_view_popups(view);
    project_view_surfaces(view);
}

static bool view_presented(const struct scene_view *view) {
    return view->placement.visible &&
           (!view->placement.tiled || has_viewport(view->placement.clip));
}

static uint64_t emit_window(struct scene_view *view, enum seaside_scene_window_event_kind kind,
                            bool enabled, bool ordinary_commit) {
    struct seaside_scene_bridge *scene = view->owner;
    if (!scene->window || (!view->id && kind != SEASIDE_WINDOW_CREATED)) {
        return 0;
    }
    struct scene_view *parent = toplevel_view(scene, view->toplevel->parent);
    struct wlr_box box = view->toplevel->base->geometry;
    const struct seaside_scene_window_event event = {
        .kind = kind,
        .id = view->id,
        .parent = parent ? parent->id : 0,
        .geometry = {box.x, box.y, box.width, box.height},
        .enabled = enabled,
        .ordinary_commit = ordinary_commit,
        .seat_client = nullptr,
        .serial = kind == SEASIDE_WINDOW_MAPPED || kind == SEASIDE_WINDOW_COMMITTED
                      ? view->toplevel->base->current.configure_serial
                      : 0,
        .resize_edges = 0,
    };
    return scene->window(scene->window_data, &event);
}

static void end_focus(struct seaside_scene_bridge *scene, bool deactivate) {
    struct scene_view *view = scene->focused_view;
    if (!view) {
        return;
    }
    scene->focused_view = nullptr;
    if (deactivate && view->toplevel->base->initialized) {
        wlr_xdg_toplevel_set_activated(view->toplevel, false);
    }
    scene->focus(scene->focus_data, nullptr);
}

static bool capture_window_cursor(void *data, double pixel_scale_x, double pixel_scale_y,
                                  struct wlr_render_texture_options *texture) noexcept {
    struct scene_view *view = static_cast<scene_view *>(data);
    struct seaside_scene_bridge *scene = view->owner;
    struct seaside_scene_target hit = {};
    if (!scene->cursor_known || !view->placement.visible ||
        !seaside_scene_window_at(scene, scene->cursor_x, scene->cursor_y, &hit) ||
        hit.id != view->id) {
        return false;
    }

    struct scene_output *viewport;
    wl_list_for_each(viewport, &scene->outputs, link) {
        const struct wlr_output *output = viewport->viewport->output;
        const struct wlr_output_cursor *cursor = output->hardware_cursor;
        const double local_x = scene->cursor_x - viewport->viewport->x;
        const double local_y = scene->cursor_y - viewport->viewport->y;
        if (!cursor || !cursor->enabled || !cursor->visible || !cursor->texture || local_x < 0 ||
            local_y < 0 || local_x >= viewport->width || local_y >= viewport->height) {
            continue;
        }
        double x = scene->cursor_x - (double)cursor->hotspot_x / output->scale;
        double y = scene->cursor_y - (double)cursor->hotspot_y / output->scale;
        view_source_position(view, &x, &y);
        const double scale_x = output->scale * (is_projected(view) ? view->projection.scale_x : 1);
        const double scale_y = output->scale * (is_projected(view) ? view->projection.scale_y : 1);
        *texture = {};
        texture->texture = cursor->texture;
        texture->src_box = cursor->src_box;
        texture->dst_box = {
            clamp_rounded_coordinate(floor(x * pixel_scale_x)),
            clamp_rounded_coordinate(floor(y * pixel_scale_y)),
            clamp_rounded_coordinate(ceil(cursor->width * pixel_scale_x / scale_x)),
            clamp_rounded_coordinate(ceil(cursor->height * pixel_scale_y / scale_y))};
        texture->transform = wlr_output_transform_invert(cursor->transform);
        return true;
    }
    return false;
}

static void consider_focus(struct scene_view *view) {
    struct seaside_scene_bridge *scene = view->owner;
    if (scene->watched_pid == 0 || scene->focused_view) {
        return;
    }
    pid_t pid = 0;
    wl_client_get_credentials(wl_resource_get_client(view->toplevel->resource), &pid, nullptr,
                              nullptr);
    if (pid != scene->watched_pid) {
        return;
    }
    scene->focused_view = view;
    wlr_xdg_toplevel_set_activated(view->toplevel, true);
    scene->focus(scene->focus_data, view->toplevel->base->surface);
}

static bool configure_applied(const struct scene_view *view, uint32_t requested) {
    const uint32_t serial = view->toplevel->base->current.configure_serial;
    return !requested || (serial && (uint32_t)(serial - requested) < UINT32_C(0x80000000));
}

static bool ordinary_buffer(const struct scene_view *view) {
    const struct wlr_xdg_toplevel_state *state = &view->toplevel->current;
    return !state->maximized && !state->fullscreen && !state->tiled &&
           configure_applied(view, view->restore_serial);
}

static void request_placed_size(struct scene_view *view, bool managed, bool restoring) {
    const struct seaside_scene_window_placement *placement = &view->placement;
    const struct seaside_scene_rect box = placement->geometry;
    struct wlr_xdg_toplevel *toplevel = view->toplevel;
    /* A client that mapped straight into a managed state may have no saved
     * floating size. Zero lets that client choose its new size. */
    const bool unconstrained = restoring && placement->unconstrained_restore;
    const int width = !unconstrained && box.width > 0 ? box.width : 0;
    const int height = !unconstrained && box.height > 0 ? box.height : 0;
    if (width == toplevel->scheduled.width && height == toplevel->scheduled.height && !restoring) {
        return;
    }
    if (placement->interactive_resize && !configure_applied(view, view->size_configure_serial)) {
        return;
    }
    view->size_configure_serial = wlr_xdg_toplevel_set_size(toplevel, width, height);
    if (!managed) {
        view->restore_serial = view->size_configure_serial;
    }
}

static void view_map(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, map);
    if (!view->mapped) {
        view->mapped = true;
        view->owner->snapshot.mapped_toplevels++;
        view->owner->snapshot.map_events++;
    }
    if (view->id) {
        emit_window(view, SEASIDE_WINDOW_MAPPED, false, ordinary_buffer(view));
    } else {
        wlr_scene_node_set_enabled(&view->tree->node, true);
        consider_focus(view);
    }
    if (view->owner->capture && !view->capture) {
        view->capture = seaside_window_capture_map(view->owner->capture, view->toplevel,
                                                   view->id ? view_presented(view)
                                                            : !wl_list_empty(&view->owner->outputs),
                                                   capture_window_cursor, view);
    }
}

static void place_popup_tree(struct scene_view *view) {
    if (!view->popup_tree) {
        return;
    }
    struct wlr_scene_node *node = &view->popup_tree->node;
    const bool active = view->mapped && view->placement.visible && view->placement.activated;
    // Menus of the active view can cross a neighboring tile. Keep their local
    // coordinate system and leave the toplevel and other windows in place.
    wlr_scene_node_reparent(node, active ? view->owner->windows : view->tree);
    wlr_scene_node_set_position(node, active ? view->tree->node.x : 0,
                                active ? view->tree->node.y : 0);
}

static void view_unmap(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, unmap);
    seaside_scene_blur_destroy(&view->blur);
    seaside_window_capture_unmap(view->capture);
    view->capture = nullptr;
    if (view->mapped) {
        view->mapped = false;
        view->owner->snapshot.mapped_toplevels--;
        view->owner->snapshot.unmap_events++;
    }
    wlr_scene_node_set_enabled(&view->tree->node, false);
    view->placement = seaside_scene_window_placement{};
    place_popup_tree(view);
    project_view_content(view);
    view->restore_serial = 0;
    view->size_configure_serial = 0;
    emit_window(view, SEASIDE_WINDOW_UNMAPPED, false, false);
    if (view->owner->focused_view == view) {
        // Unmap resets the XDG configure sequence. The next initial commit
        // will establish inactive state before another map can take focus.
        end_focus(view->owner, false);
    }
}

static void configure_decoration(struct scene_view *view) {
    struct wlr_xdg_toplevel_decoration_v1 *decoration = view->decoration;
    if (!decoration || !view->toplevel->base->initialized) {
        return;
    }
    const enum wlr_xdg_toplevel_decoration_v1_mode mode =
        view->owner->titlebars ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                               : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    // Wait for the previous mode to be acknowledged before reversing it. wlroots
    // omits a mode from a configure if it matches the last acknowledged mode.
    if (decoration->scheduled_mode != mode &&
        (view->toplevel->base->initial_commit ||
         decoration->scheduled_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_NONE ||
         decoration->pending.mode == decoration->scheduled_mode)) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, mode);
    }
}

static void request_decoration(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_decoration);
    configure_decoration(view);
}

static void detach_decoration(struct scene_view *view) {
    if (view->decoration) {
        wl_list_remove(&view->request_decoration.link);
        wl_list_remove(&view->decoration_destroy.link);
        view->decoration = nullptr;
    }
}

static void decoration_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, decoration_destroy);
    detach_decoration(view);
}

static void new_decoration(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    struct wlr_xdg_toplevel_decoration_v1 *decoration =
        static_cast<wlr_xdg_toplevel_decoration_v1 *>(data);
    struct scene_view *view = toplevel_view(scene, decoration->toplevel);
    if (!view) {
        return;
    }
    view->decoration = decoration;
    view->request_decoration.notify = request_decoration;
    view->decoration_destroy.notify = decoration_destroy;
    wl_signal_add(&decoration->events.request_mode, &view->request_decoration);
    wl_signal_add(&decoration->events.destroy, &view->decoration_destroy);
    configure_decoration(view);
}

enum { window_icon_size = 64 };

static bool copy_icon_pixels(struct wlr_buffer *buffer, int size, uint8_t *rgba) {
    void *pixels;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format,
                                          &stride)) {
        return false;
    }
    const bool alpha = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888;
    const bool swapped = format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
    const bool supported =
        (alpha || format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888) &&
        stride / sizeof(uint32_t) >= (size_t)buffer->width;
    if (supported) {
        for (int y = 0; y < size; ++y) {
            const size_t source_y = (size_t)y * (size_t)buffer->height / (size_t)size;
            const uint8_t *row = (const uint8_t *)pixels + source_y * stride;
            for (int x = 0; x < size; ++x) {
                const size_t source_x = (size_t)x * (size_t)buffer->width / (size_t)size;
                uint32_t pixel;
                memcpy(&pixel, row + source_x * sizeof(pixel), sizeof(pixel));
                const unsigned a = alpha ? pixel >> 24 : 255;
                const unsigned channels[] = {
                    (pixel >> (swapped ? 0 : 16)) & 255,
                    (pixel >> 8) & 255,
                    (pixel >> (swapped ? 16 : 0)) & 255,
                };
                for (size_t channel = 0; channel < 3; ++channel) {
                    // Wayland SHM pixels use premultiplied alpha. PNG stores straight alpha.
                    const unsigned value = a ? (channels[channel] * 255 + a / 2) / a : 0;
                    *rgba++ = value < 255 ? value : 255;
                }
                *rgba++ = a;
            }
        }
    }
    wlr_buffer_end_data_ptr_access(buffer);
    return supported;
}

static std::string encode_icon(const struct wlr_xdg_toplevel_icon_v1 *icon) {
    struct wlr_xdg_toplevel_icon_v1_buffer *entry;
    uint8_t pixels[window_icon_size * window_icon_size * 4];
    int size = 0;
    wl_list_for_each(entry, &icon->buffers, link) {
        const int width = entry->buffer->width;
        const int candidate = width < window_icon_size ? width : window_icon_size;
        if (candidate > size && copy_icon_pixels(entry->buffer, candidate, pixels)) {
            size = candidate;
        }
    }
    if (!size) {
        return {};
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(size);
    image.height = static_cast<png_uint_32>(size);
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t length = 0;
    std::unique_ptr<char, decltype(&g_free)> encoded{nullptr, g_free};
    if (png_image_write_to_memory(&image, nullptr, &length, 0, pixels, 0, nullptr)) {
        void *png = malloc(length);
        if (png && png_image_write_to_memory(&image, png, &length, 0, pixels, 0, nullptr)) {
            encoded.reset(g_base64_encode(static_cast<const guchar *>(png), length));
        }
        free(png);
    }
    png_image_free(&image);
    return encoded ? std::string{encoded.get()} : std::string{};
}

static void apply_icon(struct scene_view *view) noexcept {
    if (!view->icon_dirty) {
        return;
    }
    view->icon_dirty = false;
    view->icon_name.clear();
    view->icon_png_base64.clear();
    struct wlr_xdg_toplevel_icon_v1 *icon = view->pending_icon;
    if (icon) {
        try {
            if (icon->name && *icon->name && !strchr(icon->name, '/') &&
                strcmp(icon->name, ".") != 0 && strcmp(icon->name, "..") != 0 &&
                strlen(icon->name) <= 255) {
                view->icon_name = icon->name;
            }
            view->icon_png_base64 = encode_icon(icon);
        } catch (...) {
            fputs("seaside: could not prepare window icon\n", stderr);
        }
        wlr_xdg_toplevel_icon_v1_unref(icon);
        view->pending_icon = nullptr;
    }
}

static void set_icon(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    const struct wlr_xdg_toplevel_icon_manager_v1_set_icon_event *event =
        static_cast<const wlr_xdg_toplevel_icon_manager_v1_set_icon_event *>(data);
    struct scene_view *view = toplevel_view(scene, event->toplevel);
    if (view) {
        if (view->pending_icon) {
            wlr_xdg_toplevel_icon_v1_unref(view->pending_icon);
        }
        view->pending_icon = event->icon ? wlr_xdg_toplevel_icon_v1_ref(event->icon) : nullptr;
        view->icon_dirty = true;
    }
}

static void remove_icon_manager_watch(struct seaside_scene_bridge *scene) {
    wl_list_remove(&scene->set_icon.listener.link);
    wl_list_init(&scene->set_icon.listener.link);
    wl_list_remove(&scene->icon_manager_destroy.listener.link);
    wl_list_init(&scene->icon_manager_destroy.listener.link);
}

static void icon_manager_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    auto *scene = SceneListener::owner(listener);
    remove_icon_manager_watch(scene);
}

static void view_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, commit);
    apply_icon(view);
    configure_decoration(view);
    if (view->toplevel->base->initial_commit) {
        uint32_t capabilities = view->id ? WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
                                               WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN
                                         : 0;
        wlr_xdg_toplevel_set_wm_capabilities(view->toplevel, capabilities);
        view->size_configure_serial = wlr_xdg_toplevel_set_size(view->toplevel, 0, 0);
        wlr_xdg_toplevel_set_activated(view->toplevel, false);
        emit_window(view, SEASIDE_WINDOW_INITIAL_COMMIT, false, false);
        view->owner->snapshot.initial_configures++;
    }
    if (wlr_surface_has_buffer(view->toplevel->base->surface)) {
        view->owner->snapshot.buffer_commits++;
        /* The client's last tiled or special size may arrive after a request
         * to restore its ordinary floating geometry. */
        const bool ordinary = ordinary_buffer(view);
        if (ordinary) {
            view->restore_serial = 0;
        }
        if (view->placement.fullscreen || view->placement.fit_resize_buffer) {
            project_view_content(view);
        } else {
            clip_toplevel(view);
        }
        if (view->placement.interactive_resize && has_viewport(view->placement.geometry)) {
            request_placed_size(view, view->placement.tiled, false);
        }
        emit_window(view, SEASIDE_WINDOW_COMMITTED, false, ordinary);
    }
}

static void view_request_maximize(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_maximize);
    emit_window(view, SEASIDE_WINDOW_MAXIMIZE_REQUESTED, view->toplevel->requested.maximized,
                false);
    /* XDG requires a configure even when the requested state stays the same. */
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, request_fullscreen);
    emit_window(view, SEASIDE_WINDOW_FULLSCREEN_REQUESTED, view->toplevel->requested.fullscreen,
                false);
    if (view->toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(view->toplevel->base);
    }
}

static void view_request_gesture(struct scene_view *view, enum seaside_scene_window_event_kind kind,
                                 struct wlr_seat_client *seat, uint32_t serial, uint32_t edges) {
    if (view->mapped && view->id && view->owner->window) {
        const struct seaside_scene_window_event event = {
            .kind = kind,
            .id = view->id,
            .parent = 0,
            .geometry = {},
            .enabled = false,
            .ordinary_commit = false,
            .seat_client = seat,
            .serial = serial,
            .resize_edges = edges,
        };
        view->owner->window(view->owner->window_data, &event);
    }
}

static void view_request_move(struct wl_listener *listener, void *data) noexcept {
    struct scene_view *view = wl_container_of(listener, view, request_move);
    const struct wlr_xdg_toplevel_move_event *event =
        static_cast<const wlr_xdg_toplevel_move_event *>(data);
    view_request_gesture(view, SEASIDE_WINDOW_MOVE_REQUESTED, event->seat, event->serial, 0);
}

static void view_request_resize(struct wl_listener *listener, void *data) noexcept {
    struct scene_view *view = wl_container_of(listener, view, request_resize);
    const struct wlr_xdg_toplevel_resize_event *event =
        static_cast<const wlr_xdg_toplevel_resize_event *>(data);
    view_request_gesture(view, SEASIDE_WINDOW_RESIZE_REQUESTED, event->seat, event->serial,
                         event->edges);
}

static void view_set_parent(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, set_parent);
    emit_window(view, SEASIDE_WINDOW_PARENT_CHANGED, false, false);
}

static void destroy_view(struct scene_view *view) {
    seaside_scene_blur_destroy(&view->blur);
    if (view->pending_icon) {
        wlr_xdg_toplevel_icon_v1_unref(view->pending_icon);
    }
    detach_decoration(view);
    seaside_window_capture_unmap(view->capture);
    if (view->owner->focused_view == view) {
        end_focus(view->owner, false);
    }
    if (view->mapped) {
        view->owner->snapshot.mapped_toplevels--;
    }
    view->owner->snapshot.live_toplevels--;
    view->owner->snapshot.destroyed_toplevels++;
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->request_maximize.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->set_parent.link);
    wl_list_remove(&view->link);
    if (view->id) {
        view->owner->views_by_id.erase(view->id);
    }
    wlr_scene_node_set_enabled(&view->tree->node, false);
    if (view->popup_tree) {
        wlr_scene_node_reparent(&view->popup_tree->node, view->tree);
    }
    emit_window(view, SEASIDE_WINDOW_DESTROYED, false, false);
    wlr_scene_node_destroy(&view->tree->node);
    delete view;
}

static void view_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_view *view = wl_container_of(listener, view, destroy);
    destroy_view(view);
}

static void new_toplevel(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    struct wlr_xdg_toplevel *toplevel = static_cast<wlr_xdg_toplevel *>(data);
    auto *view = new (std::nothrow) scene_view{};
    if (!view) {
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
        return;
    }
    view->owner = scene;
    view->toplevel = toplevel;
    wl_list_init(&view->projected_surfaces);
    view->tree = wlr_scene_tree_create(scene->windows);
    if (view->tree) {
        view->xdg_tree = wlr_scene_xdg_surface_create(view->tree, toplevel->base);
    }
    if (!view->tree || !view->xdg_tree) {
        if (view->tree) {
            wlr_scene_node_destroy(&view->tree->node);
        }
        delete view;
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
        return;
    }
    view->tree->node.data = view;
    wlr_scene_node_set_enabled(&view->tree->node, false);
    view->commit.notify = view_commit;
    view->map.notify = view_map;
    view->unmap.notify = view_unmap;
    view->destroy.notify = view_destroy;
    view->request_maximize.notify = view_request_maximize;
    view->request_fullscreen.notify = view_request_fullscreen;
    view->request_move.notify = view_request_move;
    view->request_resize.notify = view_request_resize;
    view->set_parent.notify = view_set_parent;
    wl_signal_add(&toplevel->base->surface->events.commit, &view->commit);
    wl_signal_add(&toplevel->base->surface->events.map, &view->map);
    wl_signal_add(&toplevel->base->surface->events.unmap, &view->unmap);
    wl_signal_add(&toplevel->events.destroy, &view->destroy);
    wl_signal_add(&toplevel->events.request_maximize, &view->request_maximize);
    wl_signal_add(&toplevel->events.request_fullscreen, &view->request_fullscreen);
    wl_signal_add(&toplevel->events.request_move, &view->request_move);
    wl_signal_add(&toplevel->events.request_resize, &view->request_resize);
    wl_signal_add(&toplevel->events.set_parent, &view->set_parent);
    wl_list_insert(&scene->views, &view->link);
    view->id = emit_window(view, SEASIDE_WINDOW_CREATED, false, false);
    bool indexed = false;
    if (view->id) {
        try {
            scene->views_by_id.insert_or_assign(view->id, view);
            indexed = true;
        } catch (...) {
            // The registered surface still owns its listeners until its destroy event.
        }
    }
    if (!indexed && scene->window) {
        scene->snapshot.failed = true;
        wl_client_post_no_memory(wl_resource_get_client(toplevel->resource));
    }
    scene->snapshot.live_toplevels++;
}

static struct wlr_box view_source_viewport(const struct scene_view *view,
                                           struct seaside_scene_rect viewport) {
    double left = viewport.x, top = viewport.y;
    double right = (double)viewport.x + viewport.width,
           bottom = (double)viewport.y + viewport.height;
    view_source_position(view, &left, &top);
    view_source_position(view, &right, &bottom);
    /* Round towards the viewport so scaling an attached popup cannot put it
     * on a neighboring output. */
    return wlr_box{clamp_rounded_coordinate(ceil(left)), clamp_rounded_coordinate(ceil(top)),
                   clamp_rounded_coordinate(floor(right) - ceil(left)),
                   clamp_rounded_coordinate(floor(bottom) - ceil(top))};
}

static void unconstrain_popup(struct scene_popup *popup) {
    const struct scene_view *view = popup->view;
    struct wlr_box bounds = view_source_viewport(view, view->placement.output);
    if (!wlr_box_empty(&bounds)) {
        bounds.x = clamp_coordinate((int64_t)bounds.x + view->toplevel->base->geometry.x);
        bounds.y = clamp_coordinate((int64_t)bounds.y + view->toplevel->base->geometry.y);
        wlr_xdg_popup_unconstrain_from_box(popup->popup, &bounds);
    }
}

static void clip_popup(struct scene_popup *popup) {
    struct scene_view *view = popup->view;
    const struct seaside_scene_rect viewport =
        view->placement.fullscreen ? view->placement.output : view->placement.clip;
    struct wlr_scene_node *node = &popup->surface_tree->node;
    popup->source_clip = view_source_viewport(view, viewport);
    if (is_projected(view) || !has_viewport(viewport)) {
        wlr_scene_subsurface_tree_set_clip(node, nullptr);
        return;
    }
    int64_t x, y;
    source_node_position(view, node, &x, &y);
    const struct wlr_box clip = {clamp_coordinate((int64_t)popup->source_clip.x - x),
                                 clamp_coordinate((int64_t)popup->source_clip.y - y),
                                 popup->source_clip.width, popup->source_clip.height};
    wlr_scene_subsurface_tree_set_clip(node, &clip);
}

static void clip_view_popups(struct scene_view *view) {
    struct scene_popup *popup;
    wl_list_for_each(popup, &view->owner->popups, link) {
        if (popup->view == view) {
            clip_popup(popup);
        }
    }
}

static void popup_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->popup->base->initial_commit) {
        unconstrain_popup(popup);
        wlr_xdg_surface_schedule_configure(popup->popup->base);
    }
    const struct wlr_box position = popup->popup->current.geometry;
    const struct wlr_box geometry = popup->popup->base->geometry;
    wlr_scene_node_set_position(&popup->tree->node, position.x, position.y);
    wlr_scene_node_set_position(&popup->surface_tree->node, clamp_coordinate(-(int64_t)geometry.x),
                                clamp_coordinate(-(int64_t)geometry.y));
    clip_view_popups(popup->view);
    project_view_surfaces(popup->view);
}

static void popup_reposition(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, reposition);
    unconstrain_popup(popup);
}

static void popup_tree_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, tree_destroy);
    seaside_popup_focus_finish(&popup->focus);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->tree_destroy.link);
    wl_list_remove(&popup->link);
    free(popup);
}

static void popup_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_popup *popup = wl_container_of(listener, popup, destroy);
    wlr_scene_node_destroy(&popup->tree->node);
}

static void new_popup(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    struct wlr_xdg_popup *xdg = static_cast<wlr_xdg_popup *>(data);
    if (!xdg->parent) {
        // A layer-shell popup receives its parent via zwlr_layer_surface.get_popup.
        return;
    }
    if (seaside_layer_scene_add_popup(scene->layers, xdg)) {
        return;
    }
    if (!scene->window) {
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    struct scene_view *view =
        toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(xdg->parent));
    /* Keep ancestor movement in the scene hierarchy while giving every XDG
     * surface its own local viewport clip. A popup can extend beyond its tile. */
    if (view && !view->popup_tree) {
        view->popup_tree = wlr_scene_tree_create(view->tree);
        if (!view->popup_tree) {
            wl_client_post_no_memory(wl_resource_get_client(xdg->resource));
            wlr_xdg_popup_destroy(xdg);
            return;
        }
        view->popup_tree->node.data = view;
        place_popup_tree(view);
    }
    struct wlr_scene_tree *parent_tree = view ? view->popup_tree : nullptr;
    if (!parent_tree) {
        struct scene_popup *parent = surface_popup(scene, xdg->parent);
        if (parent) {
            view = parent->view;
            parent_tree = parent->tree;
        }
    }
    if (!parent_tree) {
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    struct scene_popup *popup = static_cast<scene_popup *>(calloc(1, sizeof(*popup)));
    if (popup) {
        popup->tree = wlr_scene_tree_create(parent_tree);
        if (popup->tree) {
            popup->surface_tree = wlr_scene_subsurface_tree_create(popup->tree, xdg->base->surface);
        }
    }
    if (!popup || !popup->surface_tree) {
        if (popup && popup->tree) {
            wlr_scene_node_destroy(&popup->tree->node);
        }
        free(popup);
        wl_client_post_no_memory(wl_resource_get_client(xdg->resource));
        wlr_xdg_popup_destroy(xdg);
        return;
    }
    popup->view = view;
    popup->popup = xdg;
    seaside_popup_focus_init(&popup->focus, xdg);
    popup->commit.notify = popup_commit;
    popup->reposition.notify = popup_reposition;
    popup->destroy.notify = popup_destroy;
    popup->tree_destroy.notify = popup_tree_destroy;
    wl_list_insert(&scene->popups, &popup->link);
    wl_signal_add(&xdg->base->surface->events.commit, &popup->commit);
    wl_signal_add(&xdg->events.reposition, &popup->reposition);
    wl_signal_add(&xdg->events.destroy, &popup->destroy);
    wl_signal_add(&popup->tree->node.events.destroy, &popup->tree_destroy);
}

static struct scene_output *find_output(const struct seaside_scene_bridge *scene,
                                        const struct wlr_output *output) {
    struct scene_output *view;
    wl_list_for_each(view, &scene->outputs, link) {
        if (view->viewport->output == output) {
            return view;
        }
    }
    return nullptr;
}

static void fit_wallpaper(struct scene_output *view) {
    const struct wlr_buffer *image = view->owner->wallpaper;
    if (!image || view->width <= 0 || view->height <= 0) {
        wlr_scene_node_set_enabled(&view->wallpaper->node, false);
        return;
    }
    const double output_ratio = (double)view->width / view->height;
    wlr_fbox crop{0, 0, static_cast<double>(image->width), static_cast<double>(image->height)};
    if (crop.width / crop.height > output_ratio) {
        crop.width = crop.height * output_ratio;
        crop.x = (image->width - crop.width) / 2;
    } else {
        crop.height = crop.width / output_ratio;
        crop.y = (image->height - crop.height) / 2;
    }
    wlr_scene_buffer_set_source_box(view->wallpaper, &crop);
    wlr_scene_buffer_set_dest_size(view->wallpaper, view->width, view->height);
    wlr_scene_node_set_enabled(&view->wallpaper->node, true);
}

static void fit_banner(struct scene_output *view, bool project_now) {
    int height = 0;
    if (view->banner_logical_height > 0 && view->width > 0 && view->height > 1) {
        height = view->banner_logical_height < view->height ? view->banner_logical_height
                                                            : view->height - 1;
    }
    const bool visible = height > 0 && !view->banner_fullscreen;
    wlr_scene_node_set_enabled(&view->banner->node, visible);
    if (visible) {
        const struct wlr_fbox crop = {
            .x = 0,
            .y = 0,
            .width = static_cast<double>(view->banner_width),
            .height = (double)view->banner_height * height / view->banner_logical_height,
        };
        wlr_scene_buffer_set_source_box(view->banner, &crop);
        wlr_scene_buffer_set_dest_size(view->banner, view->width, height);
    }
    seaside_layer_scene_reserve_top(view->owner->layers, view->viewport->output, height,
                                    project_now);
}

static void update_output_appearance(struct scene_output *view) {
    struct wlr_output *output = view->viewport->output;
    int width = 0, height = 0;
    if (output->enabled) {
        wlr_output_effective_resolution(output, &width, &height);
        seaside_layer_scene_update_output(
            view->owner->layers, output,
            seaside_scene_rect{view->viewport->x, view->viewport->y, width, height});
    }
    const bool reraster = view->width != width || view->scale != output->scale;
    view->width = width;
    view->height = height;
    view->scale = output->scale;
    wlr_scene_node_set_position(&view->wallpaper->node, view->viewport->x, view->viewport->y);
    wlr_scene_node_set_position(&view->banner->node, view->viewport->x, view->viewport->y);
    fit_wallpaper(view);
    if (reraster) {
        // The old banner size belongs to another viewport. Drop it before
        // moving or stretching it into the new geometry.
        wlr_scene_buffer_set_buffer(view->banner, nullptr);
        view->banner_logical_height = 0;
        if (view->owner->appearance) {
            view->owner->appearance->viewport_changed(output, width, output->scale);
        }
    }
    fit_banner(view, false);
}

static void clear_output_blur(struct scene_output *output) {
    if (output->blur_active) {
        struct scene_view *view;
        wl_list_for_each(view, &output->owner->views, link) {
            seaside_scene_blur_remove_output(view->blur, output->viewport->output);
        }
        output->blur_active = false;
    }
}

static void output_commit(struct wl_listener *listener, void *data) noexcept {
    struct scene_output *view = wl_container_of(listener, view, commit);
    const struct wlr_output_event_commit *event =
        static_cast<const wlr_output_event_commit *>(data);
    if (event->state->committed &
        (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED |
         WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_SCALE)) {
        view->tearing_window = 0;
    }
    if (event->state->committed & (WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE |
                                   WLR_OUTPUT_STATE_SCALE | WLR_OUTPUT_STATE_TRANSFORM)) {
        clear_output_blur(view);
        update_output_appearance(view);
        if (view->owner->night_light || view->render_status.night_light_applied) {
            view->lut_request = LutRequest::pending;
            wlr_output_schedule_frame(view->viewport->output);
        }
    }
}

static void output_present(struct wl_listener *listener, void *data) noexcept {
    struct scene_output *view = wl_container_of(listener, view, present);
    const struct wlr_output_event_present *event =
        static_cast<const wlr_output_event_present *>(data);
    if (!view->pending_mode || event->commit_seq != view->pending_commit) {
        return;
    }
    if (event->presented) {
        view->render_status.presentation_mode = view->pending_mode;
        view->render_status.fallback_reason = view->pending_fallback;
        view->render_status.tearing_applied =
            view->pending_tearing && !(event->flags & WLR_OUTPUT_PRESENT_VSYNC);
        if (view->unsubmitted_scanout) {
            // A replacement flip has retired earlier scanout buffers. Only
            // now can the point for an unsubmitted buffer be signalled safely.
            wlr_drm_syncobj_timeline_signal(view->unsubmitted_scanout, view->unsubmitted_point);
            wlr_drm_syncobj_timeline_unref(view->unsubmitted_scanout);
            view->unsubmitted_scanout = nullptr;
        }
    }
    if (view->owner->frame_observer) {
        const seaside_scene_frame_observation observation{
            .event = "present",
            .presentation_mode = view->pending_mode,
            .commit_seq = event->commit_seq,
            .succeeded = event->presented,
            .tearing = event->presented && view->render_status.tearing_applied,
            .presentation_ns =
                event->presented && event->when.tv_sec >= 0 && event->when.tv_nsec >= 0
                    ? (uint64_t)event->when.tv_sec * 1000000000 + (uint64_t)event->when.tv_nsec
                    : 0,
        };
        view->owner->frame_observer(view->owner->frame_observer_data, view->viewport->output,
                                    &observation);
    }
    view->pending_mode = nullptr;
}

static enum seaside_output_support tearing_support(struct wlr_output *output) {
    if (!wlr_output_is_drm(output)) {
        return SEASIDE_OUTPUT_UNSUPPORTED;
    }
    int fd = wlr_backend_get_drm_fd(output->backend);
    uint64_t atomic = 0, legacy = 0;
    bool known_atomic = drmGetCap(fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &atomic) == 0;
    bool known_legacy = drmGetCap(fd, DRM_CAP_ASYNC_PAGE_FLIP, &legacy) == 0;
    if (atomic || legacy) {
        return SEASIDE_OUTPUT_SUPPORTED;
    }
    return known_atomic && known_legacy ? SEASIDE_OUTPUT_UNSUPPORTED : SEASIDE_OUTPUT_UNKNOWN;
}

static uint64_t tearing_candidate(const struct scene_output *output) {
    const struct seaside_scene_bridge *scene = output->owner;
    if (!scene->allow_tearing || output->lut_request != LutRequest::clean ||
        output->render_status.tearing == SEASIDE_OUTPUT_UNSUPPORTED ||
        output->banner->node.enabled || !wl_list_empty(&scene->drag_icons->children) ||
        seaside_layer_scene_has_overlay(scene->layers, output->viewport->output) ||
        output->viewport->output->attach_render_locks > (scene->direct_scanout ? 0 : 1)) {
        return 0;
    }
    const struct scene_view *top = nullptr, *view;
    wl_list_for_each(view, &scene->views, link) {
        if (view->mapped && view->placement.visible &&
            view->placement.output.x == output->viewport->x &&
            view->placement.output.y == output->viewport->y &&
            (!top || view->placement.stacking > top->placement.stacking)) {
            top = view;
        }
    }
    if (top && top->placement.fullscreen &&
        wlr_tearing_control_manager_v1_surface_hint_from_surface(scene->tearing,
                                                                 top->toplevel->base->surface) ==
            WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC) {
        return top->id;
    }
    return 0;
}

static struct wlr_color_transform *night_light_transform(size_t size, int kelvin) {
    // Approximate black-body RGB gains relative to the compositor's neutral
    // 6500 K white, using representative wavelengths and SDR encoding.
    const double wavelengths[] = {610e-9, 550e-9, 460e-9};
    double gains[3], peak = 0;
    for (size_t channel = 0; channel < 3; ++channel) {
        const double c2 = 0.01438776877 / wavelengths[channel];
        gains[channel] = expm1(c2 / 6500) / expm1(c2 / kelvin);
        peak = fmax(peak, gains[channel]);
    }
    uint16_t *ramp = static_cast<uint16_t *>(malloc(3 * size * sizeof(*ramp)));
    if (!ramp) {
        return nullptr;
    }
    for (size_t channel = 0; channel < 3; ++channel) {
        const double gain = pow(gains[channel] / peak, 1.0 / 2.2);
        for (size_t i = 0; i < size; ++i) {
            ramp[channel * size + i] =
                (uint16_t)lround(gain * UINT16_MAX * (double)i / (double)(size - 1));
        }
    }
    struct wlr_color_transform *transform =
        wlr_color_transform_init_lut_3x1d(size, ramp, ramp + size, ramp + 2 * size);
    free(ramp);
    return transform;
}

static bool prepare_night_light(struct scene_output *view, struct wlr_output_state *state) {
    if (view->lut_request == LutRequest::clean) {
        return false;
    }
    view->lut_request = LutRequest::clean;
    const struct seaside_scene_bridge *scene = view->owner;
    const size_t size = wlr_output_get_gamma_size(view->viewport->output);
    if (size < 2 || size > UINT16_MAX + 1u) {
        view->render_status.night_light_reason = "no usable hardware gamma LUT";
        return false;
    }
    if (!scene->night_light && !view->render_status.night_light_applied) {
        view->render_status.night_light_reason = nullptr;
        return false;
    }
    struct wlr_color_transform *transform =
        scene->night_light ? night_light_transform(size, scene->temperature_kelvin) : nullptr;
    if (scene->night_light && !transform) {
        view->render_status.night_light_reason = "could not allocate hardware gamma LUT";
        return false;
    }
    // This state goes directly to KMS. The scene's color transform option
    // would render a GPU pass, which is outside the hardware-only policy.
    wlr_output_state_set_color_transform(state, transform);
    wlr_color_transform_unref(transform);
    return true;
}

static bool prepare_blur(struct scene_output *output) {
    struct seaside_scene_bridge *scene = output->owner;
    if (scene->blur_strength <= 0) {
        return false;
    }
    const struct wlr_box viewport = {output->viewport->x, output->viewport->y, output->width,
                                     output->height};
    bool applied = false;
    struct wlr_scene_node *node;
    // Render bottom to top so a stacked window samples lower blur layers.
    wl_list_for_each(node, &scene->windows->children, link) {
        struct scene_view *view = static_cast<scene_view *>(node->data);
        if (!view || node != &view->tree->node) {
            continue;
        }
        if ((view->id && !view->placement.visible) || view->placement.fullscreen || !view->mapped) {
            seaside_scene_blur_remove_output(view->blur, output->viewport->output);
            continue;
        }
        applied |= seaside_scene_blur_update(&view->blur, &scene->scene->tree, view->tree,
                                             view->xdg_tree, output->viewport->output, &viewport,
                                             scene->blur_strength, scene->blur_passes);
    }
    return applied;
}

static bool commit_frame(struct scene_output *view) {
    struct wlr_scene_output *scene_output = view->viewport;
    struct wlr_output *output = scene_output->output;
    if (!wlr_scene_output_needs_frame(scene_output) && view->lut_request == LutRequest::clean) {
        return true;
    }

    const bool blur = view->blur_active = prepare_blur(view);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    const bool retry_lut = view->lut_request == LutRequest::retry;
    const bool compose = view->retry_composed || retry_lut || blur;
    if (compose) {
        wlr_output_lock_attach_render(output, true);
    }
    bool built = wlr_scene_output_build_state(scene_output, &state, nullptr);
    if (compose) {
        wlr_output_lock_attach_render(output, false);
    }
    const bool scanout =
        built && state.buffer &&
        (!output->swapchain || !wlr_swapchain_has_buffer(output->swapchain, state.buffer));
    bool *tearing_rejected =
        scanout ? &view->scanout_tearing_rejected : &view->composed_tearing_rejected;
    bool async = false;
    if (built && state.buffer) {
        const uint64_t candidate = blur ? 0 : tearing_candidate(view);
        if (candidate != view->tearing_window) {
            view->tearing_window = candidate;
            view->scanout_tearing_rejected = view->composed_tearing_rejected = false;
        }
        async = candidate && !*tearing_rejected;
        view->render_status.tearing_reason =
            candidate && *tearing_rejected ? "backend rejected asynchronous presentation" : nullptr;
    }
    const bool lut = built && prepare_night_light(view, &state);
    state.tearing_page_flip = async;
    const bool committed = built && wlr_output_commit_state(output, &state);
    if (view->owner->frame_observer) {
        const seaside_scene_frame_observation observation{
            .event = "commit",
            .presentation_mode = state.buffer ? (scanout ? "scanout" : "composed") : nullptr,
            .commit_seq = committed ? output->commit_seq : 0,
            .succeeded = committed,
            .tearing = async,
            .presentation_ns = 0,
        };
        view->owner->frame_observer(view->owner->frame_observer_data, output, &observation);
    }
    if (!committed && scanout && state.signal_timeline) {
        wlr_drm_syncobj_timeline_unref(view->unsubmitted_scanout);
        view->unsubmitted_scanout = wlr_drm_syncobj_timeline_ref(state.signal_timeline);
        view->unsubmitted_point = state.signal_point;
    }
    if (committed) {
        if (lut) {
            view->render_status.night_light_reason = nullptr;
            view->render_status.night_light_applied = view->owner->night_light;
        }
        if (state.buffer) {
            view->pending_commit = output->commit_seq;
            view->pending_mode = scanout ? "scanout" : "composed";
            view->pending_tearing = async;
            view->pending_fallback = scanout                ? nullptr
                                     : view->retry_composed ? "scanout commit rejected"
                                     : blur                 ? "backdrop blur requires composition"
                                     : !view->owner->direct_scanout ? "direct scanout disabled"
                                     : !wlr_output_is_direct_scanout_allowed(output)
                                         ? "output requires composition"
                                         : "scene or buffer not eligible for scanout";
        }
        if (async) {
            view->render_status.tearing = SEASIDE_OUTPUT_SUPPORTED;
            view->render_status.tearing_reason = nullptr;
        }
        view->retry_composed = view->retry_frame = false;
    } else if (async) {
        view->render_status.tearing_reason = "backend rejected asynchronous presentation";
        // A successful synchronized fallback cannot establish async support.
        // Reconsider it on a new candidate, render path or output configuration.
        *tearing_rejected = true;
        wlr_output_schedule_frame(output);
    } else if (lut) {
        // A combined commit cannot distinguish a bad scanout buffer from a
        // rejected LUT. Try the same LUT once on a synchronized composed frame.
        view->retry_composed |= scanout;
        if (!retry_lut) {
            view->lut_request = LutRequest::retry;
        } else {
            view->render_status.night_light_reason = "backend rejected hardware gamma LUT";
            fprintf(stderr, "seaside: %s: %s\n", output->name,
                    view->render_status.night_light_reason);
        }
        wlr_output_schedule_frame(output);
    } else if (scanout) {
        view->retry_composed = true;
        wlr_output_schedule_frame(output);
    } else if (!view->retry_frame) {
        view->retry_frame = true;
        wlr_output_schedule_frame(output);
    }
    wlr_output_state_finish(&state);
    return committed;
}

static void output_frame(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_output *output = wl_container_of(listener, output, frame);
    if (output->owner->before_frame) {
        output->owner->before_frame(output->owner->before_frame_data, output->viewport->output);
    }
    update_input_popups(output->owner);
    if (!commit_frame(output)) {
        output->owner->snapshot.failed_frames++;
        return;
    }
    output->owner->snapshot.frames++;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        wlr_scene_output_send_frame_done(output->viewport, &now);
    } else {
        output->owner->snapshot.failed = true;
    }
}

static void output_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_output *output = wl_container_of(listener, output, destroy);
    struct seaside_scene_bridge *scene = output->owner;
    clear_output_blur(output);
    if (scene->layers) {
        seaside_layer_scene_remove_output(scene->layers, output->viewport->output);
    }
    output->owner->snapshot.outputs--;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->commit.link);
    wl_list_remove(&output->present.link);
    wl_list_remove(&output->link);
    wlr_scene_node_destroy(&output->wallpaper->node);
    wlr_scene_node_destroy(&output->banner->node);
    wlr_drm_syncobj_timeline_unref(output->unsubmitted_scanout);
    if (scene->appearance) {
        scene->appearance->viewport_changed(output->viewport->output, 0, 0);
    }
    free(output);
}

static void detach(struct seaside_scene_bridge *scene) {
    if (!scene->attached) {
        return;
    }
    end_focus(scene, false);
    remove_blur_watches(scene);
    remove_icon_manager_watch(scene);
    scene->window = nullptr;
    scene->window_data = nullptr;
    scene->watched_pid = 0;
    scene->focus = nullptr;
    scene->focus_data = nullptr;
    wl_list_remove(&scene->new_toplevel.listener.link);
    wl_list_remove(&scene->new_popup.listener.link);
    wl_list_remove(&scene->new_decoration.listener.link);
    wl_list_remove(&scene->shell_destroy.listener.link);
    if (scene->activation) {
        wl_list_remove(&scene->request_activation.listener.link);
        wl_list_remove(&scene->activation_destroy.listener.link);
        scene->activation = nullptr;
    }
    struct scene_view *view, *next_view;
    wl_list_for_each_safe(view, next_view, &scene->views, link) {
        destroy_view(view);
    }
    seaside_window_capture_destroy(scene->capture);
    scene->capture = nullptr;
    seaside_layer_scene_destroy(scene->layers);
    scene->layers = nullptr;
    while (!wl_list_empty(&scene->outputs)) {
        struct scene_output *output = wl_container_of(scene->outputs.next, output, link);
        wlr_scene_output_destroy(output->viewport);
    }
    seaside_scene_drop_image(scene->wallpaper);
    scene->wallpaper = nullptr;
    wlr_scene_node_destroy(&scene->scene->tree.node);
    scene->scene = nullptr;
    scene->shell = nullptr;
    scene->attached = false;
}

static void shell_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    auto *scene = SceneListener::owner(listener);
    detach(scene);
}

static void activation_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    auto *scene = SceneListener::owner(listener);
    wl_list_remove(&scene->request_activation.listener.link);
    wl_list_remove(&scene->activation_destroy.listener.link);
    scene->activation = nullptr;
}

static void request_activation(struct wl_listener *listener, void *data) noexcept {
    auto *scene = SceneListener::owner(listener);
    const struct wlr_xdg_activation_v1_request_activate_event *request =
        static_cast<const wlr_xdg_activation_v1_request_activate_event *>(data);
    struct wlr_surface *root = wlr_surface_get_root_surface(request->surface);
    struct scene_view *view =
        root ? toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root)) : nullptr;
    if (view && view->mapped && !view->placement.activated) {
        emit_window(view, SEASIDE_WINDOW_ATTENTION_REQUESTED, true, false);
    }
}

struct seaside_scene_bridge *seaside_scene_create(struct wl_display *display) {
    if (!display) {
        return nullptr;
    }
    std::unique_ptr<seaside_scene_bridge> owner{new (std::nothrow) seaside_scene_bridge{}};
    if (!owner) {
        return nullptr;
    }
    auto *scene = owner.get();
    scene->display = display;
    scene->direct_scanout = true;
    scene->titlebars = true;
    scene->active_border_color = 0xbfbfbf;
    scene->inactive_border_color = 0x595959;
    scene->temperature_kelvin = 4500;
    wl_list_init(&scene->views);
    wl_list_init(&scene->popups);
    wl_list_init(&scene->input_popup_views);
    wl_list_init(&scene->outputs);
    wl_list_init(&scene->blur_surfaces);
    wl_list_init(&scene->blur_new_surface.listener.link);
    wl_list_init(&scene->blur_compositor_destroy.listener.link);
    scene->scene = wlr_scene_create();
    if (!scene->scene) {
        return nullptr;
    }
    scene->backgrounds = wlr_scene_tree_create(&scene->scene->tree);
    scene->windows = wlr_scene_tree_create(&scene->scene->tree);
    scene->drag_icons = wlr_scene_tree_create(&scene->scene->tree);
    scene->input_popups = wlr_scene_tree_create(&scene->scene->tree);
    scene->decorations = wlr_scene_tree_create(&scene->scene->tree);
    if (!scene->backgrounds || !scene->windows || !scene->drag_icons || !scene->input_popups ||
        !scene->decorations) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        return nullptr;
    }
    scene->shell = wlr_xdg_shell_create(display, 6);
    if (!scene->shell) {
        wlr_scene_node_destroy(&scene->scene->tree.node);
        return nullptr;
    }
    scene->activation = wlr_xdg_activation_v1_create(display);
    struct wlr_xdg_decoration_manager_v1 *decoration_manager =
        wlr_xdg_decoration_manager_v1_create(display);
    struct wlr_xdg_toplevel_icon_manager_v1 *icon_manager =
        wlr_xdg_toplevel_icon_manager_v1_create(display, 1);
    scene->tearing = wlr_tearing_control_manager_v1_create(display, 1);
    scene->layers = seaside_layer_scene_create(display, scene->windows, scene->drag_icons);
    if (!scene->activation || !decoration_manager || !icon_manager || !scene->tearing ||
        !scene->layers) {
        seaside_layer_scene_destroy(scene->layers);
        wlr_scene_node_destroy(&scene->scene->tree.node);
        return nullptr;
    }
    wlr_scene_node_place_below(&scene->input_popups->node, &scene->drag_icons->node);

    scene->new_toplevel.connect(scene->shell->events.new_toplevel, *scene, new_toplevel);
    scene->new_popup.connect(scene->shell->events.new_popup, *scene, new_popup);
    scene->new_decoration.connect(decoration_manager->events.new_toplevel_decoration, *scene,
                                  new_decoration);
    int icon_sizes[] = {32, window_icon_size};
    wlr_xdg_toplevel_icon_manager_v1_set_sizes(icon_manager, icon_sizes, 2);
    scene->set_icon.connect(icon_manager->events.set_icon, *scene, set_icon);
    scene->icon_manager_destroy.connect(icon_manager->events.destroy, *scene, icon_manager_destroy);
    scene->shell_destroy.connect(scene->shell->events.destroy, *scene, shell_destroy);
    scene->activation_destroy.connect(scene->activation->events.destroy, *scene,
                                      activation_destroy);
    scene->request_activation.connect(scene->activation->events.request_activate, *scene,
                                      request_activation);
    scene->attached = true;
    return owner.release();
}

void seaside_scene_watch_surfaces(struct seaside_scene_bridge *scene,
                                  struct wlr_compositor *compositor) {
    if (compositor && wl_list_empty(&scene->blur_new_surface.listener.link)) {

        scene->blur_new_surface.connect(compositor->events.new_surface, *scene, blur_new_surface);
        scene->blur_compositor_destroy.connect(compositor->events.destroy, *scene,
                                               blur_compositor_destroy);
    }
}

void seaside_scene_set_linux_dmabuf(struct seaside_scene_bridge *scene,
                                    struct wlr_linux_dmabuf_v1 *dmabuf) {
    if (dmabuf) {
        wlr_scene_set_linux_dmabuf_v1(scene->scene, dmabuf);
    }
}

void seaside_scene_set_titlebars(struct seaside_scene_bridge *scene, bool visible) {
    if (!scene || !scene->attached || scene->titlebars == visible) {
        return;
    }
    scene->titlebars = visible;
    struct scene_view *view;
    wl_list_for_each(view, &scene->views, link) {
        configure_decoration(view);
    }
}

void seaside_scene_set_border_colors(struct seaside_scene_bridge *scene, uint32_t active,
                                     uint32_t inactive) {
    scene->active_border_color = active;
    scene->inactive_border_color = inactive;
}

void seaside_scene_set_render_policy(struct seaside_scene_bridge *scene, bool direct_scanout,
                                     bool allow_tearing, bool night_light, int temperature_kelvin) {
    const bool color_changed = scene->night_light != night_light ||
                               (night_light && scene->temperature_kelvin != temperature_kelvin);
    const bool redraw = color_changed || scene->allow_tearing != allow_tearing ||
                        scene->direct_scanout != direct_scanout;
    scene->direct_scanout = direct_scanout;
    scene->allow_tearing = allow_tearing;
    scene->night_light = night_light;
    scene->temperature_kelvin = temperature_kelvin;
    if (redraw) {
        struct scene_output *view;
        wl_list_for_each(view, &scene->outputs, link) {
            if (color_changed) {
                view->lut_request = LutRequest::pending;
            }
            view->tearing_window = 0;
            view->render_status.tearing_reason = nullptr;
            wlr_output_schedule_frame(view->viewport->output);
        }
    }
}

void seaside_scene_set_blur(struct seaside_scene_bridge *scene, bool enabled, double strength,
                            int passes) {
    if (!scene || !scene->attached) {
        return;
    }
    const double effective = enabled && isfinite(strength) ? fmin(1, fmax(0, strength)) : 0;
    if (scene->blur_strength == effective && scene->blur_passes == passes) {
        return;
    }
    scene->blur_strength = effective;
    scene->blur_passes = passes;
    if (effective == 0) {
        struct scene_view *view;
        wl_list_for_each(view, &scene->views, link) {
            seaside_scene_blur_destroy(&view->blur);
        }
    }
    struct scene_output *output;
    wl_list_for_each(output, &scene->outputs, link) {
        wlr_output_schedule_frame(output->viewport->output);
    }
}

void seaside_scene_set_frame_observer(struct seaside_scene_bridge *scene,
                                      seaside_scene_frame_fn callback, void *data) {
    scene->frame_observer = callback;
    scene->frame_observer_data = data;
}

void seaside_scene_set_before_frame(struct seaside_scene_bridge *scene,
                                    seaside_scene_before_frame_fn callback, void *data) {
    scene->before_frame = callback;
    scene->before_frame_data = data;
}

void seaside_scene_schedule_frame(struct wlr_output *output) {
    if (output) {
        wlr_output_schedule_frame(output);
    }
}

struct wl_event_loop *seaside_scene_event_loop(const struct seaside_scene_bridge *scene) {
    return wl_display_get_event_loop(scene->display);
}

struct seaside_scene_render_status
seaside_scene_get_render_status(const struct seaside_scene_bridge *scene,
                                struct wlr_output *output) {
    const struct scene_output *view = output ? find_output(scene, output) : nullptr;
    if (!view || !output->enabled) {
        seaside_scene_render_status inactive{};
        inactive.frame_state = "inactive";
        inactive.presentation_mode = "unknown";
        return inactive;
    }
    struct seaside_scene_render_status status = view->render_status;
    status.backend = wlr_output_is_drm(output)        ? "drm"
                     : wlr_output_is_headless(output) ? "headless"
                                                      : nullptr;
    status.frame_state = output->frame_pending                          ? "waiting_for_present"
                         : wlr_scene_output_needs_frame(view->viewport) ? "scheduled"
                                                                        : "idle";
    const size_t gamma_size = wlr_output_get_gamma_size(output);
    status.night_light = gamma_size >= 2 && gamma_size <= UINT16_MAX + 1u
                             ? SEASIDE_OUTPUT_SUPPORTED
                             : SEASIDE_OUTPUT_UNSUPPORTED;
    if (status.night_light == SEASIDE_OUTPUT_UNSUPPORTED) {
        status.night_light_reason = "no usable hardware gamma LUT";
    }
    if (status.tearing == SEASIDE_OUTPUT_UNSUPPORTED) {
        status.tearing_reason = "backend has no asynchronous page flips";
    }
    return status;
}

void seaside_scene_set_drag_icon(struct seaside_scene_bridge *scene, struct wlr_drag_icon *icon) {
    if (scene && scene->attached && icon) {
        wlr_scene_drag_icon_create(scene->drag_icons, icon);
    }
}

void seaside_scene_move_drag_icon(struct seaside_scene_bridge *scene, int x, int y) {
    if (scene && scene->attached && !wl_list_empty(&scene->drag_icons->children)) {
        wlr_scene_node_set_position(&scene->drag_icons->node, x, y);
    }
}

void seaside_scene_destroy(struct seaside_scene_bridge *scene) {
    if (scene) {
        detach(scene);
        delete scene;
    }
}

bool seaside_scene_add_output(struct seaside_scene_bridge *scene, struct wlr_output *output) {
    if (!scene || !scene->attached || !output || !output->enabled ||
        wlr_scene_get_scene_output(scene->scene, output)) {
        return false;
    }
    struct scene_output *view = static_cast<scene_output *>(calloc(1, sizeof(*view)));
    if (!view) {
        return false;
    }
    view->owner = scene;
    view->viewport = wlr_scene_output_create(scene->scene, output);
    if (!view->viewport) {
        free(view);
        return false;
    }
    view->wallpaper = wlr_scene_buffer_create(scene->backgrounds, scene->wallpaper);
    view->banner = wlr_scene_buffer_create(scene->decorations, nullptr);
    int width, height;
    wlr_output_effective_resolution(output, &width, &height);
    if (!view->wallpaper || !view->banner ||
        !seaside_layer_scene_add_output(scene->layers, output,
                                        seaside_scene_rect{0, 0, width, height})) {
        if (view->wallpaper) {
            wlr_scene_node_destroy(&view->wallpaper->node);
        }
        if (view->banner) {
            wlr_scene_node_destroy(&view->banner->node);
        }
        wlr_scene_output_destroy(view->viewport);
        free(view);
        return false;
    }
    view->frame.notify = output_frame;
    view->destroy.notify = output_destroy;
    view->commit.notify = output_commit;
    view->present.notify = output_present;
    view->render_status.tearing = tearing_support(output);
    view->render_status.presentation_mode = "unknown";
    view->lut_request = scene->night_light ? LutRequest::pending : LutRequest::clean;
    wl_signal_add(&output->events.frame, &view->frame);
    wl_signal_add(&view->viewport->events.destroy, &view->destroy);
    wl_signal_add(&output->events.commit, &view->commit);
    wl_signal_add(&output->events.present, &view->present);
    wl_list_insert(&scene->outputs, &view->link);
    scene->snapshot.outputs++;
    update_output_appearance(view);
    wlr_output_schedule_frame(output);
    return true;
}

bool seaside_scene_place_output(struct seaside_scene_bridge *scene, struct wlr_output *output,
                                int x, int y) {
    if (!scene || !scene->attached || !output) {
        return false;
    }
    struct scene_output *view = find_output(scene, output);
    if (!view) {
        return false;
    }
    if (view->viewport->x != x || view->viewport->y != y) {
        clear_output_blur(view);
    }
    wlr_scene_output_set_position(view->viewport, x, y);
    update_output_appearance(view);
    wlr_output_create_global(output, scene->display);
    return output->global != nullptr;
}

void seaside_scene_set_wallpaper(struct seaside_scene_bridge *scene, struct wlr_buffer *buffer) {
    if (!scene || !scene->attached) {
        seaside_scene_drop_image(buffer);
        return;
    }
    struct wlr_buffer *previous = scene->wallpaper;
    scene->wallpaper = buffer;
    struct scene_output *view;
    wl_list_for_each(view, &scene->outputs, link) {
        wlr_scene_buffer_set_buffer(view->wallpaper, buffer);
        fit_wallpaper(view);
    }
    seaside_scene_drop_image(previous);
}

void seaside_scene_set_banner(struct seaside_scene_bridge *scene, struct wlr_output *output,
                              struct wlr_buffer *buffer, int logical_height) {
    if (scene && scene->attached) {
        struct scene_output *view = find_output(scene, output);
        if (view) {
            view->banner_width = buffer ? buffer->width : 0;
            view->banner_height = buffer ? buffer->height : 0;
            view->banner_logical_height = buffer ? logical_height : 0;
            wlr_scene_buffer_set_buffer(view->banner, buffer);
            fit_banner(view, true);
        }
    }
    seaside_scene_drop_image(buffer);
}

void seaside::set_scene_appearance_observer(seaside_scene_bridge *scene,
                                            SceneAppearanceObserver *observer) noexcept {
    if (scene && scene->attached) {
        scene->appearance = observer;
    }
}

void seaside_scene_remove_output(struct seaside_scene_bridge *scene, struct wlr_output *output) {
    if (!scene || !scene->attached || !output) {
        return;
    }
    struct wlr_scene_output *viewport = wlr_scene_get_scene_output(scene->scene, output);
    if (viewport) {
        wlr_scene_output_destroy(viewport);
        wlr_output_destroy_global(output);
    }
}

void seaside_scene_set_window_handler(struct seaside_scene_bridge *scene,
                                      seaside_scene_window_fn handler, void *data) {
    if (scene && scene->attached) {
        scene->window = handler;
        scene->window_data = data;
    }
}

void seaside_scene_set_layer_handler(struct seaside_scene_bridge *scene,
                                     seaside_layer_output_fn focused_output,
                                     seaside_layer_changed_fn changed,
                                     seaside_layer_hidden_fn hidden, void *data) {
    if (scene && scene->attached) {
        seaside_layer_scene_set_handler(scene->layers, focused_output, changed, hidden, data);
    }
}

struct seaside_scene_rect seaside_scene_layer_work_area(struct seaside_scene_bridge *scene,
                                                        struct wlr_output *output,
                                                        struct seaside_scene_rect bounds,
                                                        bool fullscreen) {
    if (!scene || !scene->attached) {
        return bounds;
    }
    seaside_layer_scene_update_output(scene->layers, output, bounds);
    struct scene_output *view = find_output(scene, output);
    if (view && view->banner_fullscreen != fullscreen) {
        view->banner_fullscreen = fullscreen;
        fit_banner(view, false);
    }
    return seaside_layer_scene_work_area(scene->layers, output, fullscreen);
}

void seaside_scene_dismiss_unfocused_grabs(struct seaside_scene_bridge *scene,
                                           struct wlr_surface *target) {
    if (scene && scene->attached) {
        seaside_popup_dismiss_unfocused(scene->shell, target);
    }
}

void seaside_scene_select_layer(struct seaside_scene_bridge *scene, struct wlr_surface *surface) {
    if (scene && scene->attached) {
        seaside_layer_scene_select(scene->layers, surface);
    }
}

struct wlr_surface *seaside_scene_layer_keyboard_focus(struct seaside_scene_bridge *scene,
                                                       struct wlr_output *focused_output) {
    return scene && scene->attached
               ? seaside_layer_scene_keyboard_focus(scene->layers, focused_output)
               : nullptr;
}

bool seaside_scene_enable_window_capture(struct seaside_scene_bridge *scene,
                                         struct wlr_allocator *allocator,
                                         struct wlr_renderer *renderer,
                                         struct wlr_ext_image_copy_capture_manager_v1 *copy) {
    if (!scene || !scene->attached || scene->capture) {
        return false;
    }
    scene->capture = seaside_window_capture_create(scene->display, allocator, renderer, copy);
    return scene->capture != nullptr;
}

bool seaside_scene_window_capture_client_ready(const struct seaside_scene_bridge *scene, pid_t pid,
                                               uid_t uid) {
    return scene && seaside_window_capture_client_ready(scene->capture, pid, uid);
}

void seaside_scene_capture_pointer(struct seaside_scene_bridge *scene, double x, double y) {
    if (scene && scene->capture) {
        scene->cursor_known = isfinite(x) && isfinite(y);
        scene->cursor_x = x;
        scene->cursor_y = y;
        seaside_window_capture_cursor_changed(scene->capture);
    }
}

static bool below(const struct scene_view *left, const struct scene_view *right) {
    if (left->placement.stacking != right->placement.stacking) {
        return left->placement.stacking < right->placement.stacking;
    }
    return left->id < right->id;
}

static void restack_view(struct scene_view *view) {
    struct scene_view *previous = nullptr;
    struct scene_view *other;
    wl_list_for_each(other, &view->owner->views, link) {
        if (other->placement.visible && below(other, view) &&
            (!previous || below(previous, other))) {
            previous = other;
        }
    }
    if (previous) {
        wlr_scene_node_place_above(&view->tree->node, &previous->tree->node);
    } else {
        wlr_scene_node_lower_to_bottom(&view->tree->node);
    }
}

static void project_border(struct scene_view *view, size_t side, struct wlr_box bounds) {
    struct wlr_scene_rect *border = view->borders[side];
    if (bounds.width <= 0 || bounds.height <= 0) {
        if (border) {
            wlr_scene_node_set_enabled(&border->node, false);
        }
        return;
    }
    const uint32_t rgb = view->placement.activated ? view->owner->active_border_color
                                                   : view->owner->inactive_border_color;
    const float color[4] = {(float)((rgb >> 16) & 0xff) / 255.f, (float)((rgb >> 8) & 0xff) / 255.f,
                            (float)(rgb & 0xff) / 255.f, 1};
    if (!border) {
        border = wlr_scene_rect_create(view->tree, bounds.width, bounds.height, color);
        if (!border) {
            view->owner->snapshot.failed = true;
            return;
        }
        view->borders[side] = border;
        wlr_scene_node_place_below(&border->node, &view->xdg_tree->node);
    } else {
        wlr_scene_rect_set_color(border, color);
    }
    const struct seaside_scene_rect box = view->placement.geometry;
    wlr_scene_node_set_position(&border->node, clamp_coordinate((int64_t)bounds.x - box.x),
                                clamp_coordinate((int64_t)bounds.y - box.y));
    wlr_scene_rect_set_size(border, bounds.width, bounds.height);
    wlr_scene_node_set_enabled(&border->node, true);
}

static struct wlr_box border_bounds(const struct seaside_scene_window_placement *placement,
                                    struct seaside_scene_rect clip, int64_t x, int64_t y,
                                    int64_t width, int64_t height) {
    if (placement->unclipped_move) {
        return wlr_box{clamp_coordinate(x), clamp_coordinate(y), clamp_coordinate(width),
                       clamp_coordinate(height)};
    }
    return intersection(clip, x, y, width, height);
}

static void project_borders(struct scene_view *view) {
    const struct seaside_scene_window_placement *placement = &view->placement;
    const struct seaside_scene_rect clip =
        placement->tiled || has_viewport(placement->clip) ? placement->clip : placement->output;
    const struct seaside_scene_rect box = placement->geometry;
    const int64_t width = placement->border_width;
    if (width <= 0 || !has_viewport(box) || (!placement->unclipped_move && !has_viewport(clip))) {
        for (size_t side = 0; side < sizeof(view->borders) / sizeof(view->borders[0]); ++side) {
            project_border(view, side, wlr_box{});
        }
        return;
    }
    const int64_t left = (int64_t)box.x - width;
    const int64_t top = (int64_t)box.y - width;
    const int64_t right = (int64_t)box.x + box.width;
    const int64_t bottom = (int64_t)box.y + box.height;
    const int64_t span = box.width + 2 * width;
    project_border(view, 0, border_bounds(placement, clip, left, top, span, width));
    project_border(view, 1, border_bounds(placement, clip, left, bottom, span, width));
    project_border(view, 2, border_bounds(placement, clip, left, box.y, width, box.height));
    project_border(view, 3, border_bounds(placement, clip, right, box.y, width, box.height));
}

void seaside_scene_place_window(struct seaside_scene_bridge *scene, uint64_t id,
                                const struct seaside_scene_window_placement *placement) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return;
    }
    struct seaside_scene_window_placement previous = view->placement;
    view->placement = *placement;
    const struct seaside_scene_rect box = placement->geometry;
    if (previous.geometry.x != box.x || previous.geometry.y != box.y ||
        previous.geometry.width != box.width || previous.geometry.height != box.height ||
        previous.fullscreen != placement->fullscreen || previous.visible != placement->visible) {
        // Patches are clipped to their output and positioned relative to this
        // tree. Rebuild them before drawing at the new location.
        seaside_scene_blur_destroy(&view->blur);
    }
    wlr_scene_node_set_position(&view->tree->node, box.x, box.y);
    place_popup_tree(view);
    project_view_content(view);
    project_borders(view);
    wlr_scene_node_set_enabled(&view->tree->node, view->mapped && placement->visible);
    seaside_window_capture_presented(view->capture, view_presented(view));
    if (placement->visible && (!previous.visible || placement->stacking != previous.stacking)) {
        restack_view(view);
    }
    if (previous.geometry.x != box.x || previous.geometry.y != box.y ||
        previous.geometry.width != box.width || previous.geometry.height != box.height ||
        previous.visible != placement->visible || previous.stacking != placement->stacking ||
        previous.fullscreen != placement->fullscreen ||
        previous.stretch_fullscreen != placement->stretch_fullscreen ||
        previous.fit_resize_buffer != placement->fit_resize_buffer) {
        invalidate_active_blur(scene);
        seaside_window_capture_cursor_changed(scene->capture);
    }
    /* Keep the background opaque throughout the fullscreen transition and
     * behind any letterboxing, pillarboxing or transparent content. */
    if (placement->fullscreen && !view->fullscreen_background) {
        const float black[4] = {0, 0, 0, 1};
        view->fullscreen_background =
            wlr_scene_rect_create(view->tree, box.width, box.height, black);
        if (!view->fullscreen_background) {
            scene->snapshot.failed = true;
        } else {
            wlr_scene_node_lower_to_bottom(&view->fullscreen_background->node);
        }
    }
    if (view->fullscreen_background) {
        wlr_scene_node_set_enabled(&view->fullscreen_background->node, placement->fullscreen);
        if (placement->fullscreen) {
            wlr_scene_rect_set_size(view->fullscreen_background, box.width, box.height);
        }
    }
    struct wlr_xdg_toplevel *toplevel = view->toplevel;
    if (previous.visible && !placement->visible) {
        while (!wl_list_empty(&toplevel->base->popups)) {
            struct wlr_xdg_popup *popup = wl_container_of(toplevel->base->popups.next, popup, link);
            wlr_xdg_popup_destroy(popup);
        }
    }
    if (!toplevel->base->initialized) {
        return;
    }
    const bool maximized = placement->maximized && !placement->fullscreen;
    if (toplevel->scheduled.maximized != maximized) {
        wlr_xdg_toplevel_set_maximized(toplevel, maximized);
    }
    if (toplevel->scheduled.fullscreen != placement->fullscreen) {
        wlr_xdg_toplevel_set_fullscreen(toplevel, placement->fullscreen);
    }
    if (toplevel->scheduled.activated != placement->activated) {
        wlr_xdg_toplevel_set_activated(toplevel, placement->activated);
    }
    const bool was_configured_tiled = toplevel->scheduled.tiled != 0;
    const uint32_t tiled =
        placement->tiled ? WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT : 0;
    if (toplevel->scheduled.tiled != tiled) {
        wlr_xdg_toplevel_set_tiled(toplevel, tiled);
    }
    if (toplevel->scheduled.resizing != placement->resizing) {
        wlr_xdg_toplevel_set_resizing(toplevel, placement->resizing);
    }
    const bool managed = maximized || placement->fullscreen || placement->tiled;
    const bool restoring =
        !managed &&
        (previous.maximized || previous.fullscreen || previous.tiled || was_configured_tiled ||
         (placement->unconstrained_restore &&
          (toplevel->scheduled.width || toplevel->scheduled.height)));
    const struct wlr_box content = toplevel->base->geometry;
    const bool unchanged_floating_grab = !managed && !previous.interactive_resize &&
                                         placement->interactive_resize &&
                                         content.width == box.width && content.height == box.height;
    if (restoring || (has_viewport(box) && !unchanged_floating_grab &&
                      (managed || placement->resize || previous.interactive_resize))) {
        request_placed_size(view, managed, restoring);
    }
}

uint32_t seaside_scene_configure_tiled_window(struct seaside_scene_bridge *scene, uint64_t id,
                                              struct seaside_scene_rect geometry) {
    struct scene_view *view = find_view(scene, id);
    if (!view || !view->toplevel->base->initialized || !has_viewport(geometry)) {
        return 0;
    }
    struct wlr_xdg_toplevel *toplevel = view->toplevel;
    const uint32_t tiled = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    if (toplevel->scheduled.tiled != tiled) {
        view->size_configure_serial = wlr_xdg_toplevel_set_tiled(toplevel, tiled);
    }
    if (toplevel->scheduled.width != geometry.width ||
        toplevel->scheduled.height != geometry.height) {
        view->size_configure_serial =
            wlr_xdg_toplevel_set_size(toplevel, geometry.width, geometry.height);
    }
    return view->size_configure_serial;
}

uint32_t seaside_scene_window_commit_serial(const struct seaside_scene_bridge *scene, uint64_t id) {
    const struct scene_view *view = find_view(scene, id);
    return view ? view->toplevel->base->current.configure_serial : 0;
}

struct seaside_scene_rect
seaside_scene_window_content_geometry(const struct seaside_scene_bridge *scene, uint64_t id) {
    const struct scene_view *view = find_view(scene, id);
    if (!view || !view->mapped) {
        return seaside_scene_rect{};
    }
    const struct wlr_box box = view->toplevel->base->geometry;
    return seaside_scene_rect{box.x, box.y, box.width, box.height};
}

void seaside_scene_set_fit_resize_buffer(struct seaside_scene_bridge *scene, uint64_t id,
                                         bool fit) {
    struct scene_view *view = find_view(scene, id);
    if (view && view->placement.fit_resize_buffer != fit) {
        view->placement.fit_resize_buffer = fit;
        project_view_content(view);
        seaside_window_capture_cursor_changed(scene->capture);
    }
}

struct wlr_surface *seaside_scene_window_surface(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    return view && view->mapped ? view->toplevel->base->surface : nullptr;
}

bool seaside_scene_close_window(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view || !view->mapped) {
        return false;
    }
    wlr_xdg_toplevel_send_close(view->toplevel);
    return true;
}

struct seaside_scene_size_limits
seaside_scene_window_size_limits(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return seaside_scene_size_limits{};
    }
    const struct wlr_xdg_toplevel_state *current = &view->toplevel->current;
    return seaside_scene_size_limits{current->min_width, current->min_height, current->max_width,
                                     current->max_height};
}

struct seaside_scene_window_metadata
seaside_scene_get_window_metadata(struct seaside_scene_bridge *scene, uint64_t id) {
    struct scene_view *view = find_view(scene, id);
    if (!view) {
        return seaside_scene_window_metadata{};
    }
    return seaside_scene_window_metadata{
        .app_id = view->toplevel->app_id,
        .title = view->toplevel->title,
        .icon_name = view->icon_name.empty() ? nullptr : view->icon_name.c_str(),
        .icon_png_base64 = view->icon_png_base64.empty() ? nullptr : view->icon_png_base64.c_str(),
    };
}

static struct scene_view *node_view(struct seaside_scene_bridge *scene,
                                    struct wlr_scene_node *node) {
    while (node->parent && node->parent != scene->windows) {
        node = &node->parent->node;
    }
    return node->parent ? static_cast<scene_view *>(node->data) : nullptr;
}

struct wlr_output *seaside_scene_visible_output(const struct seaside_scene_bridge *scene,
                                                struct wlr_surface *surface) {
    if (!scene || !scene->attached || !surface || !surface->mapped) {
        return nullptr;
    }
    struct wlr_output *layer_output = seaside_layer_scene_visible_output(scene->layers, surface);
    if (layer_output) {
        return layer_output;
    }

    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    if (!root || !root->mapped) {
        return nullptr;
    }
    struct scene_popup *popup = surface_popup(scene, root);
    struct scene_view *view =
        popup ? popup->view : toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root));
    if (!view || !view->mapped || !view->placement.visible ||
        !has_viewport(view->placement.output)) {
        return nullptr;
    }
    struct wlr_scene_tree *tree = popup ? popup->surface_tree : view->tree;
    if (!seaside_scene_surface_intersects(tree, surface, view->placement.output)) {
        return nullptr;
    }
    struct scene_output *output;
    wl_list_for_each(output, &scene->outputs, link) {
        if (output->viewport->x == view->placement.output.x &&
            output->viewport->y == view->placement.output.y) {
            return output->viewport->output;
        }
    }
    return nullptr;
}

struct exact_target {
    struct seaside_scene_target *target;
    double x, y;
    bool found;
};

bool seaside_scene_pointer_content(struct seaside_scene_bridge *scene, struct wlr_surface *surface,
                                   struct seaside_pointer_content *content) {
    if (!scene || !scene->attached || !surface || !surface->mapped) {
        return false;
    }
    *content = {{}, 1, 1, false, false};
    if (seaside_layer_scene_visible_output(scene->layers, surface, &content->bounds)) {
        return true;
    }
    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    if (!root || !root->mapped) {
        return false;
    }
    struct scene_popup *popup = surface_popup(scene, root);
    struct scene_view *view =
        popup ? popup->view : toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root));
    if (!view || !view->mapped || !view->placement.visible ||
        !has_viewport(view->placement.output)) {
        return false;
    }
    struct wlr_scene_tree *tree = popup ? popup->surface_tree : view->tree;
    if (!seaside_scene_surface_intersects(tree, surface, view->placement.output,
                                          &content->bounds)) {
        return false;
    }
    const struct projected_surface *projected;
    wl_list_for_each(projected, &view->projected_surfaces, link) {
        if (projected->scene_surface->surface == surface && projected->width > 0 &&
            projected->height > 0) {
            content->layout_per_surface_x = projected->width / projected->source.width;
            content->layout_per_surface_y = projected->height / projected->source.height;
            break;
        }
    }
    content->toplevel = !popup && root == surface;
    content->fullscreen = view->placement.fullscreen;
    return true;
}

static void match_target(struct wlr_surface *surface, int x, int y, void *data) noexcept {
    struct exact_target *search = static_cast<exact_target *>(data);
    if (surface != search->target->surface) {
        return;
    }
    search->target->sx = search->x - x;
    search->target->sy = search->y - y;
    search->found = true;
}

static bool exact_surface_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target) {
    struct wlr_surface *root = wlr_surface_get_root_surface(target->surface);
    if (!root || !root->mapped) {
        return false;
    }
    struct scene_popup *popup = surface_popup(scene, root);
    struct scene_view *view =
        popup ? popup->view : toplevel_view(scene, wlr_xdg_toplevel_try_from_wlr_surface(root));
    if (!view || !view->mapped || (view->id && !view->placement.visible)) {
        return false;
    }
    const struct projected_surface *projected;
    wl_list_for_each(projected, &view->projected_surfaces, link) {
        if (projected->scene_surface->surface == target->surface && projected->width > 0 &&
            projected->height > 0) {
            int lx, ly;
            wlr_scene_node_coords(&projected->scene_surface->buffer->node, &lx, &ly);
            target->sx = x - lx;
            target->sy = y - ly;
            projected_surface_coords(projected, &target->sx, &target->sy);
            target->id = view->id;
            return true;
        }
    }
    /* An implicit grab still needs surface coordinates when a viewport shift
     * clips its original target. The protocol tree gives its uncropped origin. */
    int64_t lx, ly;
    struct wlr_scene_node *node = popup ? &popup->surface_tree->node : &view->tree->node;
    source_node_position(view, node, &lx, &ly);
    view_source_position(view, &x, &y);
    const struct wlr_box geometry = popup ? wlr_box{} : view->toplevel->base->geometry;
    struct exact_target search = {.target = target,
                                  .x = x - (double)lx + geometry.x,
                                  .y = y - (double)ly + geometry.y,
                                  .found = false};
    wlr_surface_for_each_surface(root, match_target, &search);
    if (search.found) {
        target->id = view->id;
    }
    return search.found;
}

static bool regular_surface_at(struct seaside_scene_bridge *scene, double x, double y,
                               struct seaside_scene_target *target) {
    return seaside_layer_scene_position(scene->layers, x, y, target) ||
           exact_surface_at(scene, x, y, target);
}

static struct scene_output *input_popup_output(const struct scene_input_popup *popup, double x,
                                               double y) {
    struct scene_output *output;
    wl_list_for_each(output, &popup->scene->outputs, link) {
        const double local_x = x - output->viewport->x, local_y = y - output->viewport->y;
        if (local_x >= 0 && local_y >= 0 && local_x < output->width && local_y < output->height) {
            return output;
        }
    }
    return find_output(popup->scene, seaside_scene_visible_output(popup->scene, popup->focus));
}

static double clamp_popup_axis(double position, int origin, int extent, int size) {
    return fmin(fmax(position, origin), (double)origin + fmax(0, extent - size));
}

static void update_input_popup(struct scene_input_popup *popup) {
    seaside_scene_target origin{popup->focus, 0, 0, 0};
    struct seaside_scene_target step = origin;
    if (!popup->focus || !regular_surface_at(popup->scene, 0, 0, &origin) ||
        !regular_surface_at(popup->scene, 1, 1, &step)) {
        wlr_scene_node_set_enabled(&popup->tree->node, false);
        return;
    }
    const double dx = step.sx - origin.sx, dy = step.sy - origin.sy;
    if (!(dx > 0 && dy > 0)) {
        wlr_scene_node_set_enabled(&popup->tree->node, false);
        return;
    }
    const struct wlr_box cursor =
        popup->has_cursor ? popup->cursor : wlr_box{0, 0, 0, popup->focus->current.height};
    const double left = (cursor.x - origin.sx) / dx;
    const double top = (cursor.y - origin.sy) / dy;
    const double right = left + fmax(0, cursor.width) / dx;
    const double bottom = top + fmax(0, cursor.height) / dy;
    struct scene_output *output = input_popup_output(popup, (left + right) / 2, (top + bottom) / 2);
    if (!output) {
        wlr_scene_node_set_enabled(&popup->tree->node, false);
        return;
    }
    const int width = popup->role->surface->current.width;
    const int height = popup->role->surface->current.height;
    const int ox = output->viewport->x, oy = output->viewport->y;
    const double y = bottom + height <= (double)oy + output->height ? bottom : top - height;
    const int px = clamp_rounded_coordinate(clamp_popup_axis(left, ox, output->width, width));
    const int py = clamp_rounded_coordinate(clamp_popup_axis(y, oy, output->height, height));
    wlr_scene_node_set_position(&popup->tree->node, px, py);
    wlr_scene_node_set_enabled(&popup->tree->node, popup->role->surface->mapped);
    struct wlr_box relative = {
        .x = clamp_rounded_coordinate(left - px),
        .y = clamp_rounded_coordinate(top - py),
        .width = clamp_rounded_coordinate(right - left),
        .height = clamp_rounded_coordinate(bottom - top),
    };
    if (popup->has_cursor &&
        (!popup->sent_rectangle || !wlr_box_equal(&popup->last_relative, &relative))) {
        wlr_input_popup_surface_v2_send_text_input_rectangle(popup->role, &relative);
        popup->sent_rectangle = true;
        popup->last_relative = relative;
    }
}

static void update_input_popups(struct seaside_scene_bridge *scene) {
    struct scene_input_popup *popup;
    wl_list_for_each(popup, &scene->input_popup_views, link) {
        update_input_popup(popup);
    }
}

static void input_popup_commit(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_input_popup *popup = wl_container_of(listener, popup, commit);
    update_input_popup(popup);
}

static void input_popup_focus_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_input_popup *popup = wl_container_of(listener, popup, focus_destroy);
    popup->focus = nullptr;
    wl_list_remove(&popup->focus_destroy.link);
    wl_list_init(&popup->focus_destroy.link);
    update_input_popup(popup);
}

static void input_popup_tree_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_input_popup *popup = wl_container_of(listener, popup, tree_destroy);
    wl_list_remove(&popup->link);
    wl_list_remove(&popup->destroy.link);
    wl_list_remove(&popup->tree_destroy.link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->focus_destroy.link);
    free(popup);
}

static void input_popup_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct scene_input_popup *popup = wl_container_of(listener, popup, destroy);
    wlr_scene_node_destroy(&popup->tree->node);
}

static struct scene_input_popup *find_input_popup(struct seaside_scene_bridge *scene,
                                                  struct wlr_input_popup_surface_v2 *role) {
    struct scene_input_popup *popup;
    wl_list_for_each(popup, &scene->input_popup_views, link) {
        if (popup->role == role) {
            return popup;
        }
    }
    return nullptr;
}

static struct scene_input_popup *create_input_popup(struct seaside_scene_bridge *scene,
                                                    struct wlr_input_popup_surface_v2 *role) {
    struct scene_input_popup *popup = static_cast<scene_input_popup *>(calloc(1, sizeof(*popup)));
    if (!popup) {
        return nullptr;
    }
    popup->scene = scene;
    popup->role = role;
    popup->tree = wlr_scene_tree_create(scene->input_popups);
    if (!popup->tree || !wlr_scene_subsurface_tree_create(popup->tree, role->surface)) {
        if (popup->tree) {
            wlr_scene_node_destroy(&popup->tree->node);
        }
        free(popup);
        return nullptr;
    }
    popup->destroy.notify = input_popup_destroy;
    popup->commit.notify = input_popup_commit;
    popup->tree_destroy.notify = input_popup_tree_destroy;
    popup->focus_destroy.notify = input_popup_focus_destroy;
    wl_list_init(&popup->focus_destroy.link);
    wl_list_insert(&scene->input_popup_views, &popup->link);
    wl_signal_add(&role->events.destroy, &popup->destroy);
    wl_signal_add(&role->surface->events.commit, &popup->commit);
    wl_signal_add(&popup->tree->node.events.destroy, &popup->tree_destroy);
    return popup;
}

void seaside_scene_set_input_popup(struct seaside_scene_bridge *scene,
                                   struct wlr_input_popup_surface_v2 *role,
                                   struct wlr_surface *focus, const struct wlr_box *cursor) {
    if (!scene || !scene->attached) {
        return;
    }
    struct scene_input_popup *popup = find_input_popup(scene, role);
    if (!popup && focus) {
        popup = create_input_popup(scene, role);
        if (!popup) {
            wl_client_post_no_memory(wl_resource_get_client(role->resource));
            return;
        }
    }
    if (!popup) {
        return;
    }
    if (popup->focus != focus) {
        wl_list_remove(&popup->focus_destroy.link);
        wl_list_init(&popup->focus_destroy.link);
        if (focus) {
            wl_signal_add(&focus->events.destroy, &popup->focus_destroy);
        }
        popup->focus = focus;
        popup->sent_rectangle = false;
    }
    if (!cursor) {
        popup->sent_rectangle = false;
    }
    popup->has_cursor = cursor != nullptr;
    if (cursor) {
        popup->cursor = *cursor;
    }
    update_input_popup(popup);
}

static bool input_popup_at(struct seaside_scene_bridge *scene, double x, double y,
                           struct seaside_scene_target *target) {
    if (target->surface) {
        struct wlr_surface *root = wlr_surface_get_root_surface(target->surface);
        struct wlr_input_popup_surface_v2 *role =
            root ? wlr_input_popup_surface_v2_try_from_wlr_surface(root) : nullptr;
        const struct scene_input_popup *popup = role ? find_input_popup(scene, role) : nullptr;
        if (!popup || !popup->tree->node.enabled) {
            return false;
        }
        struct exact_target search = {
            .target = target,
            .x = x - popup->tree->node.x,
            .y = y - popup->tree->node.y,
            .found = false,
        };
        wlr_surface_for_each_surface(root, match_target, &search);
        if (search.found) {
            target->id = 0;
        }
        return search.found;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&scene->input_popups->node, x, y, &target->sx, &target->sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return false;
    }
    struct wlr_scene_surface *surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!surface) {
        return false;
    }
    target->surface = surface->surface;
    target->id = 0;
    return true;
}

bool seaside_scene_window_at(struct seaside_scene_bridge *scene, double x, double y,
                             struct seaside_scene_target *target) {
    if (!scene || !scene->attached) {
        return false;
    }
    if (input_popup_at(scene, x, y, target)) {
        return true;
    }
    if (target->surface) {
        return regular_surface_at(scene, x, y, target);
    }
    if (seaside_layer_scene_at(scene->layers, true, x, y, target)) {
        return true;
    }
    struct wlr_scene_node *node =
        wlr_scene_node_at(&scene->windows->node, x, y, &target->sx, &target->sy);
    if (!node) {
        return seaside_layer_scene_at(scene->layers, false, x, y, target);
    }
    struct scene_view *view = node_view(scene, node);
    if (!view || !view->mapped) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_RECT) {
        for (size_t side = 0; side < sizeof(view->borders) / sizeof(view->borders[0]); ++side) {
            if (view->borders[side] && node == &view->borders[side]->node) {
                target->id = view->id;
                return true;
            }
        }
        return false;
    }
    if (node->type != WLR_SCENE_NODE_BUFFER) {
        return false;
    }
    struct wlr_scene_surface *surface =
        wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!surface) {
        return false;
    }
    target->id = view->id;
    target->surface = surface->surface;
    return true;
}

struct seaside_scene_snapshot seaside_scene_get_snapshot(const struct seaside_scene_bridge *scene) {
    return scene ? scene->snapshot : seaside_scene_snapshot{};
}

bool seaside_scene_watch_client(struct seaside_scene_bridge *scene, pid_t pid,
                                seaside_scene_focus_fn focus, void *data) {
    if (!scene || !scene->attached || scene->watched_pid != 0 || pid <= 0 || !focus) {
        return false;
    }
    scene->watched_pid = pid;
    scene->focus = focus;
    scene->focus_data = data;
    return true;
}

void seaside_scene_clear_client_watch(struct seaside_scene_bridge *scene) {
    if (scene) {
        end_focus(scene, true);
        scene->watched_pid = 0;
        scene->focus = nullptr;
        scene->focus_data = nullptr;
    }
}

#include "scene_blur.h"

#include <drm_fourcc.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/transform.h>

enum { BLUR_DOWNSAMPLE = 4, BLUR_MAX_DIMENSION = 1024, BLUR_MAX_PASSES = 4 };

struct blur_image {
    struct wlr_buffer buffer;
    uint32_t pixels[];
};

struct blur_patch {
    struct wl_list link;
    struct wlr_output *output;
    struct wlr_scene_tree *tree;
    struct blur_image *image;
    pixman_region32_t region;
    struct wlr_box sample;
};

struct seaside_scene_blur {
    struct wl_list patches;
};

struct blur_item {
    struct wlr_render_texture_options texture;
    struct wlr_render_rect_options rect;
    struct wlr_texture *owned_texture;
    float opacity;
    enum wlr_color_named_primaries primaries;
};

struct blur_pass {
    struct wlr_renderer *renderer;
    const struct wlr_scene_node *stop;
    const struct wlr_box *sample;
    double scale_x, scale_y;
    GArray *items;
    bool failed;
};

static void destroy_image(struct wlr_buffer *buffer) {
    struct blur_image *image = wl_container_of(buffer, image, buffer);
    wlr_buffer_finish(buffer);
    free(image);
}

static bool image_data(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format,
                       size_t *stride) {
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    struct blur_image *image = wl_container_of(buffer, image, buffer);
    *data = image->pixels;
    // Do not advertise opacity. Damage to a covered source must still schedule
    // a frame when the overlying backdrop samples it.
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)buffer->width * sizeof(*image->pixels);
    return true;
}

static void image_data_end(struct wlr_buffer *buffer) {
    (void)buffer;
}

static const struct wlr_buffer_impl image_impl = {
    .destroy = destroy_image,
    .begin_data_ptr_access = image_data,
    .end_data_ptr_access = image_data_end,
};

static struct blur_image *new_image(int width, int height) {
    const size_t count = (size_t)width * height;
    struct blur_image *image = malloc(sizeof(*image) + count * sizeof(*image->pixels));
    if (image) {
        wlr_buffer_init(&image->buffer, &image_impl, width, height);
    }
    return image;
}

static void destroy_patch(struct blur_patch *patch) {
    wl_list_remove(&patch->link);
    wlr_scene_node_destroy(&patch->tree->node);
    wlr_buffer_unlock(&patch->image->buffer);
    pixman_region32_fini(&patch->region);
    free(patch);
}

void seaside_scene_blur_remove_output(struct seaside_scene_blur *blur, struct wlr_output *output) {
    if (blur) {
        struct blur_patch *patch, *next;
        wl_list_for_each_safe(patch, next, &blur->patches, link) {
            if (patch->output == output) {
                destroy_patch(patch);
            }
        }
    }
}

void seaside_scene_blur_destroy(struct seaside_scene_blur **blur) {
    if (*blur) {
        struct blur_patch *patch, *next;
        wl_list_for_each_safe(patch, next, &(*blur)->patches, link) {
            destroy_patch(patch);
        }
        free(*blur);
        *blur = NULL;
    }
}

static bool has_texture(const struct wlr_scene_buffer *buffer) {
    // wlroots 0.20 may release a raw scene buffer after upload while the
    // cached texture remains in the scene. Borrow its cache during this pass.
    return buffer->buffer || buffer->WLR_PRIVATE.texture;
}

static struct wlr_box buffer_box(const struct wlr_scene_buffer *buffer, int x, int y) {
    if (buffer->dst_width > 0 && buffer->dst_height > 0) {
        return (struct wlr_box){x, y, buffer->dst_width, buffer->dst_height};
    }
    int width = buffer->WLR_PRIVATE.buffer_width, height = buffer->WLR_PRIVATE.buffer_height;
    wlr_output_transform_coords(buffer->transform, &width, &height);
    return (struct wlr_box){x, y, width, height};
}

static void collect_coverage(struct wlr_scene_node *node, const struct wlr_box *viewport,
                             pixman_region32_t *coverage, pixman_region32_t *opaque) {
    int x, y;
    if (!wlr_scene_node_coords(node, &x, &y)) {
        return;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
            collect_coverage(child, viewport, coverage, opaque);
        }
    } else if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
        const struct wlr_box box = {x, y, rect->width, rect->height};
        struct wlr_box visible;
        if (rect->color[3] == 1 && wlr_box_intersection(&visible, &box, viewport)) {
            pixman_region32_union_rect(opaque, opaque, visible.x, visible.y, visible.width,
                                       visible.height);
        }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        if (!has_texture(buffer) || buffer->opacity <= 0) {
            return;
        }
        const struct wlr_box box = buffer_box(buffer, x, y);
        struct wlr_box visible;
        if (!wlr_box_intersection(&visible, &box, viewport)) {
            return;
        }
        if (coverage) {
            pixman_region32_union_rect(coverage, coverage, visible.x, visible.y, visible.width,
                                       visible.height);
        }
        if (buffer->opacity < 1) {
            return;
        }
        if (buffer->WLR_PRIVATE.buffer_is_opaque) {
            pixman_region32_union_rect(opaque, opaque, visible.x, visible.y, visible.width,
                                       visible.height);
            return;
        }
        pixman_region32_t rendered_opaque;
        pixman_region32_init(&rendered_opaque);
        pixman_region32_copy(&rendered_opaque, &buffer->opaque_region);
        pixman_region32_translate(&rendered_opaque, x, y);
        pixman_region32_intersect_rect(&rendered_opaque, &rendered_opaque, visible.x, visible.y,
                                       visible.width, visible.height);
        pixman_region32_union(opaque, opaque, &rendered_opaque);
        pixman_region32_fini(&rendered_opaque);
    }
}

static bool collect_occluders(struct wlr_scene_node *node, const struct wlr_scene_node *stop,
                              const struct wlr_box *viewport, pixman_region32_t *opaque) {
    if (node == stop) {
        return true;
    }
    if (!node->enabled) {
        return false;
    }
    if (node->type == WLR_SCENE_NODE_TREE) {
        struct wlr_scene_node *child;
        wl_list_for_each_reverse(child, &wlr_scene_tree_from_node(node)->children, link) {
            if (collect_occluders(child, stop, viewport, opaque)) {
                return true;
            }
        }
    } else {
        collect_coverage(node, viewport, NULL, opaque);
    }
    return false;
}

static struct wlr_box pass_destination(const struct blur_pass *pass, const struct wlr_box *box) {
    const double left = (double)box->x - pass->sample->x, top = (double)box->y - pass->sample->y;
    const int x = (int)lround(left * pass->scale_x), y = (int)lround(top * pass->scale_y);
    return (struct wlr_box){x, y, (int)lround((left + box->width) * pass->scale_x) - x,
                            (int)lround((top + box->height) * pass->scale_y) - y};
}

static void collect_rect(struct blur_pass *pass, struct wlr_scene_rect *rect, int x, int y) {
    const struct wlr_box box = {x, y, rect->width, rect->height};
    struct wlr_box visible;
    if (rect->color[3] > 0 && wlr_box_intersection(&visible, pass->sample, &box)) {
        const struct wlr_box dest = pass_destination(pass, &visible);
        if (!wlr_box_empty(&dest)) {
            const struct blur_item item = {
                .rect = {.box = dest,
                         .color = {rect->color[0], rect->color[1], rect->color[2], rect->color[3]}},
            };
            g_array_append_val(pass->items, item);
        }
    }
}

static void crop_source(struct wlr_fbox *src, const struct wlr_scene_buffer *buffer,
                        const struct wlr_texture *texture, const struct wlr_box *box,
                        const struct wlr_box *visible) {
    *src = buffer->src_box;
    if (wlr_fbox_empty(src)) {
        *src = (struct wlr_fbox){0, 0, texture->width, texture->height};
    }
    int width = (int)texture->width, height = (int)texture->height;
    wlr_fbox_transform(src, src, buffer->transform, width, height);
    src->x += ((double)visible->x - box->x) * src->width / box->width;
    src->y += ((double)visible->y - box->y) * src->height / box->height;
    src->width *= (double)visible->width / box->width;
    src->height *= (double)visible->height / box->height;
    wlr_output_transform_coords(buffer->transform, &width, &height);
    wlr_fbox_transform(src, src, wlr_output_transform_invert(buffer->transform), width, height);
}

static void collect_buffer(struct blur_pass *pass, struct wlr_scene_buffer *buffer, int x, int y) {
    if (!has_texture(buffer) || buffer->opacity <= 0) {
        return;
    }
    const struct wlr_box box = buffer_box(buffer, x, y);
    struct wlr_box visible;
    if (!wlr_box_intersection(&visible, pass->sample, &box)) {
        return;
    }
    const struct wlr_box dest = pass_destination(pass, &visible);
    if (wlr_box_empty(&dest)) {
        return;
    }
    if (buffer->WLR_PRIVATE.wait_timeline) {
        // The regular scene pass owns these buffers' release fences.
        // A failed readback could leave a second GPU read in flight.
        pass->failed = true;
        return;
    }
    struct wlr_texture *texture = buffer->WLR_PRIVATE.texture, *owned_texture = NULL;
    if (!texture) {
        struct wlr_client_buffer *client = wlr_client_buffer_get(buffer->buffer);
        texture = client
                      ? client->texture
                      : (owned_texture = wlr_texture_from_buffer(pass->renderer, buffer->buffer));
    }
    if (!texture || texture->renderer != pass->renderer) {
        wlr_texture_destroy(owned_texture);
        pass->failed = true;
        return;
    }
    struct wlr_fbox cropped;
    crop_source(&cropped, buffer, texture, &box, &visible);
    const struct blur_item item = {
        .texture = {.texture = texture,
                    .src_box = cropped,
                    .dst_box = dest,
                    .transform = wlr_output_transform_invert(buffer->transform),
                    .filter_mode = buffer->filter_mode,
                    .transfer_function = buffer->transfer_function,
                    .color_encoding = buffer->color_encoding,
                    .color_range = buffer->color_range},
        .owned_texture = owned_texture,
        .opacity = buffer->opacity,
        .primaries = buffer->primaries,
    };
    g_array_append_val(pass->items, item);
}

static bool collect_backdrop(struct blur_pass *pass, struct wlr_scene_node *node) {
    if (node == pass->stop || pass->failed) {
        return true;
    }
    int x, y;
    if (!wlr_scene_node_coords(node, &x, &y)) {
        return false;
    }
    switch (node->type) {
    case WLR_SCENE_NODE_TREE: {
        struct wlr_scene_node *child;
        wl_list_for_each(child, &wlr_scene_tree_from_node(node)->children, link) {
            if (collect_backdrop(pass, child)) {
                return true;
            }
        }
        break;
    }
    case WLR_SCENE_NODE_RECT:
        collect_rect(pass, wlr_scene_rect_from_node(node), x, y);
        break;
    case WLR_SCENE_NODE_BUFFER:
        collect_buffer(pass, wlr_scene_buffer_from_node(node), x, y);
        break;
    }
    return false;
}

static bool read_backdrop(struct wlr_texture *texture, struct blur_image *image) {
    struct wlr_texture_read_pixels_options options = {
        .data = image->pixels,
        .format = wlr_texture_preferred_read_format(texture),
        .stride = (uint32_t)image->buffer.width * sizeof(uint32_t),
    };
    if (options.format != DRM_FORMAT_XRGB8888 && options.format != DRM_FORMAT_ARGB8888 &&
        options.format != DRM_FORMAT_XBGR8888 && options.format != DRM_FORMAT_ABGR8888) {
        options.format = DRM_FORMAT_XRGB8888;
    }
    if (!wlr_texture_read_pixels(texture, &options)) {
        if (options.format == DRM_FORMAT_ABGR8888) {
            return false;
        }
        options.format = DRM_FORMAT_ABGR8888;
        if (!wlr_texture_read_pixels(texture, &options)) {
            return false;
        }
    }
    if (options.format == DRM_FORMAT_XBGR8888 || options.format == DRM_FORMAT_ABGR8888) {
        const size_t count = (size_t)image->buffer.width * image->buffer.height;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t pixel = image->pixels[i];
            image->pixels[i] = ((pixel & 0xff) << 16) | (pixel & 0xff00) | ((pixel >> 16) & 0xff);
        }
    }
    return true;
}

static bool render_backdrop(struct wlr_scene_tree *stack, struct wlr_scene_tree *window,
                            struct wlr_output *output, const struct wlr_box *sample,
                            struct blur_image *image) {
    struct wlr_renderer *renderer = output->renderer;
    struct wlr_allocator *allocator = output->allocator;
    const struct wlr_drm_format_set *formats =
        wlr_renderer_get_texture_formats(renderer, allocator->buffer_caps);
    const struct wlr_drm_format *format =
        formats ? wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888) : NULL;
    const int width = image->buffer.width, height = image->buffer.height;
    struct wlr_buffer *target =
        format ? wlr_allocator_create_buffer(allocator, width, height, format) : NULL;
    if (!target) {
        return false;
    }
    struct blur_pass backdrop = {.renderer = renderer,
                                 .stop = &window->node,
                                 .sample = sample,
                                 .scale_x = (double)width / sample->width,
                                 .scale_y = (double)height / sample->height,
                                 .items = g_array_new(false, false, sizeof(struct blur_item))};
    const bool found = collect_backdrop(&backdrop, &stack->node);
    struct wlr_render_pass *pass =
        found && !backdrop.failed ? wlr_renderer_begin_buffer_pass(renderer, target, NULL) : NULL;
    bool rendered = false;
    if (pass) {
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box = {0, 0, width, height},
                                           .color = {0, 0, 0, 1},
                                           .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                       });
        for (unsigned i = 0; i < backdrop.items->len; ++i) {
            struct blur_item *item = &g_array_index(backdrop.items, struct blur_item, i);
            if (item->texture.texture) {
                struct wlr_render_texture_options texture = item->texture;
                struct wlr_color_primaries primaries;
                texture.alpha = &item->opacity;
                if (item->primaries) {
                    wlr_color_primaries_from_named(&primaries, item->primaries);
                    texture.primaries = &primaries;
                }
                wlr_render_pass_add_texture(pass, &texture);
            } else {
                wlr_render_pass_add_rect(pass, &item->rect);
            }
        }
        if (wlr_render_pass_submit(pass)) {
            struct wlr_texture *texture = wlr_texture_from_buffer(renderer, target);
            rendered = texture && read_backdrop(texture, image);
            wlr_texture_destroy(texture);
        }
    }
    for (unsigned i = 0; i < backdrop.items->len; ++i) {
        wlr_texture_destroy(g_array_index(backdrop.items, struct blur_item, i).owned_texture);
    }
    g_array_unref(backdrop.items);
    wlr_buffer_drop(target);
    return rendered;
}

static int sample_index(int index, int count) {
    return index < 0 ? 0 : index >= count ? count - 1 : index;
}

static void filter_lines(uint32_t *out, const uint32_t *in, int length, int lines, int stride,
                         int offset, int radius) {
    const unsigned span = 2u * radius + 1;
    for (int line = 0; line < lines; ++line) {
        const uint32_t *source = in + line * offset;
        uint32_t *target = out + line * offset;
        unsigned r = 0, g = 0, b = 0;
        for (int index = -radius; index <= radius; ++index) {
            const uint32_t pixel = source[sample_index(index, length) * stride];
            r += (pixel >> 16) & 0xff;
            g += (pixel >> 8) & 0xff;
            b += pixel & 0xff;
        }
        for (int index = 0; index < length; ++index) {
            target[index * stride] =
                UINT32_C(0xff000000) | ((r / span) << 16) | ((g / span) << 8) | (b / span);
            const uint32_t head = source[sample_index(index + radius + 1, length) * stride];
            const uint32_t tail = source[sample_index(index - radius, length) * stride];
            r = r + ((head >> 16) & 0xff) - ((tail >> 16) & 0xff);
            g = g + ((head >> 8) & 0xff) - ((tail >> 8) & 0xff);
            b = b + (head & 0xff) - (tail & 0xff);
        }
    }
}

static bool filter_image(struct blur_image *image, int radius, int passes) {
    const int width = image->buffer.width, height = image->buffer.height;
    if (width <= 0 || height <= 0) {
        return false;
    }
    uint32_t *scratch = malloc((size_t)width * height * sizeof(*scratch));
    if (!scratch) {
        return false;
    }
    for (int pass = 0; pass < passes; ++pass) {
        filter_lines(scratch, image->pixels, width, height, 1, width, radius);
        filter_lines(image->pixels, scratch, height, width, width, 1, radius);
    }
    free(scratch);
    return true;
}

static bool ignore_blur_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    (void)buffer;
    (void)sx;
    (void)sy;
    return false;
}

static struct blur_patch *create_patch(struct seaside_scene_blur *blur,
                                       struct wlr_scene_tree *window, struct wlr_output *output,
                                       const struct wlr_box *sample,
                                       const pixman_region32_t *region, struct blur_image *image,
                                       float opacity) {
    struct blur_patch *patch = calloc(1, sizeof(*patch));
    if (!patch) {
        return NULL;
    }
    patch->tree = wlr_scene_tree_create(window);
    if (!patch->tree) {
        free(patch);
        return NULL;
    }
    patch->output = output;
    patch->sample = *sample;
    patch->image = image;
    wlr_buffer_lock(&image->buffer);
    pixman_region32_init(&patch->region);
    pixman_region32_copy(&patch->region, region);
    wl_list_insert(&blur->patches, &patch->link);
    wlr_scene_node_lower_to_bottom(&patch->tree->node);
    int x, y, count;
    wlr_scene_node_coords(&window->node, &x, &y);
    wlr_scene_node_set_position(&patch->tree->node, sample->x - x, sample->y - y);
    const pixman_box32_t *rects = pixman_region32_rectangles(&patch->region, &count);
    const double sx = (double)image->buffer.width / sample->width;
    const double sy = (double)image->buffer.height / sample->height;
    for (int i = 0; i < count; ++i) {
        const pixman_box32_t r = rects[i];
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_create(patch->tree, &image->buffer);
        if (!buffer) {
            destroy_patch(patch);
            return NULL;
        }
        buffer->point_accepts_input = ignore_blur_input;
        wlr_scene_buffer_set_source_box(buffer, &(struct wlr_fbox){
                                                    (r.x1 - sample->x) * sx,
                                                    (r.y1 - sample->y) * sy,
                                                    (r.x2 - r.x1) * sx,
                                                    (r.y2 - r.y1) * sy,
                                                });
        wlr_scene_buffer_set_dest_size(buffer, r.x2 - r.x1, r.y2 - r.y1);
        wlr_scene_buffer_set_opacity(buffer, opacity);
        wlr_scene_node_set_position(&buffer->node, r.x1 - sample->x, r.y1 - sample->y);
    }
    return patch;
}

static struct blur_patch *output_patch(struct seaside_scene_blur *blur, struct wlr_output *output) {
    if (blur) {
        struct blur_patch *patch;
        wl_list_for_each(patch, &blur->patches, link) {
            if (patch->output == output) {
                return patch;
            }
        }
    }
    return NULL;
}

static bool box_equal(const struct wlr_box *a, const struct wlr_box *b) {
    return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static struct wlr_box sample_box(const pixman_box32_t *extents, const struct wlr_box *viewport,
                                 int padding) {
    const int64_t min_x = (int64_t)extents->x1 - padding, min_y = (int64_t)extents->y1 - padding;
    const int64_t max_x = (int64_t)extents->x2 + padding, max_y = (int64_t)extents->y2 + padding;
    const int64_t end_x = (int64_t)viewport->x + viewport->width,
                  end_y = (int64_t)viewport->y + viewport->height;
    const int x = min_x < viewport->x ? viewport->x : (int)min_x;
    const int y = min_y < viewport->y ? viewport->y : (int)min_y;
    return (struct wlr_box){x, y, (int)((max_x > end_x ? end_x : max_x) - x),
                            (int)((max_y > end_y ? end_y : max_y) - y)};
}

static void update_patch(struct blur_patch *patch, struct wlr_scene_tree *window,
                         struct blur_image *image, float opacity) {
    int x, y;
    wlr_scene_node_coords(&window->node, &x, &y);
    wlr_scene_node_set_position(&patch->tree->node, patch->sample.x - x, patch->sample.y - y);
    const int width = image->buffer.width, height = image->buffer.height;
    const bool changed =
        memcmp(patch->image->pixels, image->pixels, (size_t)width * height * sizeof(uint32_t)) != 0;
    struct wlr_scene_node *node;
    wl_list_for_each(node, &patch->tree->children, link) {
        struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(node);
        if (changed) {
            wlr_scene_buffer_set_buffer(buffer, &image->buffer);
        }
        wlr_scene_buffer_set_opacity(buffer, opacity);
    }
    if (changed) {
        wlr_buffer_lock(&image->buffer);
        wlr_buffer_unlock(&patch->image->buffer);
        patch->image = image;
    }
}

bool seaside_scene_blur_update(struct seaside_scene_blur **blur, struct wlr_scene_tree *stack,
                               struct wlr_scene_tree *window, struct wlr_scene_tree *content,
                               struct wlr_output *output, const struct wlr_box *viewport,
                               double strength, int passes) {
    if (!output->renderer || !output->allocator || wlr_box_empty(viewport) || !isfinite(strength) ||
        strength <= 0) {
        seaside_scene_blur_remove_output(*blur, output);
        return false;
    }
    pixman_region32_t coverage, opaque;
    pixman_region32_init(&coverage);
    pixman_region32_init(&opaque);
    collect_coverage(&content->node, viewport, &coverage, &opaque);
    if (pixman_region32_not_empty(&coverage)) {
        // Remove coverage already hidden by a higher opaque node (including
        // the fullscreen backing rectangle). Invisible effects need no pass.
        collect_occluders(&stack->node, &window->node, viewport, &opaque);
    }
    pixman_region32_subtract(&coverage, &coverage, &opaque);
    pixman_region32_fini(&opaque);

    struct blur_patch *patch = output_patch(*blur, output);
    bool applied = false;
    if (pixman_region32_not_empty(&coverage)) {
        const int extent = viewport->width > viewport->height ? viewport->width : viewport->height;
        int scale = (extent - 1) / BLUR_MAX_DIMENSION + 1;
        if (scale < BLUR_DOWNSAMPLE) {
            scale = BLUR_DOWNSAMPLE;
        }
        passes = passes < 1 ? 1 : passes > BLUR_MAX_PASSES ? BLUR_MAX_PASSES : passes;
        strength = fmin(strength, 1);
        const int radius = (int)fmax(1, round(6 * strength));
        const float opacity = (float)fmin(1, strength * 6);
        const struct wlr_box sample =
            sample_box(pixman_region32_extents(&coverage), viewport, radius * passes * scale);
        const int width = (sample.width - 1) / scale + 1, height = (sample.height - 1) / scale + 1;
        struct blur_image *image = new_image(width, height);
        if (image && render_backdrop(stack, window, output, &sample, image) &&
            filter_image(image, radius, passes)) {
            if (patch && box_equal(&patch->sample, &sample) &&
                patch->image->buffer.width == width && patch->image->buffer.height == height &&
                pixman_region32_equal(&patch->region, &coverage)) {
                update_patch(patch, window, image, opacity);
                applied = true;
            } else {
                if (!*blur) {
                    *blur = calloc(1, sizeof(**blur));
                    if (*blur) {
                        wl_list_init(&(*blur)->patches);
                    }
                }
                if (*blur) {
                    if (patch) {
                        destroy_patch(patch);
                        patch = NULL;
                    }
                    applied = create_patch(*blur, window, output, &sample, &coverage, image,
                                           opacity) != NULL;
                }
            }
        }
        if (image) {
            wlr_buffer_drop(&image->buffer);
        }
    }
    if (!applied && patch) {
        destroy_patch(patch);
    }
    pixman_region32_fini(&coverage);
    return applied;
}

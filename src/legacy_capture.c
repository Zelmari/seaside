#include "legacy_capture.h"

#include "wlr-screencopy-unstable-v1-server-protocol.h"

#include <drm_fourcc.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

struct seaside_legacy_capture {
    struct wl_global *global;
    struct wl_list clients;
    struct wl_list frames;
    seaside_legacy_output_fn supports;
    seaside_legacy_active_fn active;
    void *data;
};

struct cursor_stamp {
    const struct wlr_texture *texture;
    double x, y;
};

struct legacy_client {
    struct seaside_legacy_capture *capture;
    struct wl_resource *resource;
    struct wl_list link;
    struct wlr_output *previous_output;
    struct wlr_box previous_box;
    struct cursor_stamp cursor;
    uint64_t revision;
    size_t references;
};

struct legacy_frame {
    struct legacy_client *client;
    struct wl_resource *resource;
    struct wl_list link;
    struct wlr_output *output;
    struct wlr_box box;
    struct wlr_buffer *target;
    bool with_cursor;
    bool with_damage;
};

static void client_unref(struct legacy_client *client) {
    if (--client->references == 0) {
        wl_list_remove(&client->link);
        free(client);
    }
}

static void frame_finish(struct legacy_frame *frame) {
    if (frame->target) {
        struct seaside_legacy_capture *capture = frame->client->capture;
        capture->active(capture->data, frame->output, false);
        wlr_buffer_unlock(frame->target);
    }
    wl_list_remove(&frame->link);
    wl_resource_set_user_data(frame->resource, NULL);
    client_unref(frame->client);
    free(frame);
}

static void frame_fail(struct legacy_frame *frame) {
    zwlr_screencopy_frame_v1_send_failed(frame->resource);
    frame_finish(frame);
}

static void request_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static void resource_destroy_frame(struct wl_resource *resource) {
    struct legacy_frame *frame = wl_resource_get_user_data(resource);
    if (frame) {
        frame_finish(frame);
    }
}

static void copy_request(struct wl_resource *resource, struct wl_resource *target,
                         bool with_damage) {
    struct legacy_frame *frame = wl_resource_get_user_data(resource);
    if (!frame || frame->target) {
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_ALREADY_USED,
                               "screencopy frame was already used");
        return;
    }

    struct wlr_buffer *buffer = wlr_buffer_try_from_resource(target);
    struct wlr_shm_attributes attributes;
    if (!buffer || !wlr_buffer_get_shm(buffer, &attributes) ||
        attributes.format != DRM_FORMAT_XRGB8888 || buffer->width != frame->box.width ||
        buffer->height != frame->box.height || attributes.stride != frame->box.width * 4) {
        wlr_buffer_unlock(buffer);
        wl_resource_post_error(resource, ZWLR_SCREENCOPY_FRAME_V1_ERROR_INVALID_BUFFER,
                               "screencopy requires the advertised shared-memory buffer");
        return;
    }
    frame->target = buffer;
    frame->with_damage = with_damage;
    struct seaside_legacy_capture *capture = frame->client->capture;
    capture->active(capture->data, frame->output, true);
}

static void request_copy(struct wl_client *client, struct wl_resource *resource,
                         struct wl_resource *target) {
    (void)client;
    copy_request(resource, target, false);
}

static void request_copy_damage(struct wl_client *client, struct wl_resource *resource,
                                struct wl_resource *target) {
    (void)client;
    copy_request(resource, target, true);
}

static const struct zwlr_screencopy_frame_v1_interface frame_impl = {
    .copy = request_copy,
    .destroy = request_destroy,
    .copy_with_damage = request_copy_damage,
};

static int scaled_coordinate(int64_t coordinate, float scale, int limit) {
    const double scaled = (double)coordinate * scale + 0.5;
    return scaled <= 0.0 ? 0 : scaled >= limit ? limit : (int)scaled;
}

static struct wlr_box capture_box(struct wlr_output *output, const struct wlr_box *region) {
    if (!region) {
        return (struct wlr_box){.width = output->width, .height = output->height};
    }
    if (region->width <= 0 || region->height <= 0) {
        return (struct wlr_box){0};
    }
    int width, height;
    wlr_output_transformed_resolution(output, &width, &height);
    const int left = scaled_coordinate(region->x, output->scale, width);
    const int top = scaled_coordinate(region->y, output->scale, height);
    const int right = scaled_coordinate((int64_t)region->x + region->width, output->scale, width);
    const int bottom =
        scaled_coordinate((int64_t)region->y + region->height, output->scale, height);
    const struct wlr_box box = {.x = left, .y = top, .width = right - left, .height = bottom - top};
    struct wlr_box transformed;
    wlr_box_transform(&transformed, &box, wlr_output_transform_invert(output->transform), width,
                      height);
    return transformed;
}

static void create_frame(struct wl_client *client, struct wl_resource *manager, uint32_t id,
                         bool cursor, struct wl_resource *output_resource,
                         const struct wlr_box *region) {
    struct wl_resource *resource = wl_resource_create(client, &zwlr_screencopy_frame_v1_interface,
                                                      wl_resource_get_version(manager), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &frame_impl, NULL, resource_destroy_frame);
    struct legacy_client *state = wl_resource_get_user_data(manager);
    struct wlr_output *output = wlr_output_from_resource(output_resource);
    if (!state || !output || !state->capture->supports(state->capture->data, output)) {
        zwlr_screencopy_frame_v1_send_failed(resource);
        return;
    }
    const struct wlr_box box = capture_box(output, region);
    if (box.width <= 0 || box.width > INT_MAX / 4 || box.height <= 0) {
        zwlr_screencopy_frame_v1_send_failed(resource);
        return;
    }
    struct legacy_frame *frame = calloc(1, sizeof(*frame));
    if (!frame) {
        wl_client_post_no_memory(client);
        return;
    }
    frame->client = state;
    frame->resource = resource;
    frame->output = output;
    frame->box = box;
    frame->with_cursor = cursor;
    ++state->references;
    wl_list_insert(&state->capture->frames, &frame->link);
    wl_resource_set_user_data(resource, frame);
    zwlr_screencopy_frame_v1_send_buffer(resource, WL_SHM_FORMAT_XRGB8888, box.width, box.height,
                                         box.width * 4);
    if (wl_resource_get_version(resource) >= ZWLR_SCREENCOPY_FRAME_V1_BUFFER_DONE_SINCE_VERSION) {
        zwlr_screencopy_frame_v1_send_buffer_done(resource);
    }
}

static void request_output(struct wl_client *client, struct wl_resource *manager, uint32_t id,
                           int32_t cursor, struct wl_resource *output) {
    create_frame(client, manager, id, cursor != 0, output, NULL);
}

static void request_region(struct wl_client *client, struct wl_resource *manager, uint32_t id,
                           int32_t cursor, struct wl_resource *output, int32_t x, int32_t y,
                           int32_t width, int32_t height) {
    const struct wlr_box region = {.x = x, .y = y, .width = width, .height = height};
    create_frame(client, manager, id, cursor != 0, output, &region);
}

static const struct zwlr_screencopy_manager_v1_interface manager_impl = {
    .capture_output = request_output,
    .capture_output_region = request_region,
    .destroy = request_destroy,
};

static void resource_destroy_manager(struct wl_resource *resource) {
    struct legacy_client *client = wl_resource_get_user_data(resource);
    if (client) {
        client->resource = NULL;
        client_unref(client);
    }
}

static void bind_manager(struct wl_client *client, void *data, uint32_t version, uint32_t id) {
    struct seaside_legacy_capture *capture = data;
    uid_t uid;
    wl_client_get_credentials(client, NULL, &uid, NULL);
    if (uid != geteuid()) {
        wl_client_post_implementation_error(client, "screencopy requires same-user access");
        return;
    }
    struct legacy_client *state = calloc(1, sizeof(*state));
    if (!state) {
        wl_client_post_no_memory(client);
        return;
    }
    state->resource =
        wl_resource_create(client, &zwlr_screencopy_manager_v1_interface, (int)version, id);
    if (!state->resource) {
        free(state);
        wl_client_post_no_memory(client);
        return;
    }
    state->capture = capture;
    state->references = 1;
    wl_list_insert(&capture->clients, &state->link);
    wl_resource_set_implementation(state->resource, &manager_impl, state, resource_destroy_manager);
}

struct seaside_legacy_capture *seaside_legacy_capture_create(struct wl_display *display,
                                                             seaside_legacy_output_fn supports,
                                                             seaside_legacy_active_fn active,
                                                             void *data) {
    struct seaside_legacy_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        return NULL;
    }
    capture->supports = supports;
    capture->active = active;
    capture->data = data;
    wl_list_init(&capture->clients);
    wl_list_init(&capture->frames);
    capture->global =
        wl_global_create(display, &zwlr_screencopy_manager_v1_interface, 3, capture, bind_manager);
    if (!capture->global) {
        free(capture);
        return NULL;
    }
    return capture;
}

void seaside_legacy_capture_destroy(struct seaside_legacy_capture *capture) {
    if (!capture) {
        return;
    }
    struct legacy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &capture->frames, link) {
        frame_fail(frame);
    }
    struct legacy_client *client, *remaining;
    wl_list_for_each_safe(client, remaining, &capture->clients, link) {
        wl_resource_set_user_data(client->resource, NULL);
        client_unref(client);
    }
    wl_global_destroy(capture->global);
    free(capture);
}

void seaside_legacy_capture_output_lost(struct seaside_legacy_capture *capture,
                                        struct wlr_output *output) {
    struct legacy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &capture->frames, link) {
        if (frame->output == output) {
            frame_fail(frame);
        }
    }
    struct legacy_client *client;
    wl_list_for_each(client, &capture->clients, link) {
        if (client->previous_output == output) {
            client->previous_output = NULL;
        }
    }
}

bool seaside_legacy_capture_wants_cursor(const struct seaside_legacy_capture *capture,
                                         struct wlr_output *output) {
    const struct legacy_frame *frame;
    wl_list_for_each(frame, &capture->frames, link) {
        if (frame->output == output && frame->target && frame->with_cursor) {
            return true;
        }
    }
    return false;
}

static struct cursor_stamp current_cursor(const struct legacy_frame *frame) {
    const struct wlr_output_cursor *cursor = frame->output->hardware_cursor;
    if (frame->with_cursor && cursor && cursor->enabled && cursor->visible && cursor->texture) {
        return (struct cursor_stamp){.texture = cursor->texture,
                                     .x = cursor->x - cursor->hotspot_x,
                                     .y = cursor->y - cursor->hotspot_y};
    }
    return (struct cursor_stamp){0};
}

static bool changed(const struct legacy_frame *frame, uint64_t revision,
                    struct cursor_stamp stamp) {
    const struct legacy_client *state = frame->client;
    const struct wlr_box *a = &state->previous_box;
    const struct wlr_box *b = &frame->box;
    return state->previous_output != frame->output || state->revision != revision || a->x != b->x ||
           a->y != b->y || a->width != b->width || a->height != b->height ||
           state->cursor.texture != stamp.texture || state->cursor.x != stamp.x ||
           state->cursor.y != stamp.y;
}

static bool read_pixels(const struct legacy_frame *frame, struct wlr_buffer *buffer) {
    struct wlr_texture *texture = wlr_texture_from_buffer(frame->output->renderer, buffer);
    if (!texture) {
        return false;
    }
    void *pixels;
    uint32_t format;
    size_t stride;
    bool copied = false;
    if (wlr_buffer_begin_data_ptr_access(frame->target, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &pixels,
                                         &format, &stride)) {
        if (format == DRM_FORMAT_XRGB8888 && stride == (size_t)frame->box.width * 4) {
            const struct wlr_texture_read_pixels_options options = {
                .data = pixels,
                .format = format,
                .stride = stride,
                .src_box = frame->box,
            };
            copied = wlr_texture_read_pixels(texture, &options);
        }
        wlr_buffer_end_data_ptr_access(frame->target);
    }
    wlr_texture_destroy(texture);
    return copied;
}

void seaside_legacy_capture_present(struct seaside_legacy_capture *capture,
                                    struct wlr_output *output, struct wlr_buffer *buffer,
                                    struct wlr_buffer *cursor_buffer, const struct timespec *when,
                                    uint64_t content_revision) {
    struct legacy_frame *frame, *next;
    wl_list_for_each_safe(frame, next, &capture->frames, link) {
        if (frame->output != output || !frame->target) {
            continue;
        }
        const struct cursor_stamp stamp = current_cursor(frame);
        if (frame->with_damage && !changed(frame, content_revision, stamp)) {
            continue;
        }
        struct wlr_buffer *source = frame->with_cursor ? cursor_buffer : buffer;
        if (!source || !read_pixels(frame, source)) {
            frame_fail(frame);
            continue;
        }
        struct legacy_client *state = frame->client;
        state->previous_output = output;
        state->previous_box = frame->box;
        state->cursor = stamp;
        state->revision = content_revision;
        zwlr_screencopy_frame_v1_send_flags(frame->resource, 0);
        if (frame->with_damage) {
            zwlr_screencopy_frame_v1_send_damage(frame->resource, 0, 0, frame->box.width,
                                                 frame->box.height);
        }
        const uint64_t seconds = when->tv_sec;
        zwlr_screencopy_frame_v1_send_ready(frame->resource, seconds >> 32, seconds, when->tv_nsec);
        frame_finish(frame);
    }
}

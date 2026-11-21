#include "part3_bridge.h"

#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>

enum { OUTPUT_WIDTH = 160, OUTPUT_HEIGHT = 90, CLIENT_WIDTH = 16, CLIENT_HEIGHT = 8 };
static const uint32_t CLIENT_RGB = 0x335577;

struct part3_output {
    struct wlr_output *output;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct part3_output_snapshot snapshot;
    struct part3_output_pixel sample;
    unsigned sample_x, sample_y;
    bool sample_pixel;
    bool listening;
    bool render_locked;
};

static void output_commit(struct wl_listener *listener, void *data) {
    struct part3_output *observer = wl_container_of(listener, observer, commit);
    const struct wlr_output_event_commit *event = data;
    if (!(event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
        return;
    }
    void *pixels;
    uint32_t format;
    size_t stride;
    struct wlr_buffer *buffer = event->state->buffer;
    if (!buffer || !wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                                     &pixels, &format, &stride)) {
        observer->snapshot.invalid_frames++;
        return;
    }
    bool black = true;
    bool client = true;
    if ((format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) ||
        buffer->width != OUTPUT_WIDTH || buffer->height != OUTPUT_HEIGHT ||
        stride < OUTPUT_WIDTH * sizeof(uint32_t)) {
        black = false;
        client = false;
    } else {
        for (unsigned y = 0; y < OUTPUT_HEIGHT; y++) {
            for (unsigned x = 0; x < OUTPUT_WIDTH; x++) {
                uint32_t pixel;
                memcpy(&pixel, (const unsigned char *)pixels + y * stride + x * sizeof(pixel),
                       sizeof(pixel));
                pixel &= 0xFFFFFF;
                if (observer->sample_pixel && x == observer->sample_x && y == observer->sample_y) {
                    observer->sample.frames++;
                    observer->sample.rgb = pixel;
                }
                black = black && pixel == 0;
                const uint32_t expected = x < CLIENT_WIDTH && y < CLIENT_HEIGHT ? CLIENT_RGB : 0;
                client = client && pixel == expected;
            }
        }
    }
    wlr_buffer_end_data_ptr_access(buffer);
    if (black) {
        observer->snapshot.black_frames++;
    } else if (client) {
        observer->snapshot.client_frames++;
    } else {
        observer->snapshot.invalid_frames++;
    }
}

static void output_destroyed(struct wl_listener *listener, void *data) {
    (void)data;
    struct part3_output *observer = wl_container_of(listener, observer, destroy);
    if (observer->render_locked) {
        wlr_output_lock_attach_render(observer->output, false);
        observer->render_locked = false;
    }
    wl_list_remove(&observer->commit.link);
    wl_list_remove(&observer->destroy.link);
    observer->listening = false;
    observer->output = NULL;
}

struct wlr_backend *part3_headless_backend_create(struct wl_display *display) {
    return wlr_headless_backend_create(wl_display_get_event_loop(display));
}

struct wlr_renderer *part3_pixman_renderer_create(void) {
    return wlr_pixman_renderer_create();
}

struct part3_output *part3_output_create(struct wl_display *display, struct wlr_backend *backend,
                                         struct wlr_allocator *allocator,
                                         struct wlr_renderer *renderer) {
    struct part3_output *observer = calloc(1, sizeof(*observer));
    if (!observer) {
        return NULL;
    }
    observer->output = wlr_headless_add_output(backend, OUTPUT_WIDTH, OUTPUT_HEIGHT);
    if (!observer->output || !wlr_output_init_render(observer->output, allocator, renderer)) {
        part3_output_destroy(observer);
        return NULL;
    }
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, OUTPUT_WIDTH, OUTPUT_HEIGHT, 60000);
    bool committed = wlr_output_commit_state(observer->output, &state);
    wlr_output_state_finish(&state);
    if (!committed) {
        part3_output_destroy(observer);
        return NULL;
    }
    wlr_output_create_global(observer->output, display);
    observer->commit.notify = output_commit;
    observer->destroy.notify = output_destroyed;
    wl_signal_add(&observer->output->events.commit, &observer->commit);
    wl_signal_add(&observer->output->events.destroy, &observer->destroy);
    observer->listening = true;
    /* Pixel assertions require the actual composed output, not a client-sized
     * direct-scanout buffer plus backend destination metadata. This lock is
     * test-only and does not select production rendering or cursor policy. */
    wlr_output_lock_attach_render(observer->output, true);
    observer->render_locked = true;
    return observer;
}

struct wlr_output *part3_output_handle(struct part3_output *output) {
    return output ? output->output : NULL;
}

struct part3_output_snapshot part3_output_get_snapshot(struct part3_output *output) {
    return output ? output->snapshot : (struct part3_output_snapshot){0};
}

bool part3_output_sample_pixel(struct part3_output *output, unsigned x, unsigned y) {
    if (!output || !output->output || x >= OUTPUT_WIDTH || y >= OUTPUT_HEIGHT) {
        return false;
    }
    output->sample_x = x;
    output->sample_y = y;
    output->sample = (struct part3_output_pixel){0};
    output->sample_pixel = true;
    wlr_output_schedule_frame(output->output);
    return true;
}

struct part3_output_pixel part3_output_get_pixel(struct part3_output *output) {
    return output ? output->sample : (struct part3_output_pixel){0};
}

void part3_output_destroy(struct part3_output *output) {
    if (!output) {
        return;
    }
    if (output->output) {
        wlr_output_destroy(output->output);
    }
    free(output);
}

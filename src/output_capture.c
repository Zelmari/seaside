#include "output_capture.h"

#include "ext-image-capture-source-v1-server-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/box.h>
#include <wlr/util/transform.h>

struct seaside_output_capture {
    struct wl_display *display;
    struct wl_global *output_global;
    struct wlr_ext_image_copy_capture_manager_v1 *copy_manager;
    struct wlr_output_layout *layout;
    struct wlr_output *output;
    struct wlr_ext_image_capture_source_v1 source;
    struct wl_list output_resources;
    struct wl_list sessions;
    struct wl_listener display_destroy;
    struct wl_listener new_session;
    struct wl_listener output_destroy;
    struct wl_listener output_commit;
    struct wl_listener output_present;
    struct wlr_buffer *pending_buffer;
    struct wlr_buffer *cursor_composite;
    uint32_t pending_sequence;
    size_t active_sessions;
    bool creating_with_cursor;
    bool constraints_ready;
};

struct capture_session {
    struct wlr_ext_image_copy_capture_session_v1 *session;
    struct wl_list link;
    struct wl_listener destroy;
    bool with_cursor;
};

struct capture_frame {
    struct wlr_ext_image_capture_source_v1_frame_event base;
    struct wlr_buffer *buffer;
    struct timespec presented;
    bool cursor_composited;
};

static void release_pending(struct seaside_output_capture *capture) {
    wlr_buffer_unlock(capture->pending_buffer);
    capture->pending_buffer = NULL;
}

static void release_composite(struct seaside_output_capture *capture) {
    wlr_buffer_drop(capture->cursor_composite);
    capture->cursor_composite = NULL;
}

static void start_capture(struct wlr_ext_image_capture_source_v1 *source, bool with_cursor) {
    struct seaside_output_capture *capture = wl_container_of(source, capture, source);
    /* wlroots emits new_session synchronously after start, so its listener
     * can record the cursor option on the new session rather than the output. */
    capture->creating_with_cursor = with_cursor;
    if (capture->active_sessions++ == 0) {
        wlr_output_lock_attach_render(capture->output, true);
    }
}

static void stop_capture(struct wlr_ext_image_capture_source_v1 *source) {
    struct seaside_output_capture *capture = wl_container_of(source, capture, source);
    if (--capture->active_sessions == 0) {
        wlr_output_lock_attach_render(capture->output, false);
        release_pending(capture);
        release_composite(capture);
    }
}

static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool needs_frame) {
    struct seaside_output_capture *capture = wl_container_of(source, capture, source);
    if (needs_frame && capture->output->enabled) {
        wlr_output_schedule_frame(capture->output);
    }
}

static void on_session_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct capture_session *session = wl_container_of(listener, session, destroy);
    wl_list_remove(&session->destroy.link);
    wl_list_remove(&session->link);
    free(session);
}

static void on_new_session(struct wl_listener *listener, void *data) {
    struct seaside_output_capture *capture = wl_container_of(listener, capture, new_session);
    struct wlr_ext_image_copy_capture_session_v1 *session = data;
    if (session->source != &capture->source) {
        return;
    }
    struct capture_session *state = calloc(1, sizeof(*state));
    if (!state) {
        wl_client_post_no_memory(wl_resource_get_client(session->resource));
        return;
    }
    state->session = session;
    state->with_cursor = capture->creating_with_cursor;
    state->destroy.notify = on_session_destroy;
    wl_signal_add(&session->events.destroy, &state->destroy);
    wl_list_insert(&capture->sessions, &state->link);
}

static bool composite_cursor(struct seaside_output_capture *capture, struct capture_frame *frame,
                             const struct wlr_output_cursor *cursor) {
    if (frame->cursor_composited) {
        return true;
    }
    struct wlr_output *output = capture->output;
    struct wlr_renderer *renderer = output->renderer;
    if (!capture->cursor_composite) {
        capture->cursor_composite =
            wlr_allocator_create_buffer(output->allocator, frame->buffer->width,
                                        frame->buffer->height, &output->swapchain->format);
        if (!capture->cursor_composite) {
            return false;
        }
    }
    struct wlr_texture *background = wlr_texture_from_buffer(renderer, frame->buffer);
    if (!background) {
        return false;
    }
    struct wlr_render_pass *pass =
        wlr_renderer_begin_buffer_pass(renderer, capture->cursor_composite, NULL);
    if (!pass) {
        wlr_texture_destroy(background);
        return false;
    }

    wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                                          .texture = background,
                                          .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                                      });

    int width, height;
    wlr_output_transformed_resolution(output, &width, &height);
    struct wlr_box box = {
        .x = (int)(cursor->x - cursor->hotspot_x),
        .y = (int)(cursor->y - cursor->hotspot_y),
        .width = (int)cursor->width,
        .height = (int)cursor->height,
    };
    wlr_box_transform(&box, &box, wlr_output_transform_invert(output->transform), width, height);
    /* Render the hardware sprite into a separate capture buffer. The scanout
     * buffer and the display's hardware plane are untouched by the stream. */
    wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
                                          .texture = cursor->texture,
                                          .src_box = cursor->src_box,
                                          .dst_box = box,
                                          .transform = output->transform,
                                      });
    frame->cursor_composited = wlr_render_pass_submit(pass);
    wlr_texture_destroy(background);
    return frame->cursor_composited;
}

static void copy_frame(struct wlr_ext_image_capture_source_v1 *source,
                       struct wlr_ext_image_copy_capture_frame_v1 *client_frame,
                       struct wlr_ext_image_capture_source_v1_frame_event *event) {
    struct seaside_output_capture *capture = wl_container_of(source, capture, source);
    struct capture_frame *frame = wl_container_of(event, frame, base);
    struct wlr_output *output = capture->output;
    struct capture_session *state;
    wl_list_for_each(state, &capture->sessions, link) {
        if (state->session->frame != client_frame) {
            continue;
        }
        struct wlr_buffer *buffer = frame->buffer;
        const struct wlr_output_cursor *cursor = output->hardware_cursor;
        if (state->with_cursor && cursor && cursor->enabled && cursor->visible && cursor->texture) {
            if (!composite_cursor(capture, frame, cursor)) {
                break;
            }
            buffer = capture->cursor_composite;
        }
        if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(client_frame, buffer,
                                                            output->renderer)) {
            wlr_ext_image_copy_capture_frame_v1_ready(client_frame, output->transform,
                                                      &frame->presented);
        }
        return;
    }
    wlr_ext_image_copy_capture_frame_v1_fail(
        client_frame, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
}

static const struct wlr_ext_image_capture_source_v1_interface capture_impl = {
    .start = start_capture,
    .stop = stop_capture,
    .request_frame = request_frame,
    .copy_frame = copy_frame,
};

static bool update_constraints(struct seaside_output_capture *capture) {
    struct wlr_output *output = capture->output;
    return wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain) &&
           wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(
               &capture->source, output->swapchain, output->renderer) &&
           (capture->source.shm_formats_len || capture->source.dmabuf_formats.len);
}

static void on_output_commit(struct wl_listener *listener, void *data) {
    struct seaside_output_capture *capture = wl_container_of(listener, capture, output_commit);
    const struct wlr_output_event_commit *event = data;
    if (!capture->output->enabled) {
        release_pending(capture);
        return;
    }
    if (event->state->committed &
        (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_ENABLED)) {
        release_pending(capture);
        release_composite(capture);
        capture->constraints_ready = update_constraints(capture);
        if (!capture->constraints_ready) {
            /* A session cannot recover with dimensions or formats we can no
             * longer deliver. Its client may open a new one once they return. */
            wlr_ext_image_capture_source_v1_finish(&capture->source);
            wlr_ext_image_capture_source_v1_init(&capture->source, &capture_impl);
        }
    }
    if (capture->active_sessions && (event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
        release_pending(capture);
        capture->pending_buffer = wlr_buffer_lock(event->state->buffer);
        capture->pending_sequence = capture->output->commit_seq;
    }
}

static void on_output_present(struct wl_listener *listener, void *data) {
    struct seaside_output_capture *capture = wl_container_of(listener, capture, output_present);
    const struct wlr_output_event_present *event = data;
    if (!capture->pending_buffer || capture->pending_sequence != event->commit_seq) {
        return;
    }
    struct wlr_buffer *buffer = capture->pending_buffer;
    capture->pending_buffer = NULL;
    if (event->presented && capture->output->enabled) {
        /* Wait for presentation because the primary buffer can have an
         * explicit render fence at commit time. The capture clients share
         * conservative full damage, including cursor-only changes. */
        pixman_region32_t damage;
        pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
        struct capture_frame frame = {
            .base = {.damage = &damage},
            .buffer = buffer,
            .presented = event->when,
        };
        wl_signal_emit_mutable(&capture->source.events.frame, &frame);
        pixman_region32_fini(&damage);
    } else if (capture->output->enabled) {
        wlr_output_schedule_frame(capture->output);
    }
    wlr_buffer_unlock(buffer);
}

static void detach_output(struct seaside_output_capture *capture) {
    if (!capture->output) {
        return;
    }
    /* Finishing the source invalidates old descriptors and stops sessions.
     * Its stop callback releases the render lock while the output exists. */
    wlr_ext_image_capture_source_v1_finish(&capture->source);
    release_pending(capture);
    release_composite(capture);
    wl_list_remove(&capture->output_destroy.link);
    wl_list_remove(&capture->output_commit.link);
    wl_list_remove(&capture->output_present.link);
    wlr_output_layout_remove(capture->layout, capture->output);
    capture->output = NULL;
}

static void on_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_output_capture *capture = wl_container_of(listener, capture, output_destroy);
    detach_output(capture);
}

void seaside_output_capture_clear_output(struct seaside_output_capture *capture) {
    if (capture) {
        detach_output(capture);
    }
}

bool seaside_output_capture_set_output(struct seaside_output_capture *capture,
                                       struct wlr_output *output) {
    if (!capture || !capture->display || !capture->layout || !output || !output->enabled ||
        !output->renderer || !output->allocator) {
        return false;
    }
    if (capture->output) {
        return capture->output == output;
    }
    capture->output = output;
    wlr_ext_image_capture_source_v1_init(&capture->source, &capture_impl);
    wl_signal_add(&output->events.destroy, &capture->output_destroy);
    wl_signal_add(&output->events.present, &capture->output_present);
    wl_signal_add(&output->events.commit, &capture->output_commit);
    capture->constraints_ready = update_constraints(capture);
    if (!capture->constraints_ready || !wlr_output_layout_add(capture->layout, output, 0, 0) ||
        !output->global) {
        detach_output(capture);
        return false;
    }
    return true;
}

bool seaside_output_capture_position(struct seaside_output_capture *capture, int x, int y) {
    return capture && capture->output &&
           wlr_output_layout_add(capture->layout, capture->output, x, y);
}

static void handle_create_source(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, struct wl_resource *output_resource) {
    struct seaside_output_capture *capture = wl_resource_get_user_data(resource);
    struct wlr_ext_image_capture_source_v1 *source = NULL;
    if (capture && capture->output && capture->constraints_ready && capture->output->enabled &&
        wlr_output_from_resource(output_resource) == capture->output) {
        source = &capture->source;
    }
    (void)wlr_ext_image_capture_source_v1_create_resource(source, client, id);
}

static void handle_destroy(struct wl_client *client, struct wl_resource *resource) {
    (void)client;
    wl_resource_destroy(resource);
}

static const struct ext_output_image_capture_source_manager_v1_interface output_manager_impl = {
    .create_source = handle_create_source,
    .destroy = handle_destroy,
};

static void on_output_resource_destroy(struct wl_resource *resource) {
    wl_list_remove(wl_resource_get_link(resource));
}

static void bind_output_manager(struct wl_client *client, void *data, uint32_t version,
                                uint32_t id) {
    struct seaside_output_capture *capture = data;
    struct wl_resource *resource = wl_resource_create(
        client, &ext_output_image_capture_source_manager_v1_interface, (int)version, id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &output_manager_impl, capture,
                                   on_output_resource_destroy);
    wl_list_insert(&capture->output_resources, wl_resource_get_link(resource));
}

static void detach_display(struct seaside_output_capture *capture) {
    if (!capture->display) {
        return;
    }
    detach_output(capture);
    wl_list_remove(&capture->display_destroy.link);
    if (capture->copy_manager) {
        wl_list_remove(&capture->new_session.link);
        capture->copy_manager = NULL;
    }
    if (capture->output_global) {
        wl_global_destroy(capture->output_global);
        capture->output_global = NULL;
    }
    struct wl_resource *resource, *tmp;
    wl_resource_for_each_safe(resource, tmp, &capture->output_resources) {
        wl_resource_set_user_data(resource, NULL);
        wl_list_remove(wl_resource_get_link(resource));
        wl_list_init(wl_resource_get_link(resource));
    }
    capture->display = NULL;
}

static void on_display_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_output_capture *capture = wl_container_of(listener, capture, display_destroy);
    detach_display(capture);
}

struct seaside_output_capture *seaside_output_capture_create(struct wl_display *display,
                                                             struct wlr_output *output) {
    if (!display) {
        return NULL;
    }
    struct seaside_output_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        return NULL;
    }
    capture->display = display;
    capture->display_destroy.notify = on_display_destroy;
    capture->new_session.notify = on_new_session;
    capture->output_destroy.notify = on_output_destroy;
    capture->output_commit.notify = on_output_commit;
    capture->output_present.notify = on_output_present;
    wl_list_init(&capture->output_resources);
    wl_list_init(&capture->sessions);
    /* Remove the new_session listener before wlroots destroys its manager. */
    wl_display_add_destroy_listener(display, &capture->display_destroy);
    /* The layout and xdg-output manager share the display lifetime. Keeping
     * them alive through output removal also keeps existing client bindings. */
    capture->layout = wlr_output_layout_create(display);
    if (!capture->layout || !wlr_xdg_output_manager_v1_create(display, capture->layout)) {
        wlr_output_layout_destroy(capture->layout);
        seaside_output_capture_destroy(capture);
        return NULL;
    }
    if (!seaside_output_capture_set_output(capture, output)) {
        seaside_output_capture_destroy(capture);
        return NULL;
    }
    capture->output_global =
        wl_global_create(display, &ext_output_image_capture_source_manager_v1_interface, 1, capture,
                         bind_output_manager);
    if (!capture->output_global) {
        seaside_output_capture_destroy(capture);
        return NULL;
    }
    capture->copy_manager = wlr_ext_image_copy_capture_manager_v1_create(display, 1);
    if (!capture->copy_manager) {
        seaside_output_capture_destroy(capture);
        return NULL;
    }
    wl_signal_add(&capture->copy_manager->events.new_session, &capture->new_session);
    return capture;
}

void seaside_output_capture_destroy(struct seaside_output_capture *capture) {
    if (capture) {
        detach_display(capture);
        free(capture);
    }
}

struct client_bindings {
    const struct seaside_output_capture *capture;
    bool shm;
    bool output;
    bool output_manager;
    bool copy_manager;
};

static enum wl_iterator_result check_resource(struct wl_resource *resource, void *data) {
    struct client_bindings *bindings = data;
    const char *name = wl_resource_get_class(resource);
    if (strcmp(name, wl_shm_interface.name) == 0) {
        bindings->shm = true;
    } else if (strcmp(name, wl_output_interface.name) == 0) {
        bindings->output |= wlr_output_from_resource(resource) == bindings->capture->output;
    } else if (wl_resource_instance_of(resource,
                                       &ext_output_image_capture_source_manager_v1_interface,
                                       &output_manager_impl)) {
        bindings->output_manager |= wl_resource_get_user_data(resource) == bindings->capture;
    } else if (strcmp(name, "ext_image_copy_capture_manager_v1") == 0) {
        bindings->copy_manager |=
            wl_resource_get_user_data(resource) == bindings->capture->copy_manager;
    }
    return bindings->shm && bindings->output && bindings->output_manager && bindings->copy_manager
               ? WL_ITERATOR_STOP
               : WL_ITERATOR_CONTINUE;
}

bool seaside_output_capture_client_ready(const struct seaside_output_capture *capture, pid_t pid,
                                         uid_t uid) {
    if (!capture || !capture->display || !capture->output || !capture->constraints_ready ||
        !capture->output->enabled || pid <= 0) {
        return false;
    }
    struct wl_client *client;
    wl_client_for_each(client, wl_display_get_client_list(capture->display)) {
        pid_t client_pid;
        uid_t client_uid;
        wl_client_get_credentials(client, &client_pid, &client_uid, NULL);
        if (client_pid == pid && client_uid == uid) {
            struct client_bindings bindings = {.capture = capture};
            wl_client_for_each_resource(client, check_resource, &bindings);
            if (bindings.shm && bindings.output && bindings.output_manager &&
                bindings.copy_manager) {
                return true;
            }
        }
    }
    return false;
}

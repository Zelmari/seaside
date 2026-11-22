#include "output_capture.h"
#include "legacy_capture.h"

#include "ext-image-capture-source-v1-server-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    struct seaside_legacy_capture *legacy;
    struct wlr_output_layout *layout;
    struct wl_list output_resources;
    struct wl_list outputs;
    struct wl_listener display_destroy;
    struct wl_listener new_session;
};

struct capture_output {
    struct seaside_output_capture *owner;
    struct wlr_output *output;
    struct wlr_ext_image_capture_source_v1 source;
    struct wl_list link;
    struct wl_list sessions;
    struct wl_listener output_destroy;
    struct wl_listener output_commit;
    struct wl_listener output_present;
    struct wlr_buffer *pending_buffer;
    struct wlr_buffer *cursor_composite;
    uint32_t pending_sequence;
    uint64_t content_revision;
    size_t active_sessions;
    size_t legacy_frames;
    bool creating_with_cursor;
    bool constraints_ready;
};

static const struct wlr_ext_image_capture_source_v1_interface capture_impl;

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

static void release_pending(struct capture_output *capture) {
    wlr_buffer_unlock(capture->pending_buffer);
    capture->pending_buffer = NULL;
}

static void release_composite(struct capture_output *capture) {
    wlr_buffer_drop(capture->cursor_composite);
    capture->cursor_composite = NULL;
}

static bool active(const struct capture_output *capture) {
    return capture->active_sessions || capture->legacy_frames;
}

static void update_render_lock(struct capture_output *capture, bool was_active) {
    if (active(capture) != was_active) {
        wlr_output_lock_attach_render(capture->output, !was_active);
        if (was_active) {
            release_pending(capture);
            release_composite(capture);
        }
    }
}

static void start_capture(struct wlr_ext_image_capture_source_v1 *source, bool with_cursor) {
    struct capture_output *capture = wl_container_of(source, capture, source);
    /* wlroots emits new_session synchronously after start, so its listener
     * can record the cursor option on the new session rather than the output. */
    capture->creating_with_cursor = with_cursor;
    bool was_active = active(capture);
    capture->active_sessions++;
    update_render_lock(capture, was_active);
}

static void stop_capture(struct wlr_ext_image_capture_source_v1 *source) {
    struct capture_output *capture = wl_container_of(source, capture, source);
    capture->active_sessions--;
    update_render_lock(capture, true);
}

static void request_frame(struct wlr_ext_image_capture_source_v1 *source, bool needs_frame) {
    struct capture_output *capture = wl_container_of(source, capture, source);
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
    (void)listener;
    struct wlr_ext_image_copy_capture_session_v1 *session = data;
    if (session->source->impl != &capture_impl) {
        return;
    }
    struct capture_output *capture = wl_container_of(session->source, capture, source);
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

static bool composite_cursor(struct capture_output *capture, struct capture_frame *frame,
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
    struct capture_output *capture = wl_container_of(source, capture, source);
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

static bool update_constraints(struct capture_output *capture) {
    struct wlr_output *output = capture->output;
    return wlr_output_configure_primary_swapchain(output, NULL, &output->swapchain) &&
           wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(
               &capture->source, output->swapchain, output->renderer) &&
           (capture->source.shm_formats_len || capture->source.dmabuf_formats.len);
}

static void on_output_commit(struct wl_listener *listener, void *data) {
    struct capture_output *capture = wl_container_of(listener, capture, output_commit);
    const struct wlr_output_event_commit *event = data;
    const uint32_t flags = event->state->committed;
    if (!capture->output->enabled ||
        (flags & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_RENDER_FORMAT | WLR_OUTPUT_STATE_SCALE |
                  WLR_OUTPUT_STATE_TRANSFORM))) {
        seaside_legacy_capture_output_lost(capture->owner->legacy, capture->output);
    }
    if (!capture->output->enabled) {
        release_pending(capture);
        release_composite(capture);
        if (capture->constraints_ready) {
            wlr_ext_image_capture_source_v1_finish(&capture->source);
            wlr_ext_image_capture_source_v1_init(&capture->source, &capture_impl);
            capture->constraints_ready = false;
        }
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
    if ((flags & WLR_OUTPUT_STATE_BUFFER) &&
        (!(flags & WLR_OUTPUT_STATE_DAMAGE) || pixman_region32_not_empty(&event->state->damage))) {
        capture->content_revision++;
    }
    if (active(capture) && (flags & WLR_OUTPUT_STATE_BUFFER)) {
        release_pending(capture);
        capture->pending_buffer = wlr_buffer_lock(event->state->buffer);
        capture->pending_sequence = capture->output->commit_seq;
    }
}

static void on_output_present(struct wl_listener *listener, void *data) {
    struct capture_output *capture = wl_container_of(listener, capture, output_present);
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
        if (capture->legacy_frames) {
            struct wlr_buffer *with_cursor = buffer;
            const struct wlr_output_cursor *cursor = capture->output->hardware_cursor;
            if (cursor && cursor->enabled && cursor->visible && cursor->texture &&
                seaside_legacy_capture_wants_cursor(capture->owner->legacy, capture->output)) {
                with_cursor =
                    composite_cursor(capture, &frame, cursor) ? capture->cursor_composite : NULL;
            }
            seaside_legacy_capture_present(capture->owner->legacy, capture->output, buffer,
                                           with_cursor, &event->when, capture->content_revision);
        }
    } else if (capture->output->enabled) {
        wlr_output_schedule_frame(capture->output);
    }
    wlr_buffer_unlock(buffer);
}

static struct capture_output *find_output(const struct seaside_output_capture *capture,
                                          const struct wlr_output *output) {
    if (capture && output) {
        struct capture_output *candidate;
        wl_list_for_each(candidate, &capture->outputs, link) {
            if (candidate->output == output) {
                return candidate;
            }
        }
    }
    return NULL;
}

static bool legacy_supports(void *data, struct wlr_output *output) {
    struct capture_output *target = find_output(data, output);
    return target && output->enabled && target->constraints_ready;
}

static void legacy_active(void *data, struct wlr_output *output, bool start) {
    struct capture_output *target = find_output(data, output);
    if (!target) {
        return;
    }
    const bool was_active = active(target);
    if (start) {
        target->legacy_frames++;
        wlr_output_schedule_frame(output);
    } else {
        target->legacy_frames--;
    }
    update_render_lock(target, was_active);
}

void seaside_output_capture_cursor_changed(struct seaside_output_capture *capture) {
    if (!capture) {
        return;
    }
    struct capture_output *target;
    wl_list_for_each(target, &capture->outputs, link) {
        bool wants_cursor = seaside_legacy_capture_wants_cursor(capture->legacy, target->output);
        const struct capture_session *session;
        wl_list_for_each(session, &target->sessions, link) {
            wants_cursor |= session->with_cursor && session->session->frame &&
                            session->session->frame->capturing;
        }
        if (wants_cursor && target->output->enabled) {
            wlr_output_schedule_frame(target->output);
        }
    }
}

static void detach_output(struct capture_output *capture) {
    /* Finishing the source invalidates descriptors and stops sessions while
     * the output and its render lock are still valid. */
    wlr_ext_image_capture_source_v1_finish(&capture->source);
    seaside_legacy_capture_output_lost(capture->owner->legacy, capture->output);
    release_pending(capture);
    release_composite(capture);
    wl_list_remove(&capture->output_destroy.link);
    wl_list_remove(&capture->output_commit.link);
    wl_list_remove(&capture->output_present.link);
    wl_list_remove(&capture->link);
    wlr_output_layout_remove(capture->owner->layout, capture->output);
    free(capture);
}

static void on_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct capture_output *capture = wl_container_of(listener, capture, output_destroy);
    detach_output(capture);
}

void seaside_output_capture_remove_output(struct seaside_output_capture *capture,
                                          struct wlr_output *output) {
    struct capture_output *target = find_output(capture, output);
    if (target) {
        detach_output(target);
    }
}

bool seaside_output_capture_add_output(struct seaside_output_capture *capture,
                                       struct wlr_output *output, int x, int y) {
    if (!capture || !capture->display || !output || !output->enabled || !output->renderer ||
        !output->allocator) {
        return false;
    }
    if (find_output(capture, output)) {
        return seaside_output_capture_position(capture, output, x, y);
    }
    struct capture_output *target = calloc(1, sizeof(*target));
    if (!target) {
        return false;
    }
    target->owner = capture;
    target->output = output;
    target->output_destroy.notify = on_output_destroy;
    target->output_commit.notify = on_output_commit;
    target->output_present.notify = on_output_present;
    wl_list_init(&target->sessions);
    wl_list_insert(&capture->outputs, &target->link);
    wlr_ext_image_capture_source_v1_init(&target->source, &capture_impl);
    wl_signal_add(&output->events.destroy, &target->output_destroy);
    wl_signal_add(&output->events.present, &target->output_present);
    wl_signal_add(&output->events.commit, &target->output_commit);
    target->constraints_ready = update_constraints(target);
    if (!target->constraints_ready || !wlr_output_layout_add(capture->layout, output, x, y) ||
        !output->global) {
        detach_output(target);
        return false;
    }
    return true;
}

bool seaside_output_capture_position(struct seaside_output_capture *capture,
                                     struct wlr_output *output, int x, int y) {
    return find_output(capture, output) && wlr_output_layout_add(capture->layout, output, x, y);
}

static void handle_create_source(struct wl_client *client, struct wl_resource *resource,
                                 uint32_t id, struct wl_resource *output_resource) {
    struct seaside_output_capture *capture = wl_resource_get_user_data(resource);
    struct capture_output *target = find_output(capture, wlr_output_from_resource(output_resource));
    struct wlr_ext_image_capture_source_v1 *source = NULL;
    if (target && target->constraints_ready && target->output->enabled) {
        source = &target->source;
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
    struct capture_output *output, *next;
    wl_list_for_each_safe(output, next, &capture->outputs, link) {
        detach_output(output);
    }
    seaside_legacy_capture_destroy(capture->legacy);
    capture->legacy = NULL;
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

static bool capture_global_visible(const struct wl_client *client, const struct wl_global *global,
                                   void *data) {
    (void)data;
    const char *interface = wl_global_get_interface(global)->name;
    if (strcmp(interface, "ext_image_copy_capture_manager_v1") != 0 &&
        strcmp(interface, "ext_output_image_capture_source_manager_v1") != 0 &&
        strcmp(interface, "ext_foreign_toplevel_image_capture_source_manager_v1") != 0 &&
        strcmp(interface, "ext_foreign_toplevel_list_v1") != 0 &&
        strcmp(interface, "zwlr_screencopy_manager_v1") != 0) {
        return true;
    }
    uid_t uid;
    wl_client_get_credentials(client, NULL, &uid, NULL);
    return uid == geteuid();
}

struct seaside_output_capture *seaside_output_capture_create(struct wl_display *display) {
    if (!display) {
        return NULL;
    }
    struct seaside_output_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        return NULL;
    }
    capture->display = display;
    // Local Wayland clients can capture directly within the same-user trust
    // boundary. The external portal mediates consent outside that boundary.
    wl_display_set_global_filter(display, capture_global_visible, NULL);
    capture->display_destroy.notify = on_display_destroy;
    capture->new_session.notify = on_new_session;
    wl_list_init(&capture->output_resources);
    wl_list_init(&capture->outputs);
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
    capture->legacy =
        seaside_legacy_capture_create(display, legacy_supports, legacy_active, capture);
    if (!capture->legacy) {
        seaside_output_capture_destroy(capture);
        return NULL;
    }
    return capture;
}

void seaside_output_capture_destroy(struct seaside_output_capture *capture) {
    if (capture) {
        detach_display(capture);
        free(capture);
    }
}

struct wlr_ext_image_copy_capture_manager_v1 *
seaside_output_capture_copy_manager(const struct seaside_output_capture *capture) {
    return capture ? capture->copy_manager : NULL;
}

struct client_bindings {
    const struct seaside_output_capture *capture;
    bool shm;
    bool awaiting_output;
    bool output_manager;
    bool copy_manager;
};

static enum wl_iterator_result check_resource(struct wl_resource *resource, void *data) {
    struct client_bindings *bindings = data;
    const char *name = wl_resource_get_class(resource);
    if (strcmp(name, wl_shm_interface.name) == 0) {
        bindings->shm = true;
    } else if (strcmp(name, wl_output_interface.name) == 0) {
        struct capture_output *target =
            find_output(bindings->capture, wlr_output_from_resource(resource));
        if (target && target->constraints_ready && target->output->enabled) {
            bindings->awaiting_output = false;
        }
    } else if (wl_resource_instance_of(resource,
                                       &ext_output_image_capture_source_manager_v1_interface,
                                       &output_manager_impl)) {
        bindings->output_manager |= wl_resource_get_user_data(resource) == bindings->capture;
    } else if (strcmp(name, "ext_image_copy_capture_manager_v1") == 0) {
        bindings->copy_manager |=
            wl_resource_get_user_data(resource) == bindings->capture->copy_manager;
    }
    return bindings->shm && !bindings->awaiting_output && bindings->output_manager &&
                   bindings->copy_manager
               ? WL_ITERATOR_STOP
               : WL_ITERATOR_CONTINUE;
}

bool seaside_output_capture_client_ready(const struct seaside_output_capture *capture, pid_t pid,
                                         uid_t uid) {
    if (!capture || !capture->display || pid <= 0) {
        return false;
    }
    // The protocol managers are available before the first monitor or when all
    // monitors are asleep. Require a usable monitor binding whenever one can
    // exist, and always verify the manager bindings on this Wayland server.
    bool output_enabled = false;
    struct capture_output *target;
    wl_list_for_each(target, &capture->outputs, link) {
        output_enabled |= target->output->enabled;
    }
    struct wl_client *client;
    wl_client_for_each(client, wl_display_get_client_list(capture->display)) {
        pid_t client_pid;
        uid_t client_uid;
        wl_client_get_credentials(client, &client_pid, &client_uid, NULL);
        if (client_pid == pid && client_uid == uid) {
            struct client_bindings bindings = {.capture = capture,
                                               .awaiting_output = output_enabled};
            wl_client_for_each_resource(client, check_resource, &bindings);
            if (bindings.shm && !bindings.awaiting_output && bindings.output_manager &&
                bindings.copy_manager) {
                return true;
            }
        }
    }
    return false;
}

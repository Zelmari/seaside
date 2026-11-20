#define _POSIX_C_SOURCE 200809L

#include "tty_drm.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

enum {
    SEAT_WAIT_MS = 2000,
    CURSOR_SIZE = 16,
    MAX_KMS_PLANES = 256,
    MAX_PLANE_PROPERTIES = 64,
};

struct seaside_tty_drm {
    struct wl_display *display;
    struct wlr_session *session;
    struct wlr_backend *backend;
    struct wlr_output *output;
    struct wlr_output_cursor *cursor;
    struct wl_listener session_destroy;
    struct wl_listener session_active;
    struct wl_listener backend_destroy;
    struct wl_listener new_output;
    struct wl_listener output_destroy;
    struct wl_listener output_commit;
    struct wl_listener output_present;
    struct seaside_tty_drm_observation evidence;
    double cursor_x;
    double cursor_y;
    bool commit_pending;
    uint32_t commit_seq;
    int committed_x;
    int committed_y;
    int presented_x;
    int presented_y;
};

struct cursor_buffer {
    struct wlr_buffer base;
    uint32_t pixels[CURSOR_SIZE * CURSOR_SIZE];
};

static void destroy_cursor_buffer(struct wlr_buffer *base) {
    struct cursor_buffer *buffer = wl_container_of(base, buffer, base);
    wlr_buffer_finish(base);
    free(buffer);
}

static bool read_cursor_buffer(struct wlr_buffer *base, uint32_t flags, void **data,
                               uint32_t *format, size_t *stride) {
    struct cursor_buffer *buffer = wl_container_of(base, buffer, base);
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    *data = buffer->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = CURSOR_SIZE * sizeof(buffer->pixels[0]);
    return true;
}

static void end_cursor_buffer_access(struct wlr_buffer *base) {
    (void)base;
}

static const struct wlr_buffer_impl cursor_buffer_impl = {
    .destroy = destroy_cursor_buffer,
    .begin_data_ptr_access = read_cursor_buffer,
    .end_data_ptr_access = end_cursor_buffer_access,
};

static struct wlr_buffer *make_cursor_buffer(void) {
    struct cursor_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return NULL;
    }
    wlr_buffer_init(&buffer->base, &cursor_buffer_impl, CURSOR_SIZE, CURSOR_SIZE);
    /* A small bright cross is visible over the black test scene. A fixed
     * source image makes the DRM test independent of installed cursor themes. */
    for (size_t i = 0; i < CURSOR_SIZE; ++i) {
        buffer->pixels[CURSOR_SIZE * i + CURSOR_SIZE / 2] = UINT32_C(0xffffffff);
        buffer->pixels[CURSOR_SIZE * (CURSOR_SIZE / 2) + i] = UINT32_C(0xffffffff);
    }
    return &buffer->base;
}

static void unlink_listener(struct wl_listener *listener) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

static void fail(struct seaside_tty_drm *drm, const char *message) {
    if (!drm->evidence.failed) {
        fprintf(stderr, "TTY DRM: %s\n", message);
        drm->evidence.failed = true;
    }
}

static void on_session_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, session_destroy);
    if (drm->backend) {
        fail(drm, "seat session ended before the DRM backend stopped");
    }
    unlink_listener(&drm->session_destroy);
    unlink_listener(&drm->session_active);
    drm->session = NULL;
    drm->evidence.active = false;
}

static void on_session_active(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, session_active);
    drm->evidence.active = drm->session->active;
    if (!drm->session->active && drm->backend) {
        fail(drm, "seat became inactive during the TTY fixture run");
    }
}

static void on_backend_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, backend_destroy);
    unlink_listener(&drm->backend_destroy);
    unlink_listener(&drm->new_output);
    drm->backend = NULL;
}

static void on_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, output_destroy);
    if (drm->backend && drm->cursor) {
        fail(drm, "selected output disappeared during the TTY fixture run");
    }
    unlink_listener(&drm->output_destroy);
    unlink_listener(&drm->output_commit);
    unlink_listener(&drm->output_present);
    /* wlr_output_finish destroys the cursor after emitting this signal. */
    drm->output = NULL;
    drm->cursor = NULL;
    drm->commit_pending = false;
}

static void on_new_output(struct wl_listener *listener, void *data) {
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, new_output);
    struct wlr_output *output = data;
    if (drm->output || drm->evidence.failed || !wlr_output_is_drm(output) || output->non_desktop ||
        wl_list_empty(&output->modes)) {
        return;
    }
    drm->output = output;
    wl_signal_add(&output->events.destroy, &drm->output_destroy);
}

static bool is_cursor_plane(int fd, uint32_t plane_id) {
    drmModeObjectProperties *properties =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!properties) {
        return false;
    }
    bool cursor = false;
    if (properties->count_props <= MAX_PLANE_PROPERTIES) {
        for (uint32_t i = 0; i < properties->count_props; ++i) {
            if (properties->prop_values[i] != DRM_PLANE_TYPE_CURSOR) {
                continue;
            }
            drmModePropertyRes *prop = drmModeGetProperty(fd, properties->props[i]);
            if (prop) {
                cursor = strcmp(prop->name, "type") == 0;
                drmModeFreeProperty(prop);
            }
            if (cursor) {
                break;
            }
        }
    }
    drmModeFreeObjectProperties(properties);
    return cursor;
}

static uint32_t active_cursor_plane(struct wlr_output *output, uint32_t connector_id) {
    /* Query the same DRM file used for the successful commit. Reopening the
     * device could expose a different set of planes or alter DRM ownership. */
    int fd = wlr_backend_get_drm_fd(output->backend);
    if (fd < 0) {
        return 0;
    }
    drmModeConnector *connector = drmModeGetConnectorCurrent(fd, connector_id);
    if (!connector) {
        return 0;
    }
    uint32_t encoder_id = connector->encoder_id;
    drmModeFreeConnector(connector);
    if (encoder_id == 0) {
        return 0;
    }
    drmModeEncoder *encoder = drmModeGetEncoder(fd, encoder_id);
    if (!encoder) {
        return 0;
    }
    uint32_t crtc_id = encoder->crtc_id;
    drmModeFreeEncoder(encoder);
    if (crtc_id == 0) {
        return 0;
    }
    drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
    if (!planes) {
        return 0;
    }
    uint32_t cursor_plane_id = 0;
    if (planes->count_planes <= MAX_KMS_PLANES) {
        for (uint32_t i = 0; i < planes->count_planes; ++i) {
            drmModePlane *plane = drmModeGetPlane(fd, planes->planes[i]);
            if (!plane) {
                continue;
            }
            const bool active = plane->crtc_id == crtc_id && plane->fb_id != 0;
            const uint32_t plane_id = plane->plane_id;
            drmModeFreePlane(plane);
            if (active && is_cursor_plane(fd, plane_id)) {
                cursor_plane_id = plane_id;
                break;
            }
        }
    }
    drmModeFreePlaneResources(planes);
    return cursor_plane_id;
}

static void on_output_commit(struct wl_listener *listener, void *data) {
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, output_commit);
    const struct wlr_output_event_commit *event = data;
    if (!drm->evidence.failed && drm->output->enabled && drm->cursor &&
        drm->output->hardware_cursor == drm->cursor && drm->output->cursor_front_buffer &&
        (event->state->committed & WLR_OUTPUT_STATE_BUFFER) != 0) {
        drm->commit_seq = drm->output->commit_seq;
        drm->committed_x = (int)drm->cursor_x;
        drm->committed_y = (int)drm->cursor_y;
        drm->commit_pending = true;
    }
}

static void on_output_present(struct wl_listener *listener, void *data) {
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, output_present);
    const struct wlr_output_event_present *event = data;
    if (!drm->commit_pending || event->commit_seq != drm->commit_seq) {
        return;
    }
    drm->commit_pending = false;
    if (drm->evidence.failed || !event->presented || !drm->evidence.active) {
        return;
    }
    if (drm->output->hardware_cursor != drm->cursor || !drm->output->cursor_front_buffer) {
        fail(drm, "hardware cursor was lost before page-flip completion");
        return;
    }
    /* The first frame establishes a position. Coalesced motion that returns
     * to the last presented pixel does not prove the cursor changed on-screen. */
    bool position_changed =
        drm->evidence.cursor_presented &&
        (drm->committed_x != drm->presented_x || drm->committed_y != drm->presented_y);
    if (!drm->evidence.cursor_presented ||
        (drm->evidence.presented_moves == 0 && position_changed)) {
        /* A staged wlroots cursor is insufficient. Match a real page flip,
         * then verify that the selected CRTC has a cursor-plane framebuffer. */
        uint32_t plane_id = active_cursor_plane(drm->output, drm->evidence.connector_id);
        if (plane_id == 0) {
            fail(drm, "KMS readback did not show an active cursor plane for the selected output");
            return;
        }
        if (!drm->evidence.cursor_presented) {
            fprintf(stderr, "TTY DRM: output %s presented with cursor plane %u\n",
                    drm->output->name, plane_id);
        }
        drm->evidence.cursor_plane_id = plane_id;
        drm->evidence.cursor_presented = true;
    }
    ++drm->evidence.presented_frames;
    drm->presented_x = drm->committed_x;
    drm->presented_y = drm->committed_y;
    if (position_changed) {
        ++drm->evidence.presented_moves;
    }
}

static bool primary_card_path(const char *path) {
    const char prefix[] = "/dev/dri/card";
    struct stat info;
    if (!path || strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *suffix = path + sizeof(prefix) - 1;
    if (*suffix == '\0' || suffix[strspn(suffix, "0123456789")] != '\0') {
        return false;
    }
    return stat(path, &info) == 0 && S_ISCHR(info.st_mode) && major(info.st_rdev) == 226 &&
           minor(info.st_rdev) < 64;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wait_for_active_seat(struct seaside_tty_drm *drm, struct wl_event_loop *loop) {
    if (drm->session->active) {
        return true;
    }
    const int64_t started = monotonic_ms();
    if (started < 0) {
        return false;
    }
    while (drm->session && !drm->session->active) {
        int64_t now = monotonic_ms();
        if (now < started || now - started >= SEAT_WAIT_MS ||
            wl_event_loop_dispatch(loop, (int)(SEAT_WAIT_MS - (now - started))) < 0) {
            return false;
        }
    }
    const int64_t finished = monotonic_ms();
    return drm->session && finished >= started && finished - started < SEAT_WAIT_MS;
}

static bool add_backend(struct wlr_backend *multi, struct wlr_backend *child) {
    if (!child) {
        return false;
    }
    if (!wlr_multi_backend_add(multi, child)) {
        wlr_backend_destroy(child);
        return false;
    }
    return true;
}

struct seaside_tty_drm *seaside_tty_drm_create(struct wl_display *display, const char *device_path,
                                               struct wlr_backend **backend,
                                               struct wlr_renderer **renderer) {
    if (!display || !backend || !renderer) {
        return NULL;
    }
    *backend = NULL;
    *renderer = NULL;
    if (!primary_card_path(device_path)) {
        fprintf(stderr, "TTY DRM: missing or invalid primary DRM node: %s\n",
                device_path ? device_path : "(null)");
        return NULL;
    }
    struct seaside_tty_drm *drm = calloc(1, sizeof(*drm));
    if (!drm) {
        fprintf(stderr, "TTY DRM: failed to allocate device owner\n");
        return NULL;
    }
    drm->display = display;
    drm->session_destroy.notify = on_session_destroy;
    drm->session_active.notify = on_session_active;
    drm->backend_destroy.notify = on_backend_destroy;
    drm->new_output.notify = on_new_output;
    drm->output_destroy.notify = on_output_destroy;
    drm->output_commit.notify = on_output_commit;
    drm->output_present.notify = on_output_present;
    wl_list_init(&drm->session_destroy.link);
    wl_list_init(&drm->session_active.link);
    wl_list_init(&drm->backend_destroy.link);
    wl_list_init(&drm->new_output.link);
    wl_list_init(&drm->output_destroy.link);
    wl_list_init(&drm->output_commit.link);
    wl_list_init(&drm->output_present.link);

    wlr_log_init(WLR_INFO, NULL);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    drm->session = wlr_session_create(loop);
    if (!drm->session) {
        fail(drm, "libseat could not acquire the console session");
        goto error;
    }
    wl_signal_add(&drm->session->events.destroy, &drm->session_destroy);
    wl_signal_add(&drm->session->events.active, &drm->session_active);
    if (!wait_for_active_seat(drm, loop)) {
        fail(drm, "console seat did not become active");
        goto error;
    }
    drm->evidence.active = true;

    drm->backend = wlr_multi_backend_create(loop);
    if (!drm->backend) {
        fail(drm, "could not create the combined DRM and libinput backend");
        goto error;
    }
    wl_signal_add(&drm->backend->events.destroy, &drm->backend_destroy);
    wl_signal_add(&drm->backend->events.new_output, &drm->new_output);

    struct wlr_device *device = wlr_session_open_file(drm->session, device_path);
    if (!device || drmGetNodeTypeFromFd(device->fd) != DRM_NODE_PRIMARY) {
        fail(drm, "libseat could not open the selected primary DRM node");
        goto error;
    }
    if (!add_backend(drm->backend, wlr_drm_backend_create(drm->session, device, NULL))) {
        fail(drm, "could not create the DRM backend for the selected device");
        goto error;
    }
    if (!add_backend(drm->backend, wlr_libinput_backend_create(drm->session))) {
        fail(drm, "could not create the libinput backend for the active seat");
        goto error;
    }
    *renderer = wlr_renderer_autocreate(drm->backend);
    if (!*renderer) {
        fail(drm, "wlroots could not select a DRM renderer");
        goto error;
    }
    *backend = drm->backend;
    return drm;

error:
    /* wlroots transfers an opened device to its DRM backend on success.
     * Session teardown closes any device left over by an early create error. */
    wlr_backend_destroy(drm->backend);
    seaside_tty_drm_destroy(drm);
    return NULL;
}

void seaside_tty_drm_destroy(struct seaside_tty_drm *drm) {
    if (!drm) {
        return;
    }
    unlink_listener(&drm->backend_destroy);
    unlink_listener(&drm->new_output);
    unlink_listener(&drm->output_destroy);
    unlink_listener(&drm->output_commit);
    unlink_listener(&drm->output_present);
    wlr_output_cursor_destroy(drm->cursor);
    unlink_listener(&drm->session_destroy);
    unlink_listener(&drm->session_active);
    if (!drm->backend) {
        wlr_session_destroy(drm->session);
    }
    free(drm);
}

static struct wlr_output_mode *initial_mode(struct wlr_output *output) {
    struct wlr_output_mode *selected = wlr_output_preferred_mode(output);
    if (!selected) {
        return NULL;
    }
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &output->modes, link) {
        if (mode->width == selected->width && mode->height == selected->height &&
            mode->refresh > selected->refresh) {
            selected = mode;
        }
    }
    return selected;
}

static bool set_output_enabled(struct wlr_output *output, bool enabled,
                               struct wlr_output_mode *mode) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, enabled);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    const bool committed = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    return committed;
}

struct wlr_output *seaside_tty_drm_enable(struct seaside_tty_drm *drm,
                                          struct wlr_allocator *allocator,
                                          struct wlr_renderer *renderer) {
    if (!drm || !drm->evidence.active || drm->evidence.failed || drm->cursor) {
        return NULL;
    }
    struct wlr_output *output = drm->output;
    if (!output) {
        fail(drm, "DRM backend announced no connected desktop output with modes");
        return NULL;
    }
    struct wlr_output_mode *mode = initial_mode(output);
    if (!mode || !allocator || !renderer || !wlr_output_init_render(output, allocator, renderer) ||
        !set_output_enabled(output, true, mode)) {
        fail(drm, "could not initialize and enable the selected DRM output");
        return NULL;
    }

    struct wlr_buffer *buffer = make_cursor_buffer();
    drm->cursor = wlr_output_cursor_create(output);
    int width, height;
    wlr_output_effective_resolution(output, &width, &height);
    drm->cursor_x = width / 2;
    drm->cursor_y = height / 2;
    const bool cursor_staged =
        buffer && drm->cursor &&
        wlr_output_cursor_move(drm->cursor, drm->cursor_x, drm->cursor_y) &&
        wlr_output_cursor_set_buffer(drm->cursor, buffer, CURSOR_SIZE / 2, CURSOR_SIZE / 2) &&
        output->hardware_cursor == drm->cursor && output->cursor_front_buffer;
    wlr_buffer_drop(buffer);
    if (!cursor_staged) {
        /* wlroots considers a software fallback successful. Remove that
         * cursor before the scene ever renders, then disable this output. */
        wlr_output_cursor_destroy(drm->cursor);
        drm->cursor = NULL;
        fail(drm, "hardware cursor staging failed for the selected DRM output");
        if (!set_output_enabled(output, false, NULL)) {
            fprintf(stderr, "TTY DRM: could not disable output %s before seat teardown\n",
                    output->name);
        }
        return NULL;
    }
    drm->evidence.connector_id = wlr_drm_connector_get_id(output);
    wl_signal_add(&output->events.commit, &drm->output_commit);
    wl_signal_add(&output->events.present, &drm->output_present);
    wlr_output_create_global(output, drm->display);
    fprintf(stderr, "TTY DRM: enabled %s (%dx%d at %d mHz, connector %u)\n", output->name,
            output->width, output->height, output->refresh, drm->evidence.connector_id);
    return output;
}

static double clamp(double value, double high) {
    if (value < 0) {
        return 0;
    }
    if (value > high) {
        return high;
    }
    return value;
}

bool seaside_tty_drm_move(struct seaside_tty_drm *drm, bool absolute, double x, double y) {
    if (!drm || !drm->cursor || drm->evidence.failed || !drm->evidence.active || !isfinite(x) ||
        !isfinite(y)) {
        return false;
    }
    int width, height;
    wlr_output_effective_resolution(drm->output, &width, &height);
    if (width <= 0 || height <= 0) {
        return false;
    }
    const double next_x = clamp(absolute ? x * width : drm->cursor_x + x, width - 1);
    const double next_y = clamp(absolute ? y * height : drm->cursor_y + y, height - 1);
    /* Keep fractional motion for the next event, but only count a change the
     * DRM backend can actually represent. The fixture uses scale 1. */
    const bool changed = (int)next_x != (int)drm->cursor_x || (int)next_y != (int)drm->cursor_y;
    drm->cursor_x = next_x;
    drm->cursor_y = next_y;
    if (!changed) {
        return true;
    }
    if (drm->output->hardware_cursor != drm->cursor ||
        !wlr_output_cursor_move(drm->cursor, next_x, next_y)) {
        fail(drm, "the backend rejected hardware cursor motion");
        return false;
    }
    ++drm->evidence.cursor_moves;
    return true;
}

struct seaside_tty_drm_observation seaside_tty_drm_observation(const struct seaside_tty_drm *drm) {
    return drm ? drm->evidence : (struct seaside_tty_drm_observation){0};
}

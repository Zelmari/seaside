#define _POSIX_C_SOURCE 200809L

#include "tty_drm.h"
#include "cursor_layout.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <limits.h>
#include <linux/vt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
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
    MAX_KMS_CARDS = 64,
    MAX_KMS_PLANES = 256,
    MAX_PLANE_PROPERTIES = 64,
};

struct seaside_tty_drm {
    struct wl_display *display;
    struct wlr_session *session;
    struct wlr_backend *backend;
    struct wl_list outputs;
    struct tty_output *pointer_output;
    struct seaside_tty_drm_output_callbacks callbacks;
    void *callback_data;
    struct wl_listener session_destroy;
    struct wl_listener session_active;
    struct wl_listener backend_destroy;
    struct wl_listener new_output;
    struct seaside_tty_drm_observation evidence;
    struct wlr_buffer *visible_buffer;
    struct wlr_buffer *transparent_buffer;
    double cursor_x;
    double cursor_y;
    bool cursor_visible;
    bool cursor_position_set;
    bool fixture_mode;
};

struct tty_output {
    struct seaside_tty_drm *owner;
    struct wlr_output *output;
    struct wlr_output_cursor *cursor;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener present;
    bool pending;
    bool rejected;
    bool usable;
    bool cursor_visible;
    bool cursor_presented;
    bool commit_pending;
    bool movement_verified;
    bool render_locked;
    uint32_t connector_id;
    uint32_t cursor_plane_id;
    uint32_t commit_seq;
    int x, y, width, height;
    uint64_t committed_frames, presented_frames, discarded_frames, last_presentation_ns;
    const char *disabled_reason;
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

static struct wlr_buffer *make_cursor_buffer(bool visible) {
    struct cursor_buffer *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return NULL;
    }
    wlr_buffer_init(&buffer->base, &cursor_buffer_impl, CURSOR_SIZE, CURSOR_SIZE);
    /* A small bright cross is visible over the black test scene. A fixed
     * source image makes the DRM test independent of installed cursor themes. */
    if (visible) {
        for (size_t i = 0; i < CURSOR_SIZE; ++i) {
            buffer->pixels[CURSOR_SIZE * i + CURSOR_SIZE / 2] = UINT32_C(0xffffffff);
            buffer->pixels[CURSOR_SIZE * (CURSOR_SIZE / 2) + i] = UINT32_C(0xffffffff);
        }
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
    if (!drm->session->active) {
        /* wlroots destroys DRM outputs on VT pause and advertises fresh
         * handles on resume. Reverify the plane on the new presentation. */
        struct tty_output *view;
        wl_list_for_each(view, &drm->outputs, link) {
            view->commit_pending = false;
            view->cursor_presented = false;
            view->movement_verified = false;
            view->cursor_plane_id = 0;
        }
    }
    if (drm->backend) {
        fprintf(stderr, "TTY DRM: seat %s\n", drm->session->active ? "resumed" : "paused");
    }
}

static void on_backend_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, backend_destroy);
    unlink_listener(&drm->backend_destroy);
    unlink_listener(&drm->new_output);
    drm->backend = NULL;
}

static char *output_identity(const struct wlr_output *output);

static void observe_output(const struct tty_output *view, bool connected) {
    const struct seaside_tty_drm *drm = view->owner;
    if (drm->callbacks.observed) {
        char *identity = output_identity(view->output);
        drm->callbacks.observed(drm->callback_data, view->output, view->output->name, identity,
                                connected, view->disabled_reason);
        free(identity);
    }
}

static void remove_usable_output(struct tty_output *view) {
    struct seaside_tty_drm *drm = view->owner;
    const bool notify = view->usable;
    if (view->render_locked) {
        wlr_output_lock_attach_render(view->output, false);
        view->render_locked = false;
    }
    view->usable = false;
    if (drm->pointer_output == view) {
        drm->pointer_output = NULL;
    }
    unlink_listener(&view->commit);
    unlink_listener(&view->present);
    if (notify && drm->callbacks.removed) {
        drm->callbacks.removed(drm->callback_data, view->output, view->output->name);
    }
}

static void destroy_output_view(struct tty_output *view) {
    remove_usable_output(view);
    unlink_listener(&view->destroy);
    wl_list_remove(&view->link);
    free(view);
}

static void on_output_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct tty_output *view = wl_container_of(listener, view, destroy);
    /* wlroots destroys the cursor after emitting the output destroy event. */
    remove_usable_output(view);
    observe_output(view, false);
    destroy_output_view(view);
}

static void on_output_commit(struct wl_listener *listener, void *data);
static void on_output_present(struct wl_listener *listener, void *data);

static void on_new_output(struct wl_listener *listener, void *data) {
    struct seaside_tty_drm *drm = wl_container_of(listener, drm, new_output);
    struct wlr_output *output = data;
    if (drm->evidence.failed || !wlr_output_is_drm(output) || output->non_desktop ||
        wl_list_empty(&output->modes)) {
        return;
    }
    struct tty_output *view = calloc(1, sizeof(*view));
    if (!view) {
        fail(drm, "could not allocate DRM output state");
        return;
    }
    view->owner = drm;
    view->output = output;
    view->pending = true;
    view->connector_id = wlr_drm_connector_get_id(output);
    view->destroy.notify = on_output_destroy;
    view->commit.notify = on_output_commit;
    view->present.notify = on_output_present;
    wl_list_init(&view->commit.link);
    wl_list_init(&view->present.link);
    wl_list_insert(drm->outputs.prev, &view->link);
    wl_signal_add(&output->events.destroy, &view->destroy);
    observe_output(view, true);
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

static bool has_hardware_cursor(const struct tty_output *view) {
    return view->cursor && view->output->hardware_cursor == view->cursor &&
           view->output->cursor_front_buffer;
}

static void reject_output(struct tty_output *view, const char *reason) {
    fprintf(stderr, "TTY DRM: disabling output %s: %s\n", view->output->name, reason);
    if (view->owner->fixture_mode) {
        fail(view->owner, reason);
    }
    /* Remove a possible software cursor before another scene frame. Defer
     * the output commit until outside input and page-flip callbacks. */
    wlr_output_cursor_destroy(view->cursor);
    view->cursor = NULL;
    view->disabled_reason = reason;
    view->rejected = true;
    view->cursor_presented = false;
    view->commit_pending = false;
}

static void on_output_commit(struct wl_listener *listener, void *data) {
    struct tty_output *view = wl_container_of(listener, view, commit);
    struct seaside_tty_drm *drm = view->owner;
    const struct wlr_output_event_commit *event = data;
    if (drm->evidence.failed || !drm->evidence.active || view->rejected || !view->output->enabled ||
        (event->state->committed & WLR_OUTPUT_STATE_BUFFER) == 0) {
        return;
    }
    if (!has_hardware_cursor(view)) {
        reject_output(view, "hardware cursor was lost during frame commit");
        return;
    }
    ++view->committed_frames;
    view->commit_seq = view->output->commit_seq;
    view->committed_x = (int)view->cursor->x;
    view->committed_y = (int)view->cursor->y;
    view->commit_pending = true;
}

static void on_output_present(struct wl_listener *listener, void *data) {
    struct tty_output *view = wl_container_of(listener, view, present);
    struct seaside_tty_drm *drm = view->owner;
    const struct wlr_output_event_present *event = data;
    if (event->presented) {
        ++view->presented_frames;
        if (event->when.tv_sec >= 0 && event->when.tv_nsec >= 0) {
            view->last_presentation_ns =
                (uint64_t)event->when.tv_sec * 1000000000 + (uint64_t)event->when.tv_nsec;
        }
    } else {
        ++view->discarded_frames;
    }
    if (!view->commit_pending || event->commit_seq != view->commit_seq) {
        return;
    }
    view->commit_pending = false;
    if (drm->evidence.failed || !event->presented || !drm->evidence.active || view->rejected) {
        return;
    }
    if (!has_hardware_cursor(view)) {
        reject_output(view, "hardware cursor was lost before page-flip completion");
        return;
    }
    /* The first frame establishes a position. Coalesced motion that returns
     * to the last presented pixel does not prove the cursor changed on-screen. */
    const bool position_changed =
        view->cursor_presented && view->cursor_visible &&
        (view->committed_x != view->presented_x || view->committed_y != view->presented_y);
    if (!view->cursor_presented || (!view->movement_verified && position_changed)) {
        /* A staged wlroots cursor is insufficient. Match a real page flip,
         * then verify that the selected CRTC has a cursor-plane framebuffer. */
        uint32_t plane_id = active_cursor_plane(view->output, view->connector_id);
        if (plane_id == 0) {
            reject_output(view, "KMS readback did not show an active cursor plane");
            return;
        }
        if (!view->cursor_presented) {
            fprintf(stderr, "TTY DRM: output %s presented with cursor plane %u\n",
                    view->output->name, plane_id);
        }
        view->cursor_plane_id = plane_id;
        view->cursor_presented = true;
        view->movement_verified = position_changed;
    }
    ++drm->evidence.presented_frames;
    view->presented_x = view->committed_x;
    view->presented_y = view->committed_y;
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

bool seaside_tty_active_console(void) {
    char path[64];
    struct vt_stat active = {0};
    static const char prefix[] = "/dev/tty";
    if (ttyname_r(STDIN_FILENO, path, sizeof(path)) != 0 ||
        strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *digits = path + sizeof(prefix) - 1;
    if (*digits == '\0' || digits[strspn(digits, "0123456789")] != '\0') {
        return false;
    }
    errno = 0;
    const unsigned long vt = strtoul(digits, NULL, 10);
    return errno == 0 && vt > 0 && tcgetpgrp(STDIN_FILENO) == getpgrp() &&
           ioctl(STDIN_FILENO, VT_GETSTATE, &active) == 0 && vt == active.v_active;
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

static struct wlr_device *find_primary_device(struct wlr_session *session, const char *path) {
    if (path) {
        return wlr_session_open_file(session, path);
    }
    struct wlr_device *devices[MAX_KMS_CARDS];
    const ssize_t count = wlr_session_find_gpus(session, MAX_KMS_CARDS, devices);
    if (count <= 0) {
        return NULL;
    }
    for (ssize_t i = 1; i < count; ++i) {
        wlr_session_close_file(session, devices[i]);
    }
    return devices[0];
}

struct seaside_tty_drm *seaside_tty_drm_create(struct wl_display *display, const char *device_path,
                                               struct wlr_backend **backend,
                                               struct wlr_renderer **renderer) {
    if (!display || !backend || !renderer) {
        return NULL;
    }
    *backend = NULL;
    *renderer = NULL;
    if (device_path && !primary_card_path(device_path)) {
        fprintf(stderr, "TTY DRM: missing or invalid primary DRM node: %s\n", device_path);
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
    drm->cursor_visible = true;
    wl_list_init(&drm->outputs);
    wl_list_init(&drm->session_destroy.link);
    wl_list_init(&drm->session_active.link);
    wl_list_init(&drm->backend_destroy.link);
    wl_list_init(&drm->new_output.link);

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

    struct wlr_device *device = find_primary_device(drm->session, device_path);
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
    drm->callbacks = (struct seaside_tty_drm_output_callbacks){0};
    struct wl_list *link = drm->outputs.next;
    while (link != &drm->outputs) {
        struct tty_output *view = wl_container_of(link, view, link);
        link = link->next;
        wlr_output_cursor_destroy(view->cursor);
        destroy_output_view(view);
    }
    wlr_buffer_drop(drm->visible_buffer);
    wlr_buffer_drop(drm->transparent_buffer);
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

void seaside_tty_drm_set_output_callbacks(struct seaside_tty_drm *drm,
                                          const struct seaside_tty_drm_output_callbacks *callbacks,
                                          void *data) {
    if (drm) {
        drm->callbacks = callbacks ? *callbacks : (struct seaside_tty_drm_output_callbacks){0};
        drm->callback_data = callbacks ? data : NULL;
    }
}

struct mode_rank {
    int32_t refresh;
    int64_t pixels;
};

static struct mode_rank maximum_mode_rank(const struct wlr_output *output) {
    struct mode_rank rank = {0};
    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &output->modes, link) {
        if (mode->refresh > rank.refresh) {
            rank.refresh = mode->refresh;
        }
        int64_t pixels = (int64_t)mode->width * mode->height;
        if (pixels > rank.pixels) {
            rank.pixels = pixels;
        }
    }
    return rank;
}

static bool ranks_first(const struct tty_output *view, const struct tty_output *current) {
    if (!current) {
        return true;
    }
    const struct mode_rank a = maximum_mode_rank(view->output);
    const struct mode_rank b = maximum_mode_rank(current->output);
    if (a.refresh != b.refresh) {
        return a.refresh > b.refresh;
    }
    if (a.pixels != b.pixels) {
        return a.pixels > b.pixels;
    }
    return strcmp(view->output->name, current->output->name) < 0;
}

static struct tty_output *next_pending(struct seaside_tty_drm *drm) {
    struct tty_output *next = NULL;
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->pending && ranks_first(view, next)) {
            next = view;
        }
    }
    return next;
}

static double clamp(double value, double low, double high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/* Preserve existing logical rectangles during hotplug. A returned output can
 * reuse a large enough gap without shifting the surviving outputs. */
static bool place_output(struct tty_output *view) {
    int previous;
    do {
        previous = view->x;
        struct tty_output *other;
        wl_list_for_each(other, &view->owner->outputs, link) {
            if (other->usable && view->x < other->x + other->width &&
                (int64_t)view->x + view->width > other->x) {
                view->x = other->x + other->width;
            }
        }
    } while (view->x != previous);
    return view->x <= INT_MAX - view->width;
}

static void disable_output(struct tty_output *view) {
    remove_usable_output(view);
    view->rejected = false;
    wlr_output_cursor_destroy(view->cursor);
    view->cursor = NULL;
    if (view->output->global) {
        wlr_output_destroy_global(view->output);
    }
    if (view->output->enabled && !set_output_enabled(view->output, false, NULL)) {
        fprintf(stderr, "TTY DRM: could not disable output %s\n", view->output->name);
    }
    observe_output(view, true);
}

static bool prepare_cursor_buffers(struct seaside_tty_drm *drm) {
    if (!drm->visible_buffer) {
        drm->visible_buffer = make_cursor_buffer(true);
    }
    if (!drm->transparent_buffer) {
        drm->transparent_buffer = make_cursor_buffer(false);
    }
    if (!drm->visible_buffer || !drm->transparent_buffer) {
        fail(drm, "could not allocate the hardware cursor images");
        return false;
    }
    return true;
}

static bool load_cursor_image(struct tty_output *view, bool visible) {
    struct wlr_buffer *buffer =
        visible ? view->owner->visible_buffer : view->owner->transparent_buffer;
    if (!wlr_output_cursor_set_buffer(view->cursor, buffer, CURSOR_SIZE / 2, CURSOR_SIZE / 2) ||
        !has_hardware_cursor(view)) {
        return false;
    }
    view->cursor_visible = visible;
    return true;
}

static char *output_identity(const struct wlr_output *output) {
    if (!output->serial || !*output->serial) {
        return NULL;
    }
    const char *make = output->make ? output->make : "";
    const char *model = output->model ? output->model : "";
    int length = snprintf(NULL, 0, "%s %s %s", make, model, output->serial);
    if (length < 0) {
        return NULL;
    }
    char *identity = malloc((size_t)length + 1);
    if (identity) {
        snprintf(identity, (size_t)length + 1, "%s %s %s", make, model, output->serial);
    }
    return identity;
}

static bool enable_output(struct tty_output *view, struct wlr_allocator *allocator,
                          struct wlr_renderer *renderer) {
    struct seaside_tty_drm *drm = view->owner;
    struct wlr_output *output = view->output;
    view->pending = false;
    struct wlr_output_mode *mode = initial_mode(output);
    if (!mode || !wlr_output_init_render(output, allocator, renderer) ||
        !set_output_enabled(output, true, mode)) {
        reject_output(view, "could not initialize and enable the DRM output");
        return false;
    }
    wlr_output_effective_resolution(output, &view->width, &view->height);
    if (view->width <= 0 || view->height <= 0 || !place_output(view)) {
        reject_output(view, "invalid logical output rectangle");
        return false;
    }

    const bool initial_pointer = !drm->cursor_position_set;
    view->cursor = wlr_output_cursor_create(output);
    if (!view->cursor || !wlr_output_cursor_move(view->cursor, view->width / 2, view->height / 2) ||
        !load_cursor_image(view, initial_pointer && drm->cursor_visible)) {
        reject_output(view, "hardware cursor staging failed");
        return false;
    }
    wlr_output_create_global(output, drm->display);
    if (!output->global) {
        fail(drm, "could not publish the DRM output global");
        return false;
    }
    if (initial_pointer) {
        drm->cursor_x = view->x + view->width / 2;
        drm->cursor_y = view->height / 2;
        drm->cursor_position_set = true;
        drm->pointer_output = view;
    }
    view->usable = true;
    view->disabled_reason = NULL;
    wl_signal_add(&output->events.commit, &view->commit);
    wl_signal_add(&output->events.present, &view->present);
    if (drm->callbacks.added) {
        char *identity = output_identity(output);
        const bool attached =
            drm->callbacks.added(drm->callback_data, output, output->name, identity, view->x,
                                 view->y, view->width, view->height);
        free(identity);
        if (!attached) {
            fail(drm, "could not attach an enabled DRM output");
            return false;
        }
    }
    fprintf(stderr, "TTY DRM: enabled %s (%dx%d at %d mHz, connector %u)\n", output->name,
            output->width, output->height, output->refresh, view->connector_id);
    return true;
}

static bool available(const struct tty_output *view) {
    return view && view->usable && !view->rejected;
}

static struct seaside_cursor_box cursor_box(const struct tty_output *view) {
    return (struct seaside_cursor_box){view->x, view->y, view->width, view->height};
}

static struct tty_output *nearest_output(const struct seaside_tty_drm *drm, double x, double y,
                                         double *result_x, double *result_y) {
    struct tty_output *selected = NULL;
    double best_distance = 0;
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (!available(view)) {
            continue;
        }
        const struct seaside_cursor_point inside =
            seaside_cursor_clamp(cursor_box(view), (struct seaside_cursor_point){x, y});
        const double dx = x - inside.x;
        const double dy = y - inside.y;
        const double distance = dx * dx + dy * dy;
        if (!selected || distance < best_distance) {
            best_distance = distance;
            selected = view;
            *result_x = inside.x;
            *result_y = inside.y;
            if (distance == 0) {
                break;
            }
        }
    }
    return selected;
}

static bool move_cursor(struct seaside_tty_drm *drm, double x, double y, bool count_move) {
    double next_x, next_y;
    struct tty_output *target = nearest_output(drm, x, y, &next_x, &next_y);
    if (!target) {
        return false;
    }
    struct tty_output *previous = drm->pointer_output;
    if (previous != target && available(previous) && previous->cursor_visible &&
        !load_cursor_image(previous, false)) {
        reject_output(previous, "could not hide hardware cursor while leaving output");
    }
    drm->pointer_output = target;
    const double local_x = next_x - target->x;
    const double local_y = next_y - target->y;
    const double scale = target->output->scale;
    const bool pixel_changed = (int)(local_x * scale) != (int)target->cursor->x ||
                               (int)(local_y * scale) != (int)target->cursor->y;
    if ((pixel_changed && !wlr_output_cursor_move(target->cursor, local_x, local_y)) ||
        !has_hardware_cursor(target) ||
        (target->cursor_visible != drm->cursor_visible &&
         !load_cursor_image(target, drm->cursor_visible))) {
        reject_output(target, "the backend rejected hardware cursor motion");
        return false;
    }
    if (count_move && ((int)drm->cursor_x != (int)next_x || (int)drm->cursor_y != (int)next_y)) {
        ++drm->evidence.cursor_moves;
    }
    drm->cursor_x = next_x;
    drm->cursor_y = next_y;
    return true;
}

static struct tty_output *find_output(struct seaside_tty_drm *drm, struct wlr_output *output) {
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->output == output) {
            return view;
        }
    }
    return NULL;
}

bool seaside_tty_drm_output_status(struct seaside_tty_drm *drm, struct wlr_output *output,
                                   struct seaside_drm_output_status *status) {
    struct tty_output *view = find_output(drm, output);
    if (!view) {
        return false;
    }
    *status = (struct seaside_drm_output_status){
        .settings = {output->current_mode, output->width, output->height, output->refresh, view->x,
                     view->y, 90 * (int)output->transform, output->scale,
                     output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED},
        .logical_width = view->width,
        .logical_height = view->height,
        .cursor = has_hardware_cursor(view),
        .adaptive_sync = output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED,
        .frame_pending = view->commit_pending,
        .committed_frames = view->committed_frames,
        .presented_frames = view->presented_frames,
        .discarded_frames = view->discarded_frames,
        .last_presentation_ns = view->last_presentation_ns,
    };
    return true;
}

bool seaside_tty_drm_select_mode(struct wlr_output *output,
                                 struct seaside_drm_output_settings *settings) {
    const struct wlr_output_mode *preferred = wlr_output_preferred_mode(output);
    if (!preferred) {
        return false;
    }
    const int width = settings->width ? settings->width : preferred->width;
    const int height = settings->height ? settings->height : preferred->height;
    struct wlr_output_mode *mode, *selected = NULL;
    wl_list_for_each(mode, &output->modes, link) {
        if (mode->width == width && mode->height == height &&
            (!settings->refresh_millihz || mode->refresh == settings->refresh_millihz) &&
            (!selected || selected->refresh < mode->refresh)) {
            selected = mode;
        }
    }
    if (!selected) {
        return false;
    }
    settings->mode = selected;
    settings->width = width;
    settings->height = height;
    settings->refresh_millihz = selected->refresh;
    return true;
}

static bool configure_output(struct wlr_output *output,
                             const struct seaside_drm_output_settings *settings, bool test) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (output->current_mode != settings->mode) {
        wlr_output_state_set_mode(&state, settings->mode);
    }
    if (output->scale != settings->scale) {
        wlr_output_state_set_scale(&state, settings->scale);
    }
    const enum wl_output_transform transform = settings->rotation / 90;
    if (output->transform != transform) {
        wlr_output_state_set_transform(&state, transform);
    }
    if ((output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) !=
        settings->adaptive_sync) {
        wlr_output_state_set_adaptive_sync_enabled(&state, settings->adaptive_sync);
    }
    const bool ok = !state.committed || (test ? wlr_output_test_state(output, &state)
                                              : wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);
    return ok;
}

bool seaside_tty_drm_check_settings(struct wlr_output *output,
                                    const struct seaside_drm_output_settings *settings) {
    return configure_output(output, settings, true);
}

bool seaside_tty_drm_apply_settings(struct seaside_tty_drm *drm, struct wlr_output *output,
                                    const struct seaside_drm_output_settings *settings) {
    struct tty_output *view = find_output(drm, output);
    if (!available(view) || !configure_output(output, settings, false)) {
        return false;
    }
    view->x = settings->x;
    view->y = settings->y;
    wlr_output_effective_resolution(output, &view->width, &view->height);
    wlr_output_schedule_frame(output);
    return true;
}

void seaside_tty_drm_set_direct_scanout(struct seaside_tty_drm *drm, struct wlr_output *output,
                                        bool enabled) {
    struct tty_output *view = find_output(drm, output);
    if (available(view) && view->render_locked == enabled) {
        wlr_output_lock_attach_render(output, !enabled);
        view->render_locked = !enabled;
        wlr_output_schedule_frame(output);
    }
}

void seaside_tty_drm_reject_unrestorable(struct seaside_tty_drm *drm, struct wlr_output *output) {
    struct tty_output *view = find_output(drm, output);
    if (available(view)) {
        reject_output(view, "could not restore output state after a failed config change");
    }
}

void seaside_tty_drm_reconcile_cursor(struct seaside_tty_drm *drm) {
    if (drm->evidence.active) {
        move_cursor(drm, drm->cursor_x, drm->cursor_y, false);
    }
}

bool seaside_tty_drm_enable_pending(struct seaside_tty_drm *drm, struct wlr_allocator *allocator,
                                    struct wlr_renderer *renderer) {
    if (!drm || !allocator || !renderer || drm->evidence.failed) {
        return false;
    }
    if (!drm->evidence.active) {
        return true;
    }
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->rejected) {
            disable_output(view);
        }
    }
    if (next_pending(drm) && !prepare_cursor_buffers(drm)) {
        return false;
    }
    while ((view = next_pending(drm))) {
        if (!enable_output(view, allocator, renderer)) {
            disable_output(view);
            if (drm->evidence.failed) {
                return false;
            }
        }
    }
    if (!available(drm->pointer_output)) {
        move_cursor(drm, drm->cursor_x, drm->cursor_y, false);
    }
    return true;
}

struct wlr_output *seaside_tty_drm_enable(struct seaside_tty_drm *drm,
                                          struct wlr_allocator *allocator,
                                          struct wlr_renderer *renderer) {
    if (!drm || !drm->evidence.active || drm->evidence.failed || drm->pointer_output ||
        !allocator || !renderer) {
        return NULL;
    }
    drm->fixture_mode = true;
    struct tty_output *view = next_pending(drm);
    if (!view) {
        fail(drm, "DRM backend announced no connected desktop output with modes");
        return NULL;
    }
    if (!prepare_cursor_buffers(drm)) {
        return NULL;
    }
    if (!enable_output(view, allocator, renderer)) {
        disable_output(view);
        return NULL;
    }
    return view->output;
}

bool seaside_tty_drm_cursor_position(const struct seaside_tty_drm *drm, double *x, double *y) {
    if (!drm || !drm->evidence.active || !available(drm->pointer_output) || !x || !y) {
        return false;
    }
    *x = drm->cursor_x;
    *y = drm->cursor_y;
    return true;
}

bool seaside_tty_drm_absolute_delta(const struct seaside_tty_drm *drm, double x, double y,
                                    double *dx, double *dy) {
    if (!drm || !drm->evidence.active || !available(drm->pointer_output) || !dx || !dy ||
        !isfinite(x) || !isfinite(y)) {
        return false;
    }
    int left = INT_MAX;
    int right = INT_MIN;
    int top = INT_MAX;
    int bottom = INT_MIN;
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (!available(view)) {
            continue;
        }
        if (view->x < left) {
            left = view->x;
        }
        if (view->x + view->width > right) {
            right = view->x + view->width;
        }
        if (view->y < top) {
            top = view->y;
        }
        if (view->y + view->height > bottom) {
            bottom = view->y + view->height;
        }
    }
    *dx = left + clamp(x, 0, 1) * ((double)right - left) - drm->cursor_x;
    *dy = top + clamp(y, 0, 1) * ((double)bottom - top) - drm->cursor_y;
    return true;
}

bool seaside_tty_drm_set_cursor_visible(struct seaside_tty_drm *drm, bool visible) {
    if (!drm || drm->evidence.failed) {
        return false;
    }
    drm->cursor_visible = visible;
    struct tty_output *view = drm->pointer_output;
    if (drm->evidence.active && available(view) && view->cursor_visible != visible &&
        !load_cursor_image(view, visible)) {
        reject_output(view, "could not change hardware cursor visibility");
        return false;
    }
    return true;
}

static struct seaside_cursor_point cross_output_gap(const struct seaside_tty_drm *drm,
                                                    struct seaside_cursor_point target) {
    const struct seaside_cursor_box current = cursor_box(drm->pointer_output);
    if (seaside_cursor_contains(current, target)) {
        return target;
    }
    const struct seaside_cursor_point origin = {drm->cursor_x, drm->cursor_y};
    struct seaside_cursor_gap best = {0};
    const struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (!available(view) || view == drm->pointer_output) {
            continue;
        }
        const struct seaside_cursor_box destination = cursor_box(view);
        if (seaside_cursor_contains(destination, target)) {
            return target;
        }
        seaside_cursor_consider_gap(current, destination, origin, target, &best);
    }
    return best.found ? best.point : target;
}

bool seaside_tty_drm_move(struct seaside_tty_drm *drm, bool absolute, double x, double y) {
    if (!drm || drm->evidence.failed || !drm->evidence.active || !available(drm->pointer_output) ||
        !isfinite(x) || !isfinite(y)) {
        return false;
    }
    if (absolute && !seaside_tty_drm_absolute_delta(drm, x, y, &x, &y)) {
        return false;
    }
    struct seaside_cursor_point target = {drm->cursor_x + clamp(x, -INT_MAX, INT_MAX),
                                          drm->cursor_y + clamp(y, -INT_MAX, INT_MAX)};
    if (!absolute) {
        target = cross_output_gap(drm, target);
    }
    return move_cursor(drm, target.x, target.y, true);
}

bool seaside_tty_drm_change_vt(struct seaside_tty_drm *drm, unsigned vt) {
    return drm && drm->session && drm->session->active && wlr_session_change_vt(drm->session, vt);
}

struct seaside_tty_drm_observation seaside_tty_drm_observation(const struct seaside_tty_drm *drm) {
    if (!drm) {
        return (struct seaside_tty_drm_observation){0};
    }
    struct seaside_tty_drm_observation result = drm->evidence;
    const struct tty_output *pointer = drm->pointer_output;
    const bool has_pointer = available(pointer);
    if (result.active && has_pointer) {
        result.connector_id = pointer->connector_id;
        result.cursor_plane_id = pointer->cursor_plane_id;
        result.cursor_presented = pointer->cursor_presented;
    }
    if (result.active) {
        struct tty_output *view;
        wl_list_for_each(view, &drm->outputs, link) {
            if (view->pending || view->rejected || (!has_pointer && available(view))) {
                result.output_pending = true;
                break;
            }
        }
    }
    return result;
}

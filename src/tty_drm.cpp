
#include "tty_drm.hpp"
#include "cursor_layout.hpp"
#include "wlr_compat.hpp"

#include <atomic>
#include <drm_fourcc.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/vt.h>
#include <math.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>
#include <vector>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/backend/drm.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>
#include <wlr/xcursor.h>
}
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace seaside {

enum {
    SEAT_WAIT_MS = 2000,
    CURSOR_SIZE = default_cursor_size,
    MAX_CURSOR_IMAGE = 64,
    MAX_CLIENT_CURSOR_TEXTURE = 128,
    MAX_ACTIVE_OUTPUTS = 2,
    MAX_KMS_CARDS = 64,
    MAX_KMS_PLANES = 256,
    MAX_PLANE_PROPERTIES = 64,
};

struct TtyDrmState {
    struct wl_display *display;
    struct wlr_session *session;
    struct wlr_backend *backend;
    struct wl_list outputs;
    struct tty_output *pointer_output;
    TtyDrmObserver *observer;
    struct wl_listener session_destroy;
    struct wl_listener session_active;
    struct wl_listener backend_destroy;
    struct wl_listener new_output;
    TtyDrmObservation evidence;
    struct wlr_buffer *visible_buffer;
    struct wlr_buffer *transparent_buffer;
    struct wlr_xcursor_theme *cursor_theme;
    struct wlr_buffer *client_cursor;
    int cursor_hotspot_x, cursor_hotspot_y;
    uint64_t cursor_generation;
    double cursor_x;
    double cursor_y;
    bool cursor_visible;
    bool client_cursor_hidden;
    bool cursor_position_set;
    bool fixture_mode;
};
static_assert(std::is_standard_layout_v<TtyDrmState>);

struct tty_output {
    TtyDrmState *owner;
    struct wlr_output *output;
    struct wlr_output_cursor *cursor;
    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wl_listener present;
    bool pending;
    bool parked;
    bool rejected;
    bool usable;
    bool cursor_visible;
    bool cursor_presented;
    bool commit_pending;
    bool movement_verified;
    bool render_locked;
    bool sleeping;
    DrmOutputSettings sleep_settings;
    uint32_t connector_id;
    uint32_t cursor_plane_id;
    uint32_t commit_seq;
    uint64_t cursor_generation;
    int x, y, width, height;
    uint64_t committed_frames, presented_frames, discarded_frames, last_presentation_ns;
    const char *disabled_reason;
    int committed_x;
    int committed_y;
    int presented_x;
    int presented_y;
};
static_assert(std::is_standard_layout_v<tty_output>);

struct cursor_buffer {
    struct wlr_buffer base;
    std::vector<uint32_t> pixels;
};
static_assert(std::is_standard_layout_v<cursor_buffer>);

static void destroy_cursor_buffer(struct wlr_buffer *base) noexcept {
    struct cursor_buffer *buffer = wl_container_of(base, buffer, base);
    wlr_buffer_finish(base);
    delete buffer;
}

static bool read_cursor_buffer(struct wlr_buffer *base, uint32_t flags, void **data,
                               uint32_t *format, size_t *stride) noexcept {
    struct cursor_buffer *buffer = wl_container_of(base, buffer, base);
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    *data = buffer->pixels.data();
    *format = DRM_FORMAT_ARGB8888;
    *stride = (size_t)base->width * sizeof(buffer->pixels[0]);
    return true;
}

static void end_cursor_buffer_access(struct wlr_buffer *base) noexcept {
    (void)base;
}

static const struct wlr_buffer_impl cursor_buffer_impl = {
    .destroy = destroy_cursor_buffer,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = read_cursor_buffer,
    .end_data_ptr_access = end_cursor_buffer_access,
};

static struct cursor_buffer *new_cursor_buffer(int width, int height) noexcept {
    if (width <= 0 || height <= 0 || width > MAX_CURSOR_IMAGE || height > MAX_CURSOR_IMAGE) {
        return nullptr;
    }
    try {
        auto buffer = std::make_unique<cursor_buffer>();
        buffer->pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
        wlr_buffer_init(&buffer->base, &cursor_buffer_impl, width, height);
        return buffer.release();
    } catch (...) {
        return nullptr;
    }
}

static struct wlr_buffer *make_cursor_buffer(bool visible, bool fixture) {
    struct cursor_buffer *buffer = new_cursor_buffer(CURSOR_SIZE, CURSOR_SIZE);
    if (!buffer) {
        return nullptr;
    }
    if (visible && fixture) {
        /* Keep the qualification fixture independent of the desktop artwork. */
        for (size_t i = 0; i < CURSOR_SIZE; ++i) {
            buffer->pixels[CURSOR_SIZE * i + CURSOR_SIZE / 2] = UINT32_C(0xffffffff);
            buffer->pixels[CURSOR_SIZE * (CURSOR_SIZE / 2) + i] = UINT32_C(0xffffffff);
        }
    } else if (visible) {
        /* The arrow tip at (0, 0) is also its hardware hotspot. */
        static const char arrow[CURSOR_SIZE][CURSOR_SIZE + 1] = {
            "W...............", "WW..............", "WBW.............", "WBBW............",
            "WBBBW...........", "WBBBBW..........", "WBBBBBW.........", "WBBBBBBW........",
            "WBBBBBBBW.......", "WBBBBBBBBW......", "WBBBBBWWWWW.....", "WBBWBBW.........",
            "WBW.WBBW........", "WW..WBBW........", "W....WW.........", "................",
        };
        for (size_t y = 0; y < CURSOR_SIZE; ++y) {
            for (size_t x = 0; x < CURSOR_SIZE; ++x) {
                const char pixel = arrow[y][x];
                buffer->pixels[y * CURSOR_SIZE + x] = pixel == 'W'   ? UINT32_C(0xffffffff)
                                                      : pixel == 'B' ? UINT32_C(0xff000000)
                                                                     : 0;
            }
        }
    }
    return &buffer->base;
}

static struct cursor_buffer *copy_cursor_surface(struct wlr_surface *surface) {
    const int width = surface->current.width;
    const int height = surface->current.height;
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture || width <= 0 || height <= 0 || width > MAX_CURSOR_IMAGE ||
        height > MAX_CURSOR_IMAGE || texture->width == 0 || texture->height == 0 ||
        texture->width > MAX_CLIENT_CURSOR_TEXTURE || texture->height > MAX_CLIENT_CURSOR_TEXTURE) {
        return nullptr;
    }
    struct wlr_linux_drm_syncobj_surface_v1_state *sync =
        wlr_linux_drm_syncobj_v1_get_surface_state(surface);
    if (sync && sync->acquire_timeline) {
        bool ready = false;
        if (!wlr_drm_syncobj_timeline_check(sync->acquire_timeline, sync->acquire_point, 0,
                                            &ready) ||
            !ready) {
            return nullptr;
        }
    }
    uint32_t pixels[MAX_CLIENT_CURSOR_TEXTURE * MAX_CLIENT_CURSOR_TEXTURE];
    uint32_t format = wlr_texture_preferred_read_format(texture);
    if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_ABGR8888 &&
        format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_XBGR8888) {
        format = DRM_FORMAT_ARGB8888;
    }
    wlr_texture_read_pixels_options read_options{};
    read_options.data = pixels;
    read_options.format = format;
    read_options.stride = texture->width * static_cast<uint32_t>(sizeof(pixels[0]));
    if (!wlr_texture_read_pixels(texture, &read_options)) {
        return nullptr;
    }
    struct wlr_fbox source;
    wlr_surface_get_buffer_source_box(surface, &source);
    if (!(source.x >= 0 && source.y >= 0 && source.width > 0 && source.height > 0 &&
          source.x + source.width <= texture->width &&
          source.y + source.height <= texture->height)) {
        return nullptr;
    }
    struct cursor_buffer *image = new_cursor_buffer(width, height);
    if (!image) {
        return nullptr;
    }
    const enum wl_output_transform transform =
        wlr_output_transform_invert(surface->current.transform);
    const bool bgr = format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
    const bool opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            wlr_fbox point{
                .x = (x + 0.5) / width, .y = (y + 0.5) / height, .width = 0, .height = 0};
            wlr_fbox_transform(&point, &point, transform, 1, 1);
            const unsigned sx = (unsigned)(source.x + point.x * source.width);
            const unsigned sy = (unsigned)(source.y + point.y * source.height);
            uint32_t pixel = pixels[(size_t)sy * texture->width + sx];
            if (bgr) {
                pixel = (pixel & UINT32_C(0xff00ff00)) | ((pixel & UINT32_C(0xff0000)) >> 16) |
                        ((pixel & UINT32_C(0xff)) << 16);
            }
            image->pixels[(size_t)y * width + x] = opaque ? pixel | UINT32_C(0xff000000) : pixel;
        }
    }
    return image;
}

static void unlink_listener(struct wl_listener *listener) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
}

static void fail(TtyDrmState *drm, const char *message) {
    if (!drm->evidence.failed) {
        fprintf(stderr, "TTY DRM: %s\n", message);
        drm->evidence.failed = true;
    }
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return -1;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static std::atomic_bool drm_seat_paused{};
static std::atomic_int_fast64_t drm_log_start_ms{};

static void log_wlroots(enum wlr_log_importance importance, const char *format,
                        va_list args) noexcept {
    /* wlroots 0.20.2 destroys outputs on seat pause. Its CRTC disable returns
     * before reaching KMS when the seat is inactive, then emits this error. */
    if (importance == WLR_ERROR && drm_seat_paused.load(std::memory_order_relaxed) &&
        strcmp(format, "[%s:%d] connector %s: Failed to disable CRTC %" PRIu32) == 0) {
        va_list copy;
        va_copy(copy, args);
        const char *file = va_arg(copy, const char *);
        va_end(copy);
        const char source[] = "backend/drm/drm.c";
        const size_t length = strlen(file);
        if (strcmp(file, source) == 0 ||
            (length >= sizeof(source) && file[length - sizeof(source)] == '/' &&
             strcmp(file + length - sizeof(source) + 1, source) == 0)) {
            importance = WLR_INFO;
            format =
                "[%s:%d] connector %s: Skipped disabling CRTC %" PRIu32 " while seat is paused";
        }
    }
    if ((unsigned)importance > (unsigned)wlr_log_get_verbosity()) {
        return;
    }
    static constexpr struct {
        const char *label, *color;
    } levels[] = {
        {"", ""},
        {"[ERROR] ", "\033[1;31m"},
        {"[INFO] ", "\033[1;34m"},
        {"[DEBUG] ", "\033[1;90m"},
    };
    static_assert(WLR_SILENT == 0 && WLR_ERROR == 1 && WLR_INFO == 2 && WLR_DEBUG == 3);
    const int64_t started = drm_log_start_ms.load(std::memory_order_relaxed);
    const int64_t now = monotonic_ms();
    const int64_t ms = started >= 0 && now >= started ? now - started : 0;
    const bool color = isatty(STDERR_FILENO);
    flockfile(stderr);
    fprintf(stderr, "%02d:%02d:%02d.%03d %s", (int)(ms / 3600000), (int)(ms / 60000 % 60),
            (int)(ms / 1000 % 60), (int)(ms % 1000),
            color ? levels[importance].color : levels[importance].label);
    vfprintf(stderr, format, args);
    fputs(color ? "\033[0m\n" : "\n", stderr);
    funlockfile(stderr);
}

static void on_session_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyDrmState *drm = wl_container_of(listener, drm, session_destroy);
    if (drm->backend) {
        fail(drm, "seat session ended before the DRM backend stopped");
    }
    unlink_listener(&drm->session_destroy);
    unlink_listener(&drm->session_active);
    drm->session = nullptr;
    drm->evidence.active = false;
    drm_seat_paused.store(false, std::memory_order_relaxed);
}

static void on_session_active(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyDrmState *drm = wl_container_of(listener, drm, session_active);
    drm->evidence.active = drm->session->active;
    drm_seat_paused.store(!drm->session->active, std::memory_order_relaxed);
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

static void on_backend_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    TtyDrmState *drm = wl_container_of(listener, drm, backend_destroy);
    unlink_listener(&drm->backend_destroy);
    unlink_listener(&drm->new_output);
    drm->backend = nullptr;
}

static char *output_identity(const struct wlr_output *output);

static void observe_output(const struct tty_output *view, bool connected) {
    const TtyDrmState *drm = view->owner;
    if (drm->observer) {
        char *identity = output_identity(view->output);
        drm->observer->output_observed(view->output, view->output->name, identity, connected,
                                       view->disabled_reason);
        free(identity);
    }
}

static void remove_usable_output(struct tty_output *view) {
    TtyDrmState *drm = view->owner;
    const bool notify = view->usable;
    if (view->render_locked) {
        wlr_output_lock_attach_render(view->output, false);
        view->render_locked = false;
    }
    view->usable = false;
    if (drm->pointer_output == view) {
        drm->pointer_output = nullptr;
    }
    unlink_listener(&view->commit);
    unlink_listener(&view->present);
    if (notify && drm->observer) {
        drm->observer->output_removed(view->output);
    }
}

static void destroy_output_view(struct tty_output *view) {
    remove_usable_output(view);
    unlink_listener(&view->destroy);
    wl_list_remove(&view->link);
    delete view;
}

static void on_output_destroy(struct wl_listener *listener, void *data) noexcept {
    (void)data;
    struct tty_output *view = wl_container_of(listener, view, destroy);
    /* wlroots destroys the cursor after emitting the output destroy event. */
    remove_usable_output(view);
    observe_output(view, false);
    destroy_output_view(view);
}

static void on_output_commit(struct wl_listener *listener, void *data) noexcept;
static void on_output_present(struct wl_listener *listener, void *data) noexcept;

static void on_new_output(struct wl_listener *listener, void *data) noexcept {
    TtyDrmState *drm = wl_container_of(listener, drm, new_output);
    auto *output = static_cast<wlr_output *>(data);
    if (drm->evidence.failed || !wlr_output_is_drm(output) || output->non_desktop ||
        wl_list_empty(&output->modes)) {
        return;
    }
    struct tty_output *view = new (std::nothrow) tty_output{};
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
    view->cursor = nullptr;
    view->disabled_reason = reason;
    view->rejected = true;
    view->cursor_presented = false;
    view->commit_pending = false;
}

static void on_output_commit(struct wl_listener *listener, void *data) noexcept {
    struct tty_output *view = wl_container_of(listener, view, commit);
    TtyDrmState *drm = view->owner;
    auto *event = static_cast<const wlr_output_event_commit *>(data);
    if (drm->evidence.failed || !drm->evidence.active || view->rejected || view->sleeping ||
        !view->output->enabled || (event->state->committed & WLR_OUTPUT_STATE_BUFFER) == 0) {
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

static void on_output_present(struct wl_listener *listener, void *data) noexcept {
    struct tty_output *view = wl_container_of(listener, view, present);
    TtyDrmState *drm = view->owner;
    auto *event = static_cast<const wlr_output_event_present *>(data);
    if (!view->commit_pending || event->commit_seq != view->commit_seq) {
        return;
    }
    view->commit_pending = false;
    if (event->presented) {
        ++view->presented_frames;
        if (event->when.tv_sec >= 0 && event->when.tv_nsec >= 0) {
            view->last_presentation_ns =
                (uint64_t)event->when.tv_sec * 1000000000 + (uint64_t)event->when.tv_nsec;
        }
    } else {
        ++view->discarded_frames;
    }
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
    if (view == drm->pointer_output && view->cursor_visible &&
        view->cursor_generation == drm->cursor_generation && drm->observer) {
        drm->observer->cursor_presented(view->output, &event->when);
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

bool TtyDrm::active_console() noexcept {
    char path[64];
    struct vt_stat active = {};
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
    const unsigned long vt = strtoul(digits, nullptr, 10);
    return errno == 0 && vt > 0 && tcgetpgrp(STDIN_FILENO) == getpgrp() &&
           ioctl(STDIN_FILENO, VT_GETSTATE, &active) == 0 && vt == active.v_active;
}

static bool wait_for_active_seat(TtyDrmState *drm, struct wl_event_loop *loop) {
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
        return nullptr;
    }
    for (ssize_t i = 1; i < count; ++i) {
        wlr_session_close_file(session, devices[i]);
    }
    return devices[0];
}

void TtyDrmRendering::RendererDeleter::operator()(wlr_renderer *renderer) const noexcept {
    wlr_renderer_destroy(renderer);
}

void TtyDrmRendering::BackendDeleter::operator()(wlr_backend *backend) const noexcept {
    wlr_backend_destroy(backend);
}

TtyDrm::TtyDrm(std::unique_ptr<TtyDrmState> state) noexcept : state_(std::move(state)) {}

std::expected<std::unique_ptr<TtyDrm>, TtyDrmError>
TtyDrm::create(wl_display *display, const char *device_path) noexcept {
    if (!display) {
        return std::unexpected(TtyDrmError::invalid_display);
    }
    if (device_path && !primary_card_path(device_path)) {
        fprintf(stderr, "TTY DRM: missing or invalid primary DRM node: %s\n", device_path);
        return std::unexpected(TtyDrmError::invalid_device);
    }
    std::unique_ptr<TtyDrmState> state(new (std::nothrow) TtyDrmState{});
    if (!state) {
        fprintf(stderr, "TTY DRM: failed to allocate device owner\n");
        return std::unexpected(TtyDrmError::allocation);
    }
    TtyDrmState *drm = state.get();
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

    std::unique_ptr<TtyDrm> owner(new (std::nothrow) TtyDrm(std::move(state)));
    if (!owner) {
        return std::unexpected(TtyDrmError::allocation);
    }

    drm_log_start_ms.store(monotonic_ms(), std::memory_order_relaxed);
    wlr_log_init(WLR_INFO, log_wlroots);
    struct wl_event_loop *loop = wl_display_get_event_loop(display);
    drm->session = wlr_session_create(loop);
    if (!drm->session) {
        fail(drm, "libseat could not acquire the console session");
        return std::unexpected(TtyDrmError::session);
    }
    wl_signal_add(&drm->session->events.destroy, &drm->session_destroy);
    /* Observe the seat before the DRM backend's VT teardown listener. */
    wl_signal_add(&drm->session->events.active, &drm->session_active);
    if (!wait_for_active_seat(drm, loop)) {
        fail(drm, "console seat did not become active");
        return std::unexpected(TtyDrmError::session);
    }
    drm->evidence.active = true;

    owner->rendering_.backend.reset(wlr_multi_backend_create(loop));
    drm->backend = owner->rendering_.backend.get();
    if (!drm->backend) {
        fail(drm, "could not create the combined DRM and libinput backend");
        return std::unexpected(TtyDrmError::backend);
    }
    wl_signal_add(&drm->backend->events.destroy, &drm->backend_destroy);
    wl_signal_add(&drm->backend->events.new_output, &drm->new_output);

    struct wlr_device *device = find_primary_device(drm->session, device_path);
    if (!device || drmGetNodeTypeFromFd(device->fd) != DRM_NODE_PRIMARY) {
        fail(drm, "libseat could not open the selected primary DRM node");
        return std::unexpected(TtyDrmError::invalid_device);
    }
    if (!add_backend(drm->backend, wlr_drm_backend_create(drm->session, device, nullptr))) {
        fail(drm, "could not create the DRM backend for the selected device");
        return std::unexpected(TtyDrmError::backend);
    }
    if (!add_backend(drm->backend, wlr_libinput_backend_create(drm->session))) {
        fail(drm, "could not create the libinput backend for the active seat");
        return std::unexpected(TtyDrmError::backend);
    }
    owner->rendering_.renderer.reset(wlr_renderer_autocreate(drm->backend));
    if (!owner->rendering_.renderer) {
        fail(drm, "wlroots could not select a DRM renderer");
        return std::unexpected(TtyDrmError::renderer);
    }
    return owner;
}

TtyDrm::~TtyDrm() noexcept {
    TtyDrmState *drm = state_.get();
    drm->observer = nullptr;
    // The backend owns its opened DRM device on success. A session whose
    // backend could not be created closes its outstanding device on teardown.
    rendering_.backend.reset();
    rendering_.renderer.reset();
    unlink_listener(&drm->backend_destroy);
    unlink_listener(&drm->new_output);
    struct wl_list *link = drm->outputs.next;
    while (link != &drm->outputs) {
        struct tty_output *view = wl_container_of(link, view, link);
        link = link->next;
        wlr_output_cursor_destroy(view->cursor);
        destroy_output_view(view);
    }
    wlr_buffer_drop(drm->visible_buffer);
    wlr_buffer_drop(drm->transparent_buffer);
    wlr_buffer_unlock(drm->client_cursor);
    if (drm->cursor_theme) {
        wlr_xcursor_theme_destroy(drm->cursor_theme);
    }
    unlink_listener(&drm->session_destroy);
    unlink_listener(&drm->session_active);
    if (drm->session) {
        drm_seat_paused.store(false, std::memory_order_relaxed);
    }
    if (!drm->backend) {
        wlr_session_destroy(drm->session);
    }
}

TtyDrmRendering TtyDrm::take_rendering() noexcept {
    return std::move(rendering_);
}

static struct wlr_output_mode *initial_mode(struct wlr_output *output) {
    DrmOutputSettings settings = {};
    return select_drm_mode(*output, settings) ? settings.mode : nullptr;
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

void TtyDrm::set_observer(TtyDrmObserver *observer) noexcept {
    state_->observer = observer;
}

struct mode_rank {
    int32_t refresh;
    int64_t pixels;
};

static struct mode_rank maximum_mode_rank(const struct wlr_output *output) {
    struct mode_rank rank = {};
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

DrmOutputDescription describe_drm_output(wlr_output &output) noexcept {
    const struct wlr_output_mode *preferred = wlr_output_preferred_mode(&output);
    const struct mode_rank rank = maximum_mode_rank(&output);
    return DrmOutputDescription{
        .make = output.make,
        .model = output.model,
        .preferred_width = preferred ? preferred->width : 0,
        .preferred_height = preferred ? preferred->height : 0,
        .max_refresh_millihz = rank.refresh,
        .max_pixels = rank.pixels,
        .adaptive_sync_supported = output.adaptive_sync_supported,
    };
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

static unsigned usable_output_count(const TtyDrmState *drm) {
    unsigned count = 0;
    const struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->usable && !view->rejected) {
            ++count;
        }
    }
    return count;
}

static struct tty_output *next_pending(TtyDrmState *drm, struct wlr_output *preferred) {
    struct tty_output *next = nullptr;
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (!view->pending && !view->parked) {
            continue;
        }
        if (view->output == preferred) {
            return view;
        }
        if (ranks_first(view, next)) {
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
    view->cursor = nullptr;
    if (view->output->global) {
        wlr_output_destroy_global(view->output);
    }
    if (view->output->enabled && !set_output_enabled(view->output, false, nullptr)) {
        fprintf(stderr, "TTY DRM: could not disable output %s\n", view->output->name);
    }
    observe_output(view, true);
}

static bool prepare_cursor_buffers(TtyDrmState *drm) {
    if (!drm->visible_buffer) {
        drm->visible_buffer = make_cursor_buffer(true, drm->fixture_mode);
    }
    if (!drm->transparent_buffer) {
        drm->transparent_buffer = make_cursor_buffer(false, drm->fixture_mode);
    }
    if (!drm->visible_buffer || !drm->transparent_buffer) {
        fail(drm, "could not allocate the hardware cursor images");
        return false;
    }
    return true;
}

static bool cursor_should_show(const TtyDrmState *drm) {
    return drm->cursor_visible && !drm->client_cursor_hidden;
}

static struct cursor_buffer *scale_cursor_buffer(struct wlr_buffer *buffer, double scale) {
    const int width = (int)ceil(buffer->width * scale);
    const int height = (int)ceil(buffer->height * scale);
    struct cursor_buffer *image = new_cursor_buffer(width, height);
    if (!image) {
        return nullptr;
    }
    void *data;
    uint32_t format;
    size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format,
                                          &stride)) {
        wlr_buffer_drop(&image->base);
        return nullptr;
    }
    if (format != DRM_FORMAT_ARGB8888) {
        wlr_buffer_end_data_ptr_access(buffer);
        wlr_buffer_drop(&image->base);
        return nullptr;
    }
    for (int y = 0; y < height; ++y) {
        const unsigned sy = (unsigned)(y * buffer->height / height);
        const unsigned char *row = (const unsigned char *)data + sy * stride;
        for (int x = 0; x < width; ++x) {
            const unsigned sx = (unsigned)(x * buffer->width / width);
            memcpy(&image->pixels[(size_t)y * width + x], row + sx * sizeof(image->pixels[0]),
                   sizeof(image->pixels[0]));
        }
    }
    wlr_buffer_end_data_ptr_access(buffer);
    return image;
}

static bool stage_cursor_buffer(struct tty_output *view, struct wlr_buffer *buffer, int hotspot_x,
                                int hotspot_y) {
    const double scale = view->output->scale;
    if (!view->owner->fixture_mode && scale != 1.0) {
        struct cursor_buffer *scaled = scale_cursor_buffer(buffer, scale);
        if (scaled) {
            const bool displayed = wlr_output_cursor_set_buffer(view->cursor, &scaled->base,
                                                                (int32_t)ceil(hotspot_x * scale),
                                                                (int32_t)ceil(hotspot_y * scale)) &&
                                   has_hardware_cursor(view);
            wlr_buffer_drop(&scaled->base);
            if (displayed) {
                return true;
            }
        }
    }
    return buffer->width >= scale && buffer->height >= scale &&
           wlr_output_cursor_set_buffer(view->cursor, buffer, hotspot_x, hotspot_y) &&
           has_hardware_cursor(view);
}

static bool load_cursor_image(struct tty_output *view, bool visible) {
    const TtyDrmState *drm = view->owner;
    const int hotspot = drm->fixture_mode ? CURSOR_SIZE / 2 : 0;
    if (!visible || !drm->client_cursor ||
        !stage_cursor_buffer(view, drm->client_cursor, drm->cursor_hotspot_x,
                             drm->cursor_hotspot_y)) {
        // An unsupported client image must never force software cursor presentation
        // or disable a display that can show the built-in hardware cursor.
        struct wlr_buffer *buffer = visible ? drm->visible_buffer : drm->transparent_buffer;
        if (!stage_cursor_buffer(view, buffer, hotspot, hotspot)) {
            return false;
        }
    }
    view->cursor_visible = visible;
    view->cursor_generation = drm->cursor_generation;
    return true;
}

static char *output_identity(const struct wlr_output *output) {
    if (!output->serial || !*output->serial) {
        return nullptr;
    }
    const char *make = output->make ? output->make : "";
    const char *model = output->model ? output->model : "";
    int length = snprintf(nullptr, 0, "%s %s %s", make, model, output->serial);
    if (length < 0) {
        return nullptr;
    }
    char *identity = static_cast<char *>(malloc(static_cast<size_t>(length) + 1));
    if (identity) {
        snprintf(identity, (size_t)length + 1, "%s %s %s", make, model, output->serial);
    }
    return identity;
}

static bool enable_output(struct tty_output *view, struct wlr_allocator *allocator,
                          struct wlr_renderer *renderer) {
    TtyDrmState *drm = view->owner;
    struct wlr_output *output = view->output;
    view->pending = false;
    view->parked = false;
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
        !load_cursor_image(view, initial_pointer && cursor_should_show(drm))) {
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
    view->disabled_reason = nullptr;
    wl_signal_add(&output->events.commit, &view->commit);
    wl_signal_add(&output->events.present, &view->present);
    if (drm->observer &&
        !drm->observer->output_added(output, view->x, view->y, view->width, view->height)) {
        fail(drm, "could not attach an enabled DRM output");
        return false;
    }
    fprintf(stderr, "TTY DRM: enabled %s (%dx%d at %d mHz, connector %u)\n", output->name,
            output->width, output->height, output->refresh, view->connector_id);
    return true;
}

static bool available(const struct tty_output *view) {
    return view && view->usable && !view->rejected;
}

static struct seaside_cursor_box cursor_box(const struct tty_output *view) {
    return seaside_cursor_box{view->x, view->y, view->width, view->height};
}

static struct tty_output *nearest_output(const TtyDrmState *drm, double x, double y,
                                         double *result_x, double *result_y) {
    struct tty_output *selected = nullptr;
    double best_distance = 0;
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (!available(view)) {
            continue;
        }
        const struct seaside_cursor_point inside =
            seaside_cursor_clamp(cursor_box(view), seaside_cursor_point{x, y});
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

static bool move_cursor(TtyDrmState *drm, double x, double y, bool count_move) {
    double next_x = x, next_y = y;
    struct tty_output *target = nearest_output(drm, x, y, &next_x, &next_y);
    if (!target) {
        return false;
    }
    struct tty_output *previous = drm->pointer_output;
    if (previous != target && available(previous) && !previous->sleeping &&
        previous->cursor_visible && !load_cursor_image(previous, false)) {
        reject_output(previous, "could not hide hardware cursor while leaving output");
    }
    drm->pointer_output = target;
    if (!target->sleeping) {
        const double local_x = next_x - target->x;
        const double local_y = next_y - target->y;
        const double scale = target->output->scale;
        const bool pixel_changed = (int)(local_x * scale) != (int)target->cursor->x ||
                                   (int)(local_y * scale) != (int)target->cursor->y;
        const bool visible = cursor_should_show(drm);
        if ((pixel_changed && !wlr_output_cursor_move(target->cursor, local_x, local_y)) ||
            !has_hardware_cursor(target) ||
            ((target->cursor_visible != visible ||
              (visible && target->cursor_generation != drm->cursor_generation)) &&
             !load_cursor_image(target, visible))) {
            reject_output(target, "the backend rejected hardware cursor motion");
            return false;
        }
    }
    if (count_move && ((int)drm->cursor_x != (int)next_x || (int)drm->cursor_y != (int)next_y)) {
        ++drm->evidence.cursor_moves;
    }
    drm->cursor_x = next_x;
    drm->cursor_y = next_y;
    return true;
}

static struct tty_output *find_output(TtyDrmState *drm, struct wlr_output *output) {
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->output == output) {
            return view;
        }
    }
    return nullptr;
}

static DrmOutputSettings current_settings(const struct tty_output *view) {
    const struct wlr_output *output = view->output;
    return DrmOutputSettings{
        .mode = output->current_mode,
        .width = output->width,
        .height = output->height,
        .refresh_millihz = output->refresh,
        .x = view->x,
        .y = view->y,
        .rotation = 90 * (int)output->transform,
        .scale = output->scale,
        .adaptive_sync = output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED,
    };
}

bool TtyDrm::output_status(wlr_output &output, DrmOutputStatus &status) noexcept {
    auto *drm = state_.get();
    struct tty_output *view = find_output(drm, &output);
    if (!view) {
        return false;
    }
    const DrmOutputSettings applied = current_settings(view);
    status = DrmOutputStatus{
        .settings = view->sleeping ? view->sleep_settings : applied,
        .last_mode = {applied.width, applied.height, applied.refresh_millihz},
        .logical_width = view->width,
        .logical_height = view->height,
        .sleeping = view->sleeping,
        .cursor = has_hardware_cursor(view),
        .adaptive_sync =
            !view->sleeping && output.adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED,
        .frame_pending = view->commit_pending,
        .committed_frames = view->committed_frames,
        .presented_frames = view->presented_frames,
        .discarded_frames = view->discarded_frames,
        .last_presentation_ns = view->last_presentation_ns,
    };
    return true;
}

bool select_drm_mode(wlr_output &output, DrmOutputSettings &settings) noexcept {
    const struct wlr_output_mode *preferred = wlr_output_preferred_mode(&output);
    if (!preferred) {
        return false;
    }
    const int width = settings.width ? settings.width : preferred->width;
    const int height = settings.height ? settings.height : preferred->height;
    struct wlr_output_mode *mode, *selected = nullptr;
    int64_t best_distance = 500; // Half a Hz covers nominal rates from EDID timings.
    wl_list_for_each(mode, &output.modes, link) {
        if (mode->width != width || mode->height != height) {
            continue;
        }
        const int64_t distance =
            settings.refresh_millihz ? llabs((int64_t)mode->refresh - settings.refresh_millihz) : 0;
        if (distance < best_distance ||
            (distance == best_distance && (!selected || selected->refresh < mode->refresh))) {
            selected = mode;
            best_distance = distance;
        }
    }
    if (!selected) {
        return false;
    }
    settings.mode = selected;
    settings.width = width;
    settings.height = height;
    settings.refresh_millihz = selected->refresh;
    return true;
}

static bool configure_output(struct wlr_output *output, const DrmOutputSettings &settings,
                             bool test, bool wake) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    if (output->current_mode != settings.mode) {
        wlr_output_state_set_mode(&state, settings.mode);
    }
    if (output->scale != settings.scale) {
        wlr_output_state_set_scale(&state, settings.scale);
    }
    const auto transform = static_cast<wl_output_transform>(settings.rotation / 90);
    if (output->transform != transform) {
        wlr_output_state_set_transform(&state, transform);
    }
    if ((output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED) !=
        settings.adaptive_sync) {
        wlr_output_state_set_adaptive_sync_enabled(&state, settings.adaptive_sync);
    }
    if (wake || (test && !output->enabled && state.committed)) {
        // A dry-run uses an enabled state to validate dormant mode changes.
        // wlroots rejects MODE/ADAPTIVE_SYNC settings on a disabled commit.
        wlr_output_state_set_enabled(&state, true);
    }
    const bool ok = !state.committed || (test ? wlr_output_test_state(output, &state)
                                              : wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);
    return ok;
}

DrmOutputDescription TtyDrm::describe_output(wlr_output &output) const noexcept {
    return describe_drm_output(output);
}

bool TtyDrm::select_mode(wlr_output &output, DrmOutputSettings &settings) const noexcept {
    return select_drm_mode(output, settings);
}

bool TtyDrm::check_settings(wlr_output &output, const DrmOutputSettings &settings) const noexcept {
    return configure_output(&output, settings, true, false);
}

bool TtyDrm::apply_settings(wlr_output &output, const DrmOutputSettings &settings) noexcept {
    auto *drm = state_.get();
    struct tty_output *view = find_output(drm, &output);
    if (!available(view)) {
        return false;
    }
    if (view->sleeping) {
        view->sleep_settings = settings;
        view->x = settings.x;
        view->y = settings.y;
        const bool rotated = settings.rotation == 90 || settings.rotation == 270;
        view->width = (int)((float)(rotated ? settings.height : settings.width) / settings.scale);
        view->height = (int)((float)(rotated ? settings.width : settings.height) / settings.scale);
        return true;
    }
    const bool geometry_changed =
        output.current_mode != settings.mode || output.scale != settings.scale ||
        output.transform != (enum wl_output_transform)(settings.rotation / 90);
    if (!configure_output(&output, settings, false, false)) {
        return false;
    }
    if (geometry_changed) {
        view->cursor_presented = false;
        view->movement_verified = false;
        view->cursor_plane_id = 0;
        view->cursor_generation = drm->cursor_generation - 1;
    }
    view->x = settings.x;
    view->y = settings.y;
    wlr_output_effective_resolution(&output, &view->width, &view->height);
    wlr_output_schedule_frame(&output);
    return true;
}

bool TtyDrm::set_output_power(wlr_output *output, bool on) noexcept {
    auto *drm = state_.get();
    struct tty_output *view = find_output(drm, output);
    if (!available(view) || !drm->evidence.active) {
        return false;
    }
    const bool sleep = !on;
    if (sleep == view->sleeping) {
        return true;
    }
    if (on) {
        DrmOutputSettings settings = view->sleep_settings;
        bool enabled = configure_output(output, settings, false, true);
        if (!enabled && settings.adaptive_sync) {
            settings.adaptive_sync = false;
            enabled = configure_output(output, settings, false, true);
            if (enabled) {
                fprintf(stderr, "TTY DRM: %s resumed with fixed refresh\n", output->name);
            }
        }
        if (!enabled) {
            fprintf(stderr, "TTY DRM: %s rejected display wake\n", output->name);
            return false;
        }
        view->sleeping = false;
        const bool pointer_here = drm->pointer_output == view;
        const double x = pointer_here ? drm->cursor_x - view->x : view->width / 2;
        const double y = pointer_here ? drm->cursor_y - view->y : view->height / 2;
        view->cursor = wlr_output_cursor_create(output);
        if (!view->cursor || !wlr_output_cursor_move(view->cursor, x, y) ||
            !load_cursor_image(view, pointer_here && cursor_should_show(drm))) {
            reject_output(view, "hardware cursor staging failed after display wake");
            return false;
        }
        wlr_output_schedule_frame(output);
    } else {
        view->sleep_settings = current_settings(view);
        if (!set_output_enabled(output, false, nullptr)) {
            fprintf(stderr, "TTY DRM: %s rejected display sleep\n", output->name);
            return false;
        }
        view->sleeping = true;
        view->cursor_presented = false;
        view->movement_verified = false;
        view->cursor_plane_id = 0;
        view->commit_pending = false;
        wlr_output_cursor_destroy(view->cursor);
        view->cursor = nullptr;
    }
    fprintf(stderr, "TTY DRM: output %s display power %s\n", output->name, on ? "on" : "off");
    return true;
}

void TtyDrm::set_direct_scanout(wlr_output &output, bool enabled) noexcept {
    auto *drm = state_.get();
    struct tty_output *view = find_output(drm, &output);
    if (available(view) && view->render_locked == enabled) {
        wlr_output_lock_attach_render(&output, !enabled);
        view->render_locked = !enabled;
        wlr_output_schedule_frame(&output);
    }
}

void TtyDrm::reject_unrestorable(wlr_output &output) noexcept {
    auto *drm = state_.get();
    struct tty_output *view = find_output(drm, &output);
    if (available(view)) {
        reject_output(view, "could not restore output state after a failed config change");
    }
}

void TtyDrm::reconcile_cursor() noexcept {
    auto *drm = state_.get();
    if (drm->evidence.active) {
        move_cursor(drm, drm->cursor_x, drm->cursor_y, false);
    }
}

void TtyDrm::reject_unpresented_cursors() noexcept {
    auto *drm = state_.get();
    if (!drm->evidence.active) {
        return;
    }
    struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (available(view) && !view->sleeping && !view->cursor_presented) {
            reject_output(view, "hardware cursor presentation timed out");
        }
    }
}

bool TtyDrm::enable_pending(wlr_allocator *allocator, wlr_renderer *renderer,
                            wlr_output *preferred) noexcept {
    auto *drm = state_.get();
    if (!allocator || !renderer || drm->evidence.failed) {
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
    unsigned active = usable_output_count(drm);
    while (active < MAX_ACTIVE_OUTPUTS && (view = next_pending(drm, preferred))) {
        if (!prepare_cursor_buffers(drm)) {
            return false;
        }
        if (!enable_output(view, allocator, renderer)) {
            disable_output(view);
            if (drm->evidence.failed) {
                return false;
            }
        } else {
            ++active;
        }
    }
    wl_list_for_each(view, &drm->outputs, link) {
        if (view->pending) {
            view->pending = false;
            view->parked = true;
            view->disabled_reason = "two-display limit";
            disable_output(view);
        }
    }
    if (!available(drm->pointer_output)) {
        move_cursor(drm, drm->cursor_x, drm->cursor_y, false);
    }
    return true;
}

wlr_output *TtyDrm::enable(wlr_allocator *allocator, wlr_renderer *renderer) noexcept {
    auto *drm = state_.get();
    if (!drm->evidence.active || drm->evidence.failed || drm->pointer_output || !allocator ||
        !renderer) {
        return nullptr;
    }
    drm->fixture_mode = true;
    struct tty_output *view = next_pending(drm, nullptr);
    if (!view) {
        fail(drm, "DRM backend announced no connected desktop output with modes");
        return nullptr;
    }
    if (!prepare_cursor_buffers(drm)) {
        return nullptr;
    }
    if (!enable_output(view, allocator, renderer)) {
        disable_output(view);
        return nullptr;
    }
    return view->output;
}

bool TtyDrm::cursor_position(double &x, double &y) const noexcept {
    const auto *drm = state_.get();
    if (!drm->evidence.active || !available(drm->pointer_output)) {
        return false;
    }
    x = drm->cursor_x;
    y = drm->cursor_y;
    return true;
}

bool TtyDrm::absolute_delta(double x, double y, double &dx, double &dy) const noexcept {
    const auto *drm = state_.get();
    if (!drm->evidence.active || !available(drm->pointer_output) || !isfinite(x) || !isfinite(y)) {
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
    dx = left + clamp(x, 0, 1) * ((double)right - left) - drm->cursor_x;
    dy = top + clamp(y, 0, 1) * ((double)bottom - top) - drm->cursor_y;
    return true;
}

static bool refresh_cursor_image(TtyDrmState *drm) {
    struct tty_output *view = drm->pointer_output;
    const bool visible = cursor_should_show(drm);
    if (drm->evidence.active && available(view) && !view->sleeping &&
        (view->cursor_visible != visible ||
         (visible && view->cursor_generation != drm->cursor_generation)) &&
        !load_cursor_image(view, visible)) {
        reject_output(view, "could not update the hardware cursor");
        return false;
    }
    return true;
}

bool TtyDrm::set_cursor_visible(bool visible) noexcept {
    auto *drm = state_.get();
    if (drm->evidence.failed) {
        return false;
    }
    drm->cursor_visible = visible;
    return refresh_cursor_image(drm);
}

static bool set_cursor_image(TtyDrmState *drm, struct wlr_buffer *buffer, int32_t hotspot_x,
                             int32_t hotspot_y, bool hidden) {
    if (drm->client_cursor != buffer || drm->cursor_hotspot_x != hotspot_x ||
        drm->cursor_hotspot_y != hotspot_y || drm->client_cursor_hidden != hidden) {
        if (buffer) {
            wlr_buffer_lock(buffer);
        }
        wlr_buffer_unlock(drm->client_cursor);
        drm->client_cursor = buffer;
        drm->cursor_hotspot_x = hotspot_x;
        drm->cursor_hotspot_y = hotspot_y;
        drm->client_cursor_hidden = hidden;
        ++drm->cursor_generation;
    }
    return refresh_cursor_image(drm);
}

static bool resize_cursor_ink(int x, int y, int direction) {
    const int dx = x - CURSOR_SIZE / 2;
    const int dy = y - CURSOR_SIZE / 2;
    int along = abs(dx);
    int across = abs(dy);
    if (direction == 1) {
        along = abs(dy);
        across = abs(dx);
    } else if (direction == 2) {
        along = abs(dx + dy) / 2;
        across = abs(dx - dy) / 2;
    } else if (direction == 3) {
        along = abs(dx - dy) / 2;
        across = abs(dx + dy) / 2;
    }
    return along <= 6 && (across <= 1 || (along >= 2 && across <= 6 - along));
}

static struct cursor_buffer *make_resize_cursor(const char *shape) {
    static const char *const names[][2] = {
        {"ew-resize", "col-resize"},
        {"ns-resize", "row-resize"},
        {"nwse-resize", nullptr},
        {"nesw-resize", nullptr},
    };
    for (int direction = 0; direction < 4; ++direction) {
        if (strcmp(shape, names[direction][0]) != 0 &&
            (!names[direction][1] || strcmp(shape, names[direction][1]) != 0)) {
            continue;
        }
        struct cursor_buffer *buffer = new_cursor_buffer(CURSOR_SIZE, CURSOR_SIZE);
        if (!buffer) {
            return nullptr;
        }
        // Keep the common bidirectional resize shapes available even when
        // the installed Xcursor theme provides only the legacy side shapes.
        for (int y = 0; y < CURSOR_SIZE; ++y) {
            for (int x = 0; x < CURSOR_SIZE; ++x) {
                uint32_t pixel = 0;
                if (resize_cursor_ink(x, y, direction)) {
                    pixel = UINT32_C(0xff000000);
                } else if (resize_cursor_ink(x - 1, y, direction) ||
                           resize_cursor_ink(x + 1, y, direction) ||
                           resize_cursor_ink(x, y - 1, direction) ||
                           resize_cursor_ink(x, y + 1, direction)) {
                    pixel = UINT32_C(0xffffffff);
                }
                buffer->pixels[CURSOR_SIZE * y + x] = pixel;
            }
        }
        return buffer;
    }
    return nullptr;
}

bool TtyDrm::set_cursor_shape(const char *shape) noexcept {
    auto *drm = state_.get();
    if (drm->evidence.failed || drm->fixture_mode) {
        return false;
    }
    if (shape && strcmp(shape, "default") != 0) {
        if (!drm->cursor_theme) {
            drm->cursor_theme = wlr_xcursor_theme_load(nullptr, CURSOR_SIZE);
        }
        struct wlr_xcursor *cursor =
            drm->cursor_theme ? wlr_xcursor_theme_get_cursor(drm->cursor_theme, shape) : nullptr;
        if (cursor && cursor->image_count > 0) {
            struct wlr_xcursor_image *image = cursor->images[0];
            if (image->width <= MAX_CURSOR_IMAGE && image->height <= MAX_CURSOR_IMAGE &&
                image->hotspot_x < image->width && image->hotspot_y < image->height) {
                return set_cursor_image(drm, wlr_xcursor_image_get_buffer(image),
                                        (int32_t)image->hotspot_x, (int32_t)image->hotspot_y,
                                        false);
            }
        }
        struct cursor_buffer *resize = make_resize_cursor(shape);
        if (resize) {
            const bool displayed =
                set_cursor_image(drm, &resize->base, CURSOR_SIZE / 2, CURSOR_SIZE / 2, false);
            wlr_buffer_drop(&resize->base);
            return displayed;
        }
    }
    return set_cursor_image(drm, nullptr, 0, 0, false);
}

bool TtyDrm::set_cursor_surface(wlr_surface *surface, int32_t hotspot_x,
                                int32_t hotspot_y) noexcept {
    auto *drm = state_.get();
    if (drm->evidence.failed || drm->fixture_mode) {
        return false;
    }
    if (!surface || !wlr_surface_has_buffer(surface)) {
        return set_cursor_image(drm, nullptr, 0, 0, true);
    }
    if (hotspot_x < -MAX_CURSOR_IMAGE || hotspot_x > MAX_CURSOR_IMAGE ||
        hotspot_y < -MAX_CURSOR_IMAGE || hotspot_y > MAX_CURSOR_IMAGE) {
        return set_cursor_shape(nullptr);
    }
    struct cursor_buffer *image = copy_cursor_surface(surface);
    if (!image) {
        return set_cursor_shape(nullptr);
    }
    const bool displayed = set_cursor_image(drm, &image->base, hotspot_x, hotspot_y, false);
    wlr_buffer_drop(&image->base);
    return displayed;
}

static struct seaside_cursor_point route_relative_cursor(const TtyDrmState *drm,
                                                         struct seaside_cursor_point target) {
    const struct seaside_cursor_box current = cursor_box(drm->pointer_output);
    if (seaside_cursor_contains(current, target)) {
        return target;
    }
    const struct seaside_cursor_point origin = {drm->cursor_x, drm->cursor_y};
    const struct tty_output *view;
    wl_list_for_each(view, &drm->outputs, link) {
        if (available(view) && view != drm->pointer_output) {
            const struct seaside_cursor_box destination = cursor_box(view);
            return seaside_cursor_route_relative(current, origin, target, &destination);
        }
    }
    return seaside_cursor_route_relative(current, origin, target, nullptr);
}

bool TtyDrm::move(seaside_pointer_motion kind, double x, double y) noexcept {
    auto *drm = state_.get();
    if (drm->evidence.failed || !drm->evidence.active || !available(drm->pointer_output) ||
        !isfinite(x) || !isfinite(y)) {
        return false;
    }
    if (kind == SEASIDE_POINTER_ABSOLUTE && !absolute_delta(x, y, x, y)) {
        return false;
    }
    struct seaside_cursor_point target = {drm->cursor_x + clamp(x, -INT_MAX, INT_MAX),
                                          drm->cursor_y + clamp(y, -INT_MAX, INT_MAX)};
    if (kind == SEASIDE_POINTER_RELATIVE) {
        target = route_relative_cursor(drm, target);
    }
    return move_cursor(drm, target.x, target.y, true);
}

bool TtyDrm::change_vt(unsigned vt) noexcept {
    auto *drm = state_.get();
    return drm->session && drm->session->active && wlr_session_change_vt(drm->session, vt);
}

TtyDrmObservation TtyDrm::observation() const noexcept {
    const auto *drm = state_.get();
    TtyDrmObservation result = drm->evidence;
    const struct tty_output *pointer = drm->pointer_output;
    const bool has_pointer = available(pointer);
    if (result.active && has_pointer) {
        result.connector_id = pointer->connector_id;
        result.cursor_plane_id = pointer->cursor_plane_id;
        result.cursor_presented = pointer->cursor_presented;
    }
    if (result.active) {
        const bool slot_available = usable_output_count(drm) < MAX_ACTIVE_OUTPUTS;
        struct tty_output *view;
        wl_list_for_each(view, &drm->outputs, link) {
            if (view->pending || view->rejected || (view->parked && slot_available) ||
                (!has_pointer && available(view))) {
                result.output_pending = true;
                break;
            }
        }
    }
    return result;
}

} // namespace seaside

// Let a Pixman capture pass complete at a point explicitly chosen by the test.
#include "capture_fence_fixture.h"

#include <stdlib.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>

static struct wlr_drm_syncobj_timeline fence;
static struct wlr_linux_drm_syncobj_surface_v1_state surface_state = {.acquire_timeline = &fence};
static struct wlr_drm_syncobj_timeline_waiter *pending;
static wlr_drm_syncobj_timeline_ready_callback completed;
static bool enabled, signalled;

static void require(bool condition) {
    if (!condition) {
        abort();
    }
}

void capture_test_enable_fences(struct wlr_renderer *renderer) {
    enabled = renderer->features.timeline = true;
}

void capture_test_disable_fences(struct wlr_renderer *renderer) {
    require(!pending);
    enabled = renderer->features.timeline = false;
}

bool capture_test_fence_pending(void) {
    return pending != NULL;
}

void capture_test_complete_fence(void) {
    require(pending);
    signalled = true;
    completed(pending);
}

struct wlr_linux_drm_syncobj_surface_v1_state *
__real_wlr_linux_drm_syncobj_v1_get_surface_state(struct wlr_surface *surface);
struct wlr_linux_drm_syncobj_surface_v1_state *
__wrap_wlr_linux_drm_syncobj_v1_get_surface_state(struct wlr_surface *surface) {
    return enabled ? &surface_state : __real_wlr_linux_drm_syncobj_v1_get_surface_state(surface);
}

bool __wrap_wlr_linux_drm_syncobj_v1_state_add_release_point(
    struct wlr_linux_drm_syncobj_surface_v1_state *state, struct wlr_drm_syncobj_timeline *timeline,
    uint64_t point, struct wl_event_loop *loop) {
    return enabled && state == &surface_state && timeline == &fence && point > 0 && loop;
}

struct wlr_drm_syncobj_timeline *__wrap_wlr_drm_syncobj_timeline_create(int fd) {
    require(enabled && fd == -1);
    return &fence;
}

void __real_wlr_drm_syncobj_timeline_unref(struct wlr_drm_syncobj_timeline *timeline);
void __wrap_wlr_drm_syncobj_timeline_unref(struct wlr_drm_syncobj_timeline *timeline) {
    if (timeline != &fence) {
        __real_wlr_drm_syncobj_timeline_unref(timeline);
    }
}

bool __wrap_wlr_drm_syncobj_timeline_waiter_init(struct wlr_drm_syncobj_timeline_waiter *waiter,
                                                 struct wlr_drm_syncobj_timeline *timeline,
                                                 uint64_t point, uint32_t flags,
                                                 struct wl_event_loop *loop,
                                                 wlr_drm_syncobj_timeline_ready_callback callback) {
    require(enabled && timeline == &fence && point && flags == 0 && loop && !pending);
    signalled = false;
    completed = callback;
    pending = waiter;
    return true;
}

void __wrap_wlr_drm_syncobj_timeline_waiter_finish(struct wlr_drm_syncobj_timeline_waiter *waiter) {
    require(waiter == pending);
    pending = NULL;
}

bool __wrap_wlr_drm_syncobj_timeline_check(struct wlr_drm_syncobj_timeline *timeline,
                                           uint64_t point, uint32_t flags, bool *result) {
    require(timeline == &fence && point && flags);
    *result = signalled;
    return true;
}

struct wlr_render_pass *
__real_wlr_renderer_begin_buffer_pass(struct wlr_renderer *renderer, struct wlr_buffer *buffer,
                                      const struct wlr_buffer_pass_options *options);
struct wlr_render_pass *
__wrap_wlr_renderer_begin_buffer_pass(struct wlr_renderer *renderer, struct wlr_buffer *buffer,
                                      const struct wlr_buffer_pass_options *options) {
    if (options && options->signal_timeline == &fence) {
        struct wlr_buffer_pass_options copy = *options;
        copy.signal_timeline = NULL;
        return __real_wlr_renderer_begin_buffer_pass(renderer, buffer, &copy);
    }
    return __real_wlr_renderer_begin_buffer_pass(renderer, buffer, options);
}

void __real_wlr_render_pass_add_texture(struct wlr_render_pass *pass,
                                        const struct wlr_render_texture_options *options);
void __wrap_wlr_render_pass_add_texture(struct wlr_render_pass *pass,
                                        const struct wlr_render_texture_options *options) {
    if (options->wait_timeline == &fence) {
        struct wlr_render_texture_options copy = *options;
        copy.wait_timeline = NULL;
        __real_wlr_render_pass_add_texture(pass, &copy);
        return;
    }
    __real_wlr_render_pass_add_texture(pass, options);
}

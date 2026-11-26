#include "scene_bridge.hpp"
#include "wlr_scene_compat.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <wayland-server-core.h>
#include <xf86drm.h>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pixman.h>
#include <wlr/render/swapchain.h>
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Scanout release at line %d: %s\n", __LINE__, #condition);             \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

// Model the kernel handle only. wlroots allocates, retains and destroys the real
// scene-output timeline and assigns release points during direct scanout.
enum { TEST_HANDLE = 91 };
static uint64_t signalled, last_rejection;
static unsigned destroyed, signals, rejections, presentations;
static bool discard, unplug_on_rejection, waiting_for_unplug;
static struct wl_event_loop *loop;
static struct wlr_drm_syncobj_timeline *timeline;

extern "C" int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle) {
    CHECK(fd == -1 && !flags);
    *handle = TEST_HANDLE;
    return 0;
}
extern "C" int drmSyncobjDestroy(int fd, uint32_t handle) {
    CHECK(fd == -1 && handle == TEST_HANDLE);
    destroyed++;
    return 0;
}
extern "C" int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                                        uint32_t count) {
    CHECK(fd == -1 && count == 1 && *handles == TEST_HANDLE && *points >= signalled);
    signalled = *points;
    signals++;
    return 0;
}

static bool (*headless_test)(struct wlr_output *, const struct wlr_output_state *);
static bool allow_timeline(struct wlr_output *output, const struct wlr_output_state *state) {
    struct wlr_output_state supported = *state;
    supported.committed &= ~WLR_OUTPUT_STATE_SIGNAL_TIMELINE;
    supported.signal_timeline = nullptr;
    return headless_test(output, &supported);
}

extern "C" bool
__real_wlr_scene_output_build_state(struct wlr_scene_output *viewport,
                                    struct wlr_output_state *state,
                                    const struct wlr_scene_output_state_options *options);
extern "C" bool
__wrap_wlr_scene_output_build_state(struct wlr_scene_output *viewport,
                                    struct wlr_output_state *state,
                                    const struct wlr_scene_output_state_options *options) {
    if (waiting_for_unplug)
        return false;
    if (!timeline) {
        timeline = wlr_drm_syncobj_timeline_create(-1);
        CHECK(timeline);
        viewport->WLR_PRIVATE.out_timeline = timeline;
    }
    // A headless output normally rejects signal timelines at the test-only
    // commit. Permit the real scene builder to consider this scanout candidate.
    struct wlr_output *output = viewport->output;
    const struct wlr_output_impl *original = output->impl;
    struct wlr_output_impl test_impl = *original;
    headless_test = original->test;
    test_impl.test = allow_timeline;
    output->impl = &test_impl;
    output->backend->features.timeline = true;
    bool built = __real_wlr_scene_output_build_state(viewport, state, options);
    output->backend->features.timeline = false;
    output->impl = original;
    return built;
}

extern "C" bool __real_wlr_output_commit_state(struct wlr_output *output,
                                               const struct wlr_output_state *state);
extern "C" bool __wrap_wlr_output_commit_state(struct wlr_output *output,
                                               const struct wlr_output_state *state) {
    if (state->signal_timeline) {
        CHECK(state->signal_timeline == timeline && state->signal_point > last_rejection);
        CHECK(state->buffer &&
              (!output->swapchain || !wlr_swapchain_has_buffer(output->swapchain, state->buffer)));
        last_rejection = state->signal_point;
        rejections++;
        waiting_for_unplug = unplug_on_rejection;
        return false;
    }
    return __real_wlr_output_commit_state(output, state);
}

static void present(struct wl_listener *listener, void *data) noexcept {
    (void)listener;
    auto *event = static_cast<wlr_output_event_present *>(data);
    if (discard)
        event->presented = false;
    presentations++;
}

static void wait_for(const unsigned *counter, unsigned count) {
    for (unsigned i = 0; *counter < count && i < 100; i++) {
        CHECK(wl_event_loop_dispatch(loop, 5) >= 0);
    }
    CHECK(*counter >= count);
}

static void paint(struct seaside_scene_bridge *scene) {
    auto *pixels = static_cast<uint8_t *>(calloc(160 * 90, 4));
    CHECK(pixels);
    struct wlr_buffer *buffer = seaside_scene_create_image(160, 90, pixels, free, pixels);
    CHECK(buffer);
    seaside_scene_set_wallpaper(scene, buffer);
}

int main(void) {
    struct wl_display *display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
    CHECK(allocator && wlr_backend_start(backend));
    struct seaside_scene_bridge *scene = seaside_scene_create(display);
    struct wlr_output *output = wlr_headless_add_output(backend, 160, 90);
    CHECK(scene && output && wlr_output_init_render(output, allocator, renderer));
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    CHECK(wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);
    wl_listener listener{.link = {}, .notify = present};
    wl_signal_add(&output->events.present, &listener);
    CHECK(seaside_scene_add_output(scene, output) &&
          seaside_scene_place_output(scene, output, 0, 0));
    wait_for(&presentations, 1);
    unsigned initial = presentations;
    for (unsigned i = 1; i <= 3; i++) {
        discard = i < 3;
        paint(scene);
        wait_for(&rejections, i);
        wait_for(&presentations, initial + i);
        CHECK(discard ? signals == 0 : signals == 1 && signalled == last_rejection);
    }
    unplug_on_rejection = true;
    paint(scene);
    wait_for(&rejections, 4);
    CHECK(signalled < last_rejection && !destroyed);
    wl_list_remove(&listener.link);
    wlr_output_destroy(output);
    CHECK(signalled == UINT64_MAX && signals == 2 && destroyed == 1);
    seaside_scene_destroy(scene);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return EXIT_SUCCESS;
}

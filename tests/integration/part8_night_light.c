#include "scene_bridge.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/color.h>
#include <wlr/render/pixman.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Night light fixture at line %d: %s\n", __LINE__, #condition);         \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

enum failure { ACCEPT, REJECT_ONCE, REJECT_ALWAYS };

struct monitor {
    struct wlr_output *output;
    enum failure failure;
    unsigned attempts, presented;
    bool initial_scanout, composed_retry, blamed_before_retry, tearing;
    float applied_blue;
};

static struct monitor monitors[2];
static struct seaside_scene_bridge *scene;
static struct wl_event_loop *loop;

size_t __wrap_wlr_output_get_gamma_size(struct wlr_output *output) {
    (void)output;
    return 256;
}

bool __real_wlr_output_commit_state(struct wlr_output *output,
                                    const struct wlr_output_state *state);

bool __wrap_wlr_output_commit_state(struct wlr_output *output,
                                    const struct wlr_output_state *state) {
    if (!(state->committed & WLR_OUTPUT_STATE_COLOR_TRANSFORM)) {
        return __real_wlr_output_commit_state(output, state);
    }
    struct monitor *monitor = output == monitors[0].output ? &monitors[0] : &monitors[1];
    const unsigned attempt = monitor->attempts++;
    const bool composed = state->buffer && output->swapchain &&
                          wlr_swapchain_has_buffer(output->swapchain, state->buffer);
    if (attempt == 0) {
        monitor->initial_scanout = state->buffer && !composed;
    } else if (attempt == 1) {
        monitor->composed_retry = composed;
        monitor->blamed_before_retry =
            seaside_scene_get_render_status(scene, output).night_light_reason != NULL;
    }
    monitor->tearing |= state->tearing_page_flip;
    if (monitor->failure != ACCEPT) {
        if (monitor->failure == REJECT_ONCE) {
            monitor->failure = ACCEPT;
        }
        return false;
    }
    // Model a hardware LUT. Headless still commits the real pixel buffer.
    struct wlr_output_state accepted = *state;
    accepted.committed &= ~WLR_OUTPUT_STATE_COLOR_TRANSFORM;
    accepted.color_transform = NULL;
    if (!__real_wlr_output_commit_state(output, &accepted)) {
        return false;
    }
    float white[] = {1, 1, 1};
    float result[] = {1, 1, 1};
    if (state->color_transform) {
        wlr_color_transform_eval(state->color_transform, result, white);
    }
    monitor->applied_blue = result[2];
    return true;
}

static void observe(void *data, struct wlr_output *output,
                    const struct seaside_scene_frame_observation *frame) {
    (void)data;
    if (strcmp(frame->event, "present") == 0 && frame->succeeded) {
        struct monitor *monitor = output == monitors[0].output ? &monitors[0] : &monitors[1];
        monitor->presented++;
    }
}

static bool wait_for_presentations(unsigned left, unsigned right) {
    for (unsigned i = 0; i < 100 && (monitors[0].presented < left || monitors[1].presented < right);
         ++i) {
        CHECK(wl_event_loop_dispatch(loop, 10) >= 0);
    }
    CHECK(monitors[0].presented >= left && monitors[1].presented >= right);
    return true;
}

static bool present_wallpaper(void) {
    // A single opaque buffer covers each output, permitting real scene scanout.
    const int width = 160, height = 90;
    void *pixels = calloc((size_t)width * height, 4);
    CHECK(pixels);
    struct wlr_buffer *buffer = seaside_scene_create_image(width, height, pixels, free, pixels);
    if (!buffer) {
        free(pixels);
        return false;
    }
    seaside_scene_set_wallpaper(scene, buffer);
    return wait_for_presentations(monitors[0].presented + 1, monitors[1].presented + 1);
}

static bool test_change(bool before, int from, bool after, int to, enum failure failure,
                        unsigned target) {
    struct monitor *fault = &monitors[target], *healthy = &monitors[1 - target];
    // Reset the runtime policy and apply a known starting transform on both outputs.
    monitors[0].failure = monitors[1].failure = ACCEPT;
    seaside_scene_set_render_policy(scene, true, true, !before, from);
    CHECK(present_wallpaper());
    seaside_scene_set_render_policy(scene, true, true, before, from);
    CHECK(present_wallpaper());
    const float previous = fault->applied_blue;
    CHECK((previous < 1) == before && previous == healthy->applied_blue);
    fault->failure = failure;
    fault->tearing = false;
    fault->attempts = healthy->attempts = 0;

    seaside_scene_set_render_policy(scene, true, true, after, to);
    CHECK(present_wallpaper());
    // Ongoing damage must still present after a permanent rejection.
    for (unsigned frame = 0; frame < 3; frame++) {
        CHECK(present_wallpaper());
    }

    const struct seaside_scene_render_status status =
        seaside_scene_get_render_status(scene, fault->output);
    const struct seaside_scene_render_status other =
        seaside_scene_get_render_status(scene, healthy->output);
    fprintf(stderr, "%dK/%d -> %dK/%d, %s rejection, output %u: LUT attempts=%u applied=%d\n", from,
            before, to, after, failure == REJECT_ONCE ? "transient" : "permanent", target,
            fault->attempts, status.night_light_applied);
    CHECK(healthy->attempts == 1 && other.night_light_applied == after &&
          !other.night_light_reason);
    CHECK((healthy->applied_blue < 1) == after && healthy->applied_blue != previous);
    CHECK(fault->attempts == 2);
    CHECK(fault->initial_scanout == (target == 0) && fault->composed_retry);
    CHECK(!fault->tearing && !fault->blamed_before_retry);
    if (failure == REJECT_ONCE) {
        CHECK(status.night_light_applied == after && !status.night_light_reason);
        CHECK(fault->applied_blue == healthy->applied_blue);
    } else {
        CHECK(status.night_light_applied == before && status.night_light_reason);
        CHECK(strcmp(status.night_light_reason, "backend rejected hardware gamma LUT") == 0);
        CHECK(fault->applied_blue == previous);
    }
    return true;
}

int main(void) {
    struct wl_display *display = wl_display_create();
    loop = wl_display_get_event_loop(display);
    struct wlr_backend *backend = wlr_headless_backend_create(loop);
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
    if (!allocator || !wlr_backend_start(backend) || !(scene = seaside_scene_create(display))) {
        return EXIT_FAILURE;
    }
    seaside_scene_set_frame_observer(scene, observe, NULL);
    for (int i = 0; i < 2; i++) {
        struct wlr_output *output = wlr_headless_add_output(backend, 160, 90);
        monitors[i] = (struct monitor){.output = output, .applied_blue = 1};
        if (!output || !wlr_output_init_render(output, allocator, renderer)) {
            return EXIT_FAILURE;
        }
        struct wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        bool enabled = wlr_output_commit_state(output, &state);
        wlr_output_state_finish(&state);
        if (!enabled || !seaside_scene_add_output(scene, output) ||
            !seaside_scene_place_output(scene, output, i * 160, 0)) {
            return EXIT_FAILURE;
        }
    }
    // The other monitor always uses the compositor swapchain.
    wlr_output_lock_attach_render(monitors[1].output, true);
    bool passed = true;
    for (unsigned target = 0; target < 2; ++target) {
        for (enum failure failure = REJECT_ONCE; failure <= REJECT_ALWAYS; ++failure) {
            passed &= test_change(false, 4000, true, 4000, failure, target);
            passed &= test_change(true, 4000, true, 5000, failure, target);
            passed &= test_change(true, 4000, false, 4000, failure, target);
        }
    }
    wlr_output_lock_attach_render(monitors[1].output, false);
    seaside_scene_destroy(scene);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

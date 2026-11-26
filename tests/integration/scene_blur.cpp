#include "scene_blur.hpp"
#include "wlr_scene_compat.hpp"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>
#include <wayland-server-core.h>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_output.h>
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "Scene blur at line %d: %s\n", __LINE__, #condition);                  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

enum { WIDTH = 160, HEIGHT = 90, GLASS_WIDTH = 80, GLASS_HEIGHT = 50 };

struct glass {
    struct wlr_buffer base;
    uint32_t pixels[GLASS_WIDTH * GLASS_HEIGHT];
};
static_assert(std::is_standard_layout_v<glass>);

static void destroy_glass(struct wlr_buffer *buffer) noexcept {
    struct glass *glass = wl_container_of(buffer, glass, base);
    wlr_buffer_finish(buffer);
    free(glass);
}

static bool glass_data(struct wlr_buffer *buffer, uint32_t flags, void **data, uint32_t *format,
                       size_t *stride) noexcept {
    if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ) {
        return false;
    }
    struct glass *glass = wl_container_of(buffer, glass, base);
    *data = glass->pixels;
    *format = DRM_FORMAT_ARGB8888;
    *stride = GLASS_WIDTH * sizeof(*glass->pixels);
    return true;
}

static void glass_data_end(struct wlr_buffer *buffer) noexcept {
    (void)buffer;
}

static const struct wlr_buffer_impl glass_impl = {
    .destroy = destroy_glass,
    .get_dmabuf = nullptr,
    .get_shm = nullptr,
    .begin_data_ptr_access = glass_data,
    .end_data_ptr_access = glass_data_end,
};

static struct glass *create_glass(void) {
    struct glass *glass = static_cast<struct glass *>(calloc(1, sizeof(struct glass)));
    CHECK(glass);
    wlr_buffer_init(&glass->base, &glass_impl, GLASS_WIDTH, GLASS_HEIGHT);
    for (size_t i = 0; i < GLASS_WIDTH * GLASS_HEIGHT; ++i) {
        glass->pixels[i] = i < 10 * GLASS_WIDTH ? UINT32_C(0xff00ff00) : UINT32_C(0x80000000);
    }
    return glass;
}

static void read_output(struct wlr_scene_output *viewport, uint32_t pixels[WIDTH * HEIGHT]) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    CHECK(wlr_scene_output_build_state(viewport, &state, nullptr));
    CHECK(state.buffer && state.buffer->width == WIDTH && state.buffer->height == HEIGHT);
    struct wlr_texture *texture = wlr_texture_from_buffer(viewport->output->renderer, state.buffer);
    wlr_texture_read_pixels_options options{};
    options.data = pixels;
    options.format = DRM_FORMAT_XRGB8888;
    options.stride = WIDTH * sizeof(*pixels);
    CHECK(texture && wlr_texture_read_pixels(texture, &options));
    wlr_texture_destroy(texture);
    CHECK(wlr_output_commit_state(viewport->output, &state));
    wlr_output_state_finish(&state);
}

static uint32_t rgb(const uint32_t pixels[WIDTH * HEIGHT], int x, int y) {
    return pixels[y * WIDTH + x] & UINT32_C(0xffffff);
}

int main(void) {
    struct wl_display *display = wl_display_create();
    CHECK(display);
    struct wlr_backend *backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
    struct wlr_renderer *renderer = wlr_pixman_renderer_create();
    CHECK(backend && renderer);
    struct wlr_allocator *allocator = wlr_allocator_autocreate(backend, renderer);
    struct wlr_output *output = wlr_headless_add_output(backend, WIDTH, HEIGHT);
    CHECK(allocator && output && wlr_backend_start(backend));
    CHECK(wlr_output_init_render(output, allocator, renderer));
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    CHECK(wlr_output_commit_state(output, &state));
    wlr_output_state_finish(&state);

    struct wlr_scene *scene = wlr_scene_create();
    CHECK(scene);
    struct wlr_scene_output *viewport = wlr_scene_output_create(scene, output);
    CHECK(viewport);
    const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1};
    struct wlr_scene_rect *left = wlr_scene_rect_create(&scene->tree, WIDTH / 2, HEIGHT, red);
    struct wlr_scene_rect *right = wlr_scene_rect_create(&scene->tree, WIDTH / 2, HEIGHT, blue);
    CHECK(left && right);
    wlr_scene_node_set_position(&right->node, WIDTH / 2, 0);

    struct wlr_scene_tree *window = wlr_scene_tree_create(&scene->tree);
    CHECK(window);
    struct wlr_scene_tree *content = wlr_scene_tree_create(window);
    CHECK(content);
    wlr_scene_node_set_position(&window->node, 40, 20);
    struct glass *glass = create_glass();
    struct wlr_scene_buffer *surface = wlr_scene_buffer_create(content, &glass->base);
    CHECK(surface);
    pixman_region32_t opaque;
    pixman_region32_init_rect(&opaque, 0, 0, GLASS_WIDTH, 10);
    wlr_scene_buffer_set_opaque_region(surface, &opaque);
    pixman_region32_fini(&opaque);

    struct seaside_scene_blur *blur = nullptr;
    const struct wlr_box geometry = {0, 0, WIDTH, HEIGHT};
    uint32_t pixels[WIDTH * HEIGHT];
    read_output(viewport, pixels);
    const uint32_t sharp = rgb(pixels, 76, 50);
    CHECK((sharp & 0xff) == 0);
    CHECK(seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 1, 2));
    read_output(viewport, pixels);
    CHECK((rgb(pixels, 76, 50) & 0xff) > 20);
    CHECK(rgb(pixels, 76, 24) == UINT32_C(0x00ff00));
    CHECK(rgb(pixels, 76, 80) == UINT32_C(0xff0000));
    CHECK(!wlr_scene_output_needs_frame(viewport));
    CHECK(seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 1, 2));
    CHECK(!wlr_scene_output_needs_frame(viewport));

    struct wlr_scene_rect *cover =
        wlr_scene_rect_create(&scene->tree, GLASS_WIDTH, GLASS_HEIGHT, green);
    CHECK(cover);
    wlr_scene_node_set_position(&cover->node, 40, 20);
    CHECK(
        !seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 1, 2));
    read_output(viewport, pixels);
    CHECK(rgb(pixels, 76, 50) == UINT32_C(0x00ff00));
    wlr_scene_node_destroy(&cover->node);

    wlr_scene_rect_set_color(left, green);
    CHECK(seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 1, 2));
    read_output(viewport, pixels);
    const uint32_t refreshed = rgb(pixels, 76, 50);
    CHECK((refreshed & UINT32_C(0xff0000)) == 0 && (refreshed & 0xff) > 20);
    wlr_scene_node_set_position(&window->node, 80, 20);
    CHECK(seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 1, 2));
    read_output(viewport, pixels);
    CHECK(rgb(pixels, 76, 50) == UINT32_C(0x00ff00));
    wlr_scene_node_set_position(&window->node, 40, 20);
    CHECK(
        !seaside_scene_blur_update(&blur, &scene->tree, window, content, output, &geometry, 0, 2));
    read_output(viewport, pixels);
    CHECK((rgb(pixels, 76, 50) & 0xff) == 0);

    seaside_scene_blur_destroy(&blur);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_buffer_drop(&glass->base);
    wlr_backend_destroy(backend);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wl_display_destroy(display);
    return 0;
}

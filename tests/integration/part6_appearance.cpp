#include "part3_bridge.hpp"
#include "scene.hpp"
#include "server.hpp"
#include "wlroots_bridge.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <wayland-server-core.h>

namespace {
using namespace std::chrono_literals;
using Output = std::unique_ptr<part3_output, decltype(&part3_output_destroy)>;

bool wait_for_pixel(wl_event_loop *loop, part3_output *output, unsigned x, unsigned y,
                    std::uint32_t expected) {
    if (!part3_output_sample_pixel(output, x, y)) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        if (wl_event_loop_dispatch(loop, 25) < 0) {
            return false;
        }
        const auto sample = part3_output_get_pixel(output);
        if (sample.frames && sample.rgb == expected) {
            return true;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

TEST(SceneAppearance, CropsWallpaperAndRestoresBannerAfterOutputReplacement) {
    auto server = seaside::Server::create();
    ASSERT_TRUE(server);
    auto *display = (*server)->display();
    auto *loop = wl_display_get_event_loop(display);
    auto scene = seaside::Scene::create(display);
    ASSERT_TRUE(scene);
    auto *backend = part3_headless_backend_create(display);
    ASSERT_NE(backend, nullptr);
    auto *renderer = part3_pixman_renderer_create();
    if (!renderer) {
        seaside_wlr_backend_destroy(backend);
        FAIL() << "Cannot create Pixman fixture renderer";
    }
    ASSERT_TRUE((*server)->start(backend, renderer));
    const auto make_output = [&]() {
        return Output(part3_output_create(display, backend, (*server)->allocator(), renderer),
                      part3_output_destroy);
    };
    auto output = make_output();
    ASSERT_TRUE(output);
    ASSERT_TRUE((*scene)->add_output(part3_output_handle(output.get())));
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 0, 0));

    auto wallpaper = std::make_shared<seaside::PixelImage>();
    wallpaper->width = 9;
    wallpaper->height = 3;
    for (int y = 0; y < wallpaper->height; ++y) {
        for (int x = 0; x < wallpaper->width; ++x) {
            const bool edge = x == 0 || x == wallpaper->width - 1;
            wallpaper->pixels.insert(wallpaper->pixels.end(),
                                     {0, static_cast<std::uint8_t>(edge ? 0 : 255),
                                      static_cast<std::uint8_t>(edge ? 255 : 0), 255});
        }
    }
    auto prepared = seaside::Scene::prepare_wallpaper(std::move(wallpaper));
    ASSERT_TRUE(prepared);
    (*scene)->set_wallpaper(std::move(*prepared));
    // Cover crops off both outer red columns. A stretched image would keep them.
    constexpr std::uint32_t green = 0x00ff00;
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 89, green));
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 159, 89, green));

    const seaside::Diagnostic error{.path = "/tmp/café.toml",
                                    .line = 3,
                                    .column = 1,
                                    .field = "window.layout",
                                    .message = "Invalid value"};
    (*scene)->set_config_error(error);
    auto banner = seaside::render_config_banner("color sample", 160, 1);
    ASSERT_TRUE(banner);
    const auto &pixel = banner->pixels;
    const auto color = static_cast<std::uint32_t>(pixel[0] | pixel[1] << 8 | pixel[2] << 16);
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 0, color));

    output.reset();
    output = make_output();
    ASSERT_TRUE(output);
    ASSERT_TRUE((*scene)->add_output(part3_output_handle(output.get())));
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 0, color));
    (*scene)->set_config_error(std::nullopt);
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 0, green));
    (*scene)->set_wallpaper({});
    EXPECT_TRUE(wait_for_pixel(loop, output.get(), 0, 0, 0));
    EXPECT_FALSE((*scene)->snapshot().failed);
}
} // namespace

#include "render_assets.hpp"
#include "test_directory.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <png.h>
#include <span>
#include <turbojpeg.h>
#include <webp/encode.h>

namespace seaside {
namespace {

void write_bytes(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
}

TEST(Wallpaper, PngAlphaFlattensOverBlackAndRejectsTruncation) {
    test::Directory directory;
    const auto path = directory.path() / "translucent.png";
    const std::array<std::uint8_t, 8> pixels{6, 16, 42, 255, 20, 40, 60, 128};
    png_image file{.opaque = nullptr,
                   .version = PNG_IMAGE_VERSION,
                   .width = 2,
                   .height = 1,
                   .format = PNG_FORMAT_BGRA,
                   .flags = 0,
                   .colormap_entries = 0,
                   .warning_or_error = 0,
                   .message = {}};
    ASSERT_NE(png_image_write_to_file(&file, path.c_str(), false, pixels.data(), 0, nullptr), 0);
    auto image = decode_wallpaper(path);
    ASSERT_TRUE(image);
    ASSERT_TRUE(*image);
    EXPECT_EQ((*image)->width, 2);
    EXPECT_EQ((*image)->height, 1);
    EXPECT_EQ((*image)->pixels, (std::vector<std::uint8_t>{6, 16, 42, 255, 10, 20, 30, 255}));

    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    image = decode_wallpaper(path);
    ASSERT_FALSE(image);
    EXPECT_EQ(image.error().field, "wallpaper.path");
    EXPECT_EQ(image.error().path, path);
}

TEST(Wallpaper, JpegUsesOpaqueBgraAndRejectsTruncatedEntropyData) {
    test::Directory directory;
    const auto path = directory.path() / "blue.jpg";
    const std::array<std::uint8_t, 4> pixel{224, 16, 8, 255};
    std::unique_ptr<void, decltype(&tj3Destroy)> encoder(tj3Init(TJINIT_COMPRESS), tj3Destroy);
    ASSERT_TRUE(encoder);
    ASSERT_EQ(tj3Set(encoder.get(), TJPARAM_SUBSAMP, TJSAMP_444), 0);
    ASSERT_EQ(tj3Set(encoder.get(), TJPARAM_QUALITY, 95), 0);
    unsigned char *encoded = nullptr;
    std::size_t encoded_size = 0;
    ASSERT_EQ(
        tj3Compress8(encoder.get(), pixel.data(), 1, 0, 1, TJPF_BGRA, &encoded, &encoded_size), 0);
    std::unique_ptr<unsigned char, decltype(&tj3Free)> bytes(encoded, tj3Free);
    write_bytes(path, std::span(bytes.get(), encoded_size));
    const auto image = decode_wallpaper(path);
    ASSERT_TRUE(image);
    ASSERT_TRUE(*image);
    ASSERT_EQ((*image)->pixels.size(), 4);
    EXPECT_GE((*image)->pixels[0], 215);
    EXPECT_LE((*image)->pixels[1], 24);
    EXPECT_LE((*image)->pixels[2], 16);
    EXPECT_EQ((*image)->pixels[3], 255);

    ASSERT_GT(encoded_size, 2);
    write_bytes(path, std::span(bytes.get(), encoded_size - 2));
    EXPECT_FALSE(decode_wallpaper(path));
}

TEST(Wallpaper, WebpIsRecognizedByContentAndFileSizeIsBounded) {
    test::Directory directory;
    const auto path = directory.path() / "content.bin";
    const std::array<std::uint8_t, 4> pixel{16, 32, 64, 128};
    std::uint8_t *encoded = nullptr;
    const auto encoded_size = WebPEncodeLosslessBGRA(pixel.data(), 1, 1, 4, &encoded);
    ASSERT_GT(encoded_size, 0);
    std::unique_ptr<std::uint8_t, decltype(&WebPFree)> bytes(encoded, WebPFree);
    write_bytes(path, std::span(bytes.get(), encoded_size));
    const auto image = decode_wallpaper(path);
    ASSERT_TRUE(image);
    ASSERT_TRUE(*image);
    EXPECT_EQ((*image)->pixels, (std::vector<std::uint8_t>{8, 16, 32, 255}));
    std::filesystem::resize_file(path, 32 * 1024 * 1024 + 1);
    EXPECT_FALSE(decode_wallpaper(path));
    EXPECT_FALSE(decode_wallpaper(directory.path()));
}

TEST(Wallpaper, PackagedBlackUsesTheDefaultSceneClear) {
    const auto image = decode_wallpaper("/usr/share/seaside/wallpapers/black.png");
    ASSERT_TRUE(image);
    EXPECT_FALSE(*image);
}

TEST(ConfigBanner, RendersUnicodeAsPlainTextAtFractionalScale) {
    const auto image = render_config_banner("bad Ω /café.toml:4\nlayout <unknown>", 320, 1.25);
    ASSERT_TRUE(image);
    EXPECT_EQ(image->width, 400);
    EXPECT_GT(image->logical_height, 16);
    EXPECT_LE(image->logical_height, 104);
    EXPECT_GE(image->height, image->logical_height * 1.25);
    bool found_glyph = false;
    for (std::size_t i = 0; i < image->pixels.size(); i += 4) {
        const auto *pixel = image->pixels.data() + i;
        found_glyph |= pixel[0] > 200 && pixel[1] > 200 && pixel[2] > 200;
    }
    EXPECT_TRUE(found_glyph);
    const auto narrow = render_config_banner(std::string(8192, 'w'), 42, 4);
    ASSERT_TRUE(narrow);
    EXPECT_LE(narrow->logical_height, 104);
    EXPECT_FALSE(render_config_banner("invalid extent", 0, 1));
}

} // namespace
} // namespace seaside

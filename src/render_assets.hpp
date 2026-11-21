#ifndef SEASIDE_RENDER_ASSETS_HPP
#define SEASIDE_RENDER_ASSETS_HPP

#include "config.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace seaside {

// Packed BGRA pixels, with alpha premultiplied for use as DRM_FORMAT_ARGB8888.
struct PixelImage {
    int width = 0;
    int height = 0;
    int logical_height = 0;
    std::vector<std::uint8_t> pixels;
};

using Wallpaper = std::shared_ptr<const PixelImage>;

// The bundled black wallpaper uses the scene's black clear color directly.
std::expected<Wallpaper, Diagnostic> decode_wallpaper(const std::string &path);

inline constexpr std::size_t max_config_banner_bytes = 16 * 1024 * 1024;
std::shared_ptr<const PixelImage> render_config_banner(std::string_view message, int width,
                                                       float scale);

} // namespace seaside

#endif

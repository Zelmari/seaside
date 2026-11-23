#include "render_assets.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <pango/pangocairo.h>
#include <png.h>
#include <span>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <webp/decode.h>

namespace seaside {
namespace {

constexpr std::string_view default_wallpaper = "/usr/share/seaside/wallpapers/black.png";
constexpr std::size_t max_file_bytes = 32 * 1024 * 1024;
constexpr std::size_t max_pixels = 64 * 1024 * 1024 / 4;
constexpr unsigned max_extent = 8192;

Diagnostic wallpaper_error(const std::string &path, std::string_view reason) {
    Diagnostic error;
    error.path = path;
    error.field = "wallpaper.path";
    error.message = reason;
    return error;
}

struct SourceFile {
    int fd;
    ~SourceFile() { ::close(fd); }
};

std::expected<std::vector<std::uint8_t>, Diagnostic> read_wallpaper(const std::string &path) {
    SourceFile file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)};
    if (file.fd < 0) {
        return std::unexpected(wallpaper_error(path, "Cannot open wallpaper image"));
    }
    struct stat info{};
    if (::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uintmax_t>(info.st_size) > max_file_bytes) {
        return std::unexpected(
            wallpaper_error(path, "Expected a nonempty image file up to 32 MiB"));
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t offset = 0;
    while (offset != bytes.size()) {
        const auto count = ::read(file.fd, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count == 0 || errno != EINTR) {
            return std::unexpected(
                wallpaper_error(path, "Cannot read the complete wallpaper image"));
        }
    }
    std::uint8_t extra = 0;
    ssize_t count;
    do {
        count = ::read(file.fd, &extra, 1);
    } while (count < 0 && errno == EINTR);
    if (count != 0) {
        return std::unexpected(wallpaper_error(path, "Wallpaper image changed while being read"));
    }
    return bytes;
}

bool allocate(PixelImage &image, unsigned width, unsigned height) {
    if (!width || !height || width > max_extent || height > max_extent ||
        static_cast<std::size_t>(width) * height > max_pixels) {
        return false;
    }
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.pixels.resize(static_cast<std::size_t>(width) * height * 4);
    return true;
}

struct PngRead {
    png_image image{};
    ~PngRead() { png_image_free(&image); }
};

std::uint32_t read_network_u32(const std::uint8_t *bytes) {
    return static_cast<std::uint32_t>(bytes[0]) << 24 | static_cast<std::uint32_t>(bytes[1]) << 16 |
           static_cast<std::uint32_t>(bytes[2]) << 8 | bytes[3];
}

bool static_png(std::span<const std::uint8_t> bytes) {
    // libpng's simplified API can read an APNG's fallback image. A static
    // wallpaper must contain a complete IEND and must not declare animation.
    for (std::size_t start = 8; bytes.size() - start >= 12;) {
        const auto length = read_network_u32(bytes.data() + start);
        if (length > bytes.size() - start - 12) {
            return false;
        }
        const auto *type = bytes.data() + start + 4;
        if (std::memcmp(type, "acTL", 4) == 0) {
            return false;
        }
        if (std::memcmp(type, "IEND", 4) == 0) {
            return length == 0;
        }
        start += static_cast<std::size_t>(length) + 12;
    }
    return false;
}

bool decode_png(std::span<const std::uint8_t> bytes, PixelImage &result) {
    PngRead file;
    file.image.version = PNG_IMAGE_VERSION;
    if (!static_png(bytes) ||
        !png_image_begin_read_from_memory(&file.image, bytes.data(), bytes.size()) ||
        !allocate(result, file.image.width, file.image.height)) {
        return false;
    }
    file.image.format = PNG_FORMAT_BGRA;
    return png_image_finish_read(&file.image, nullptr, result.pixels.data(), 0, nullptr) != 0;
}

bool decode_jpeg(std::span<const std::uint8_t> bytes, PixelImage &result) {
    std::unique_ptr<void, decltype(&tj3Destroy)> decoder(tj3Init(TJINIT_DECOMPRESS), tj3Destroy);
    if (!decoder || tj3Set(decoder.get(), TJPARAM_STOPONWARNING, 1) != 0 ||
        tj3DecompressHeader(decoder.get(), bytes.data(), bytes.size()) != 0 ||
        !allocate(result, static_cast<unsigned>(tj3Get(decoder.get(), TJPARAM_JPEGWIDTH)),
                  static_cast<unsigned>(tj3Get(decoder.get(), TJPARAM_JPEGHEIGHT)))) {
        return false;
    }
    return tj3Decompress8(decoder.get(), bytes.data(), bytes.size(), result.pixels.data(),
                          result.width * 4, TJPF_BGRA) == 0;
}

bool decode_webp(std::span<const std::uint8_t> bytes, PixelImage &result) {
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK ||
        features.has_animation || !allocate(result, features.width, features.height)) {
        return false;
    }
    return WebPDecodeBGRAInto(bytes.data(), bytes.size(), result.pixels.data(),
                              result.pixels.size(), result.width * 4) != nullptr;
}

void flatten_to_black(PixelImage &image) {
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        const auto alpha = image.pixels[i + 3];
        if (alpha == 255) {
            continue;
        }
        for (std::size_t channel = 0; channel < 3; ++channel) {
            image.pixels[i + channel] = static_cast<std::uint8_t>(
                (static_cast<unsigned>(image.pixels[i + channel]) * alpha + 127) / 255);
        }
        image.pixels[i + 3] = 255;
    }
}

} // namespace

std::expected<Wallpaper, Diagnostic> decode_wallpaper(const std::string &path) {
    if (path == default_wallpaper) {
        return Wallpaper{};
    }
    auto bytes = read_wallpaper(path);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    auto image = std::make_shared<PixelImage>();
    bool decoded = false;
    if (bytes->size() >= 8 && png_sig_cmp(bytes->data(), 0, 8) == 0) {
        decoded = decode_png(*bytes, *image);
    } else if (bytes->size() >= 2 && (*bytes)[0] == 0xFF && (*bytes)[1] == 0xD8) {
        decoded = decode_jpeg(*bytes, *image);
    } else if (bytes->size() >= 12 && std::memcmp(bytes->data(), "RIFF", 4) == 0 &&
               std::memcmp(bytes->data() + 8, "WEBP", 4) == 0) {
        decoded = decode_webp(*bytes, *image);
    }
    if (!decoded) {
        return std::unexpected(wallpaper_error(
            path, "Expected a complete static PNG, JPEG or WebP image within size limits"));
    }
    flatten_to_black(*image);
    return image;
}

std::shared_ptr<const PixelImage> render_config_banner(std::string_view message, int width,
                                                       float scale, bool dark) {
    if (message.empty() || width <= 0 || !std::isfinite(scale) || scale <= 0 || scale > 4) {
        return {};
    }
    constexpr int padding = 8;
    constexpr int max_height = 104;
    message = message.substr(0, 10 * 1024);
    const int layout_width = std::min(width, static_cast<int>(max_extent));
    const int pixels_wide = static_cast<int>(
        std::min<double>(max_extent, std::ceil(static_cast<double>(layout_width) * scale)));
    const double horizontal_scale = static_cast<double>(pixels_wide) / layout_width;
    std::unique_ptr<PangoFontMap, decltype(&g_object_unref)> fonts(pango_cairo_font_map_new(),
                                                                   g_object_unref);
    if (!fonts) {
        return {};
    }
    std::unique_ptr<PangoContext, decltype(&g_object_unref)> context(
        pango_font_map_create_context(fonts.get()), g_object_unref);
    std::unique_ptr<PangoLayout, decltype(&g_object_unref)> layout(pango_layout_new(context.get()),
                                                                   g_object_unref);
    std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)> font(
        pango_font_description_from_string("Sans 11"), pango_font_description_free);
    std::unique_ptr<gchar, decltype(&g_free)> valid_text(
        g_utf8_make_valid(message.data(), static_cast<gssize>(message.size())), g_free);
    pango_layout_set_font_description(layout.get(), font.get());
    pango_layout_set_width(layout.get(), std::max(1, layout_width - 2 * padding) * PANGO_SCALE);
    pango_layout_set_wrap(layout.get(), PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout.get(), PANGO_ELLIPSIZE_END);
    pango_layout_set_height(layout.get(), (max_height - 2 * padding) * PANGO_SCALE);
    pango_layout_set_text(layout.get(), valid_text.get(), -1);
    int height = 0;
    pango_layout_get_pixel_size(layout.get(), nullptr, &height);
    const int logical_height = std::clamp(height + 2 * padding, 2 * padding, max_height);
    const int pixels_tall =
        static_cast<int>(std::ceil(static_cast<double>(logical_height) * scale));
    auto result = std::make_shared<PixelImage>();
    if (!allocate(*result, pixels_wide, pixels_tall)) {
        return {};
    }
    result->logical_height = logical_height;
    std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
        cairo_image_surface_create_for_data(result->pixels.data(), CAIRO_FORMAT_ARGB32, pixels_wide,
                                            pixels_tall, pixels_wide * 4),
        cairo_surface_destroy);
    std::unique_ptr<cairo_t, decltype(&cairo_destroy)> cairo(cairo_create(surface.get()),
                                                             cairo_destroy);
    if (dark) {
        cairo_set_source_rgb(cairo.get(), 0.33, 0.06, 0.10);
    } else {
        cairo_set_source_rgb(cairo.get(), 0.98, 0.90, 0.91);
    }
    cairo_paint(cairo.get());
    cairo_scale(cairo.get(), horizontal_scale, static_cast<double>(pixels_tall) / logical_height);
    cairo_move_to(cairo.get(), padding, padding);
    if (dark) {
        cairo_set_source_rgb(cairo.get(), 1, 1, 1);
    } else {
        cairo_set_source_rgb(cairo.get(), 0.28, 0.06, 0.09);
    }
    pango_cairo_update_layout(cairo.get(), layout.get());
    pango_cairo_show_layout(cairo.get(), layout.get());
    cairo_surface_flush(surface.get());
    return cairo_status(cairo.get()) == CAIRO_STATUS_SUCCESS ? result : nullptr;
}

} // namespace seaside

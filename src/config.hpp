#ifndef SEASIDE_CONFIG_HPP
#define SEASIDE_CONFIG_HPP

#include "bindings.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seaside {

inline constexpr std::size_t max_config_bytes = 1024 * 1024;
inline constexpr std::string_view legacy_bindings_comment =
    "# This table is the complete active map: delete/comment an entry to disable it.";
inline constexpr std::string_view bindings_comment =
    "# This table is the complete binding map. Delete or comment an entry to disable it.";
inline constexpr std::string_view legacy_move_binding_example =
    "# \"Super+Shift+Left\" = \"move_window left\"";
enum class Layout { dwindle, scrolling };
enum class Insertion { focused, largest };
enum class AdaptiveSync { off, fullscreen, always };
enum class FullscreenAspect { preserve, stretch };
enum class ColorScheme { dark, light };
enum class ScrollDirection { normal, inverse };
enum class PowerProfile { saver, balanced, performance };

struct Resolution {
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool operator==(const Resolution &) const = default;
};
struct Position {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const Position &) const = default;
};
struct MonitorSettings {
    std::optional<Resolution> resolution;
    std::optional<double> refresh_rate;
    double scale = 1.0;
    std::int32_t rotation = 0;
    std::optional<Position> position;
    bool operator==(const MonitorSettings &) const = default;
};
struct MonitorOverride {
    OutputSelector match;
    // The outer optional distinguishes omitted inheritance from explicit "auto".
    std::optional<std::optional<Resolution>> resolution;
    std::optional<std::optional<double>> refresh_rate;
    std::optional<double> scale;
    std::optional<std::int32_t> rotation;
    std::optional<std::optional<Position>> position;
    bool operator==(const MonitorOverride &) const = default;
};

struct Config {
    struct Autostart {
        std::vector<std::string> commands;
        bool operator==(const Autostart &) const = default;
    } autostart;
    struct Wallpaper {
        std::string path = "/usr/share/seaside/wallpapers/black.png";
        bool operator==(const Wallpaper &) const = default;
    } wallpaper;
    struct Window {
        bool tiling = true;
        bool titlebars = true;
        bool allow_edge_drag = false;
        Layout layout = Layout::dwindle;
        std::int32_t gaps_outer = 10;
        std::int32_t gaps_inner = 5;
        std::int32_t border_width = 2;
        std::uint32_t active_border_color = 0xbfbfbf;
        std::uint32_t inactive_border_color = 0x595959;
        bool operator==(const Window &) const = default;
    } window;
    struct Appearance {
        ColorScheme color_scheme = ColorScheme::dark;
        bool operator==(const Appearance &) const = default;
    } appearance;
    struct Applications {
        std::string default_browser = "firefox";
        bool operator==(const Applications &) const = default;
    } applications;
    struct Audio {
        std::optional<std::string> default_output;
        std::optional<std::string> default_input;
        bool operator==(const Audio &) const = default;
    } audio;
    struct Power {
        std::optional<PowerProfile> profile;
        bool operator==(const Power &) const = default;
    } power;
    struct Monitors {
        std::optional<OutputSelector> primary;
        MonitorSettings defaults;
        std::vector<MonitorOverride> output;
        bool operator==(const Monitors &) const = default;
    } monitors;
    struct Dwindle {
        Insertion insertion = Insertion::focused;
        double split_ratio = 0.5;
        bool operator==(const Dwindle &) const = default;
    } dwindle;
    struct Scrolling {
        double default_column_width = 0.5;
        bool operator==(const Scrolling &) const = default;
    } scrolling;
    struct Input {
        struct Mouse {
            std::string profile = "flat";
            double sensitivity = 0.0;
            ScrollDirection scroll_direction = ScrollDirection::normal;
            bool operator==(const Mouse &) const = default;
        } mouse;
        bool operator==(const Input &) const = default;
    } input;
    struct Idle {
        std::int32_t display_off_after_minutes = 15;
        bool operator==(const Idle &) const = default;
    } idle;
    struct Render {
        bool direct_scanout = true;
        AdaptiveSync adaptive_sync = AdaptiveSync::off;
        bool allow_tearing = true;
        FullscreenAspect fullscreen_aspect = FullscreenAspect::preserve;
        struct Blur {
            bool enabled = false;
            double strength = 0.5;
            std::int32_t passes = 2;
            bool operator==(const Blur &) const = default;
        } blur;
        bool operator==(const Render &) const = default;
    } render;
    struct NightLight {
        bool enabled = false;
        std::int32_t temperature_kelvin = 4500;
        bool operator==(const NightLight &) const = default;
    } night_light;
    BindingMap keybinds;
    bool operator==(const Config &) const = default;
};

Config default_config();
std::string_view default_template() noexcept;
std::expected<Config, Diagnostic> parse_config(std::string_view text,
                                               std::string_view path = "config.toml");
MonitorSettings effective_monitor_settings(const MonitorSettings &defaults,
                                           const MonitorOverride &override);

// Capability preparation receives only caller-supplied current identities. It
// validates ambiguity/duplicate matches without pretending to qualify DRM modes.
struct LiveOutputIdentity {
    std::string identity;
    std::string connector;
};
std::expected<void, Diagnostic>
validate_monitor_matches(const Config &config, const std::vector<LiveOutputIdentity> &outputs);

} // namespace seaside

#endif

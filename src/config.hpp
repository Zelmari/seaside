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
enum class Layout { dwindle, scrolling };
enum class WorkspaceMode { global, per_monitor };
enum class Insertion { focused, largest };
enum class AdaptiveSync { off, fullscreen, always };
enum class FullscreenAspect { preserve, stretch };

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
        Layout layout = Layout::dwindle;
        std::int32_t gaps_outer = 10;
        std::int32_t gaps_inner = 5;
        std::int32_t border_width = 2;
        bool operator==(const Window &) const = default;
    } window;
    struct Workspaces {
        WorkspaceMode mode = WorkspaceMode::global;
        bool operator==(const Workspaces &) const = default;
    } workspaces;
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
        bool allow_tearing = false;
        FullscreenAspect fullscreen_aspect = FullscreenAspect::preserve;
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

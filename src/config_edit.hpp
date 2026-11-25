#ifndef SEASIDE_CONFIG_EDIT_HPP
#define SEASIDE_CONFIG_EDIT_HPP

#include "config.hpp"

#include <variant>

namespace seaside {

enum class Setting {
    keybinds,
    night_light_enabled,
    night_light_temperature,
    power_profile,
    idle_minutes,
    allow_edge_drag
};

struct SettingEdit {
    Setting setting;
    std::variant<std::monostate, bool, int, std::string, NightLightAction> value{};
    std::optional<Chord> chord{};
    std::optional<std::uint64_t> revision{};
};

struct SettingError {
    std::string code;
    std::string message;
};

std::optional<Setting> parse_setting(std::string_view name) noexcept;
std::expected<std::string, SettingError> rewrite_setting(std::string text, const Config &active,
                                                         const SettingEdit &edit);

} // namespace seaside
#endif

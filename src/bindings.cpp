#include "bindings.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace seaside {
namespace {
Diagnostic error(std::string_view field, std::string_view message) {
    return {"", 0, 0, std::string(field.substr(0, 256)), std::string(message.substr(0, 512))};
}

bool whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

std::vector<std::string_view> words(std::string_view input) {
    std::vector<std::string_view> result;
    result.reserve(4);
    while (!input.empty()) {
        while (!input.empty() && whitespace(input.front())) {
            input.remove_prefix(1);
        }
        if (input.empty()) {
            break;
        }
        auto end = input.find_first_of(" \t\n\r");
        result.push_back(input.substr(0, end));
        if (result.size() == 4)
            break;
        if (end == std::string_view::npos) {
            break;
        }
        input.remove_prefix(end);
    }
    return result;
}

Direction direction(std::string_view value) {
    if (value == "left")
        return Direction::left;
    if (value == "right")
        return Direction::right;
    if (value == "up")
        return Direction::up;
    if (value == "down")
        return Direction::down;
    return Direction::none;
}

std::optional<std::int64_t> positive(std::string_view value, std::int64_t maximum) {
    std::int64_t number = 0;
    auto [end, code] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (code != std::errc{} || end != value.data() + value.size() || number <= 0 ||
        number > maximum) {
        return {};
    }
    return number;
}
} // namespace

std::expected<OutputSelector, Diagnostic> parse_output_selector(std::string_view value) {
    auto separator = value.find(':');
    if (separator == std::string_view::npos || separator + 1 == value.size() ||
        value.find('\0') != std::string_view::npos ||
        value.find_first_of("\r\n") != std::string_view::npos) {
        return std::unexpected(error("selector", "Expected identity:<value> or connector:<name>"));
    }
    auto prefix = value.substr(0, separator);
    if (prefix != "identity" && prefix != "connector") {
        return std::unexpected(error("selector", "Unknown output selector prefix"));
    }
    return OutputSelector{prefix == "identity" ? SelectorKind::identity : SelectorKind::connector,
                          std::string(value.substr(separator + 1))};
}

std::expected<Chord, Diagnostic> parse_chord(std::string_view value) {
    Chord result;
    if (value.empty() || value.find('\0') != std::string_view::npos) {
        return std::unexpected(error("keybinds", "Expected a nonempty chord"));
    }
    while (true) {
        auto split = value.find('+');
        if (split == std::string_view::npos)
            break;
        auto modifier = value.substr(0, split);
        std::uint8_t bit = modifier == "Super"   ? 1
                           : modifier == "Ctrl"  ? 2
                           : modifier == "Alt"   ? 4
                           : modifier == "Shift" ? 8
                                                 : 0;
        if (!bit || (result.modifiers & bit)) {
            return std::unexpected(error("keybinds", "Unknown or duplicate modifier"));
        }
        result.modifiers |= bit;
        value.remove_prefix(split + 1);
    }
    if (value == "MouseLeftDrag" || value == "MouseRightDrag") {
        if (!result.modifiers) {
            return std::unexpected(error("keybinds", "Mouse drags require a modifier"));
        }
        result.kind =
            value == "MouseLeftDrag" ? ChordKind::mouse_left_drag : ChordKind::mouse_right_drag;
    } else {
        // XKB resolves aliases. Do not lowercase names or infer Shift from case.
        result.keysym = xkb_keysym_from_name(std::string(value).c_str(), XKB_KEYSYM_NO_FLAGS);
        if (!result.keysym) {
            return std::unexpected(error("keybinds", "Unknown or missing XKB keysym"));
        }
    }
    return result;
}

std::expected<Action, Diagnostic> parse_action(std::string_view value, const Chord &chord) {
    if (value.find('\0') != std::string_view::npos) {
        return std::unexpected(error("keybinds", "Actions cannot contain NUL bytes"));
    }
    auto tokens = words(value);
    if (tokens.empty()) {
        return std::unexpected(error("keybinds", "Expected an action"));
    }
    Action result;
    const auto command = tokens[0];
    const bool keyboard = chord.kind == ChordKind::keyboard;
    if (command == "exec") {
        auto position = value.find(command) + command.size();
        while (position < value.size() && whitespace(value[position]))
            ++position;
        if (position == value.size() || !keyboard) {
            return std::unexpected(
                error("keybinds", "exec requires a keyboard chord and shell command"));
        }
        result.kind = ActionKind::exec;
        result.text = value.substr(position);
        return result;
    }
    if (command == "move_window" || command == "resize_window") {
        result.kind =
            command == "move_window" ? ActionKind::move_window : ActionKind::resize_window;
        if (!keyboard) {
            const bool matched = command == "move_window"
                                     ? chord.kind == ChordKind::mouse_left_drag
                                     : chord.kind == ChordKind::mouse_right_drag;
            if (tokens.size() == 1 && matched)
                return result;
            return std::unexpected(
                error("keybinds", "Action is incompatible with this mouse gesture"));
        }
    } else if (!keyboard) {
        return std::unexpected(error("keybinds", "Only move/resize actions accept mouse gestures"));
    }
    const std::array simple{
        std::pair{"toggle_window_tiling", ActionKind::toggle_window_tiling},
        std::pair{"cycle_focus_mru", ActionKind::cycle_focus_mru},
        std::pair{"toggle_window_pin", ActionKind::toggle_window_pin},
        std::pair{"toggle_window_maximize", ActionKind::toggle_window_maximize},
        std::pair{"toggle_window_fullscreen", ActionKind::toggle_window_fullscreen},
        std::pair{"close_window", ActionKind::close_window},
        std::pair{"exit", ActionKind::exit},
    };
    for (const auto &[name, kind] : simple) {
        if (command == name && tokens.size() == 1) {
            result.kind = kind;
            return result;
        }
    }
    if (command == "focus" || command == "swap_window" || command == "move_window" ||
        command == "resize_window") {
        const bool resize = command == "resize_window";
        if ((tokens.size() != 2 && !(resize && tokens.size() == 3)) ||
            (result.direction = direction(tokens[1])) == Direction::none) {
            return std::unexpected(
                error("keybinds", "Expected a direction and permitted arguments"));
        }
        result.kind = command == "focus"         ? ActionKind::focus
                      : command == "swap_window" ? ActionKind::swap_window
                      : command == "move_window" ? ActionKind::move_window
                                                 : ActionKind::resize_window;
        if (resize) {
            result.argument = 32;
            if (tokens.size() == 3) {
                auto number = positive(tokens[2], std::numeric_limits<std::int32_t>::max());
                if (!number)
                    return std::unexpected(
                        error("keybinds", "Resize distance must be a positive integer"));
                result.argument = *number;
            }
        }
        return result;
    }
    if ((command == "workspace" || command == "move_to_workspace") && tokens.size() == 2) {
        auto number = positive(tokens[1], std::numeric_limits<std::int32_t>::max());
        if (!number)
            return std::unexpected(error("keybinds", "Workspace ID is outside 1..2147483647"));
        result.kind =
            command == "workspace" ? ActionKind::workspace : ActionKind::move_to_workspace;
        result.argument = *number;
        return result;
    }
    if (command == "transfer_workspace") {
        auto position = value.find(command) + command.size();
        while (position < value.size() && whitespace(value[position]))
            ++position;
        // An opaque selector may contain spaces. Its whole suffix is one
        // argument; do not impose a whitespace-free identity format here.
        auto suffix = value.substr(position);
        auto selector = parse_output_selector(suffix);
        if (!selector)
            return std::unexpected(selector.error());
        result.kind = ActionKind::transfer_workspace;
        result.text = suffix;
        return result;
    }
    if (command == "night_light" && tokens.size() == 2) {
        if (tokens[1] != "on" && tokens[1] != "off" && tokens[1] != "toggle") {
            return std::unexpected(error("keybinds", "Expected night_light on, off or toggle"));
        }
        result.kind = ActionKind::night_light;
        result.night_light = tokens[1] == "on"    ? NightLightAction::on
                             : tokens[1] == "off" ? NightLightAction::off
                                                  : NightLightAction::toggle;
        return result;
    }
    return std::unexpected(error("keybinds", "Unknown action or invalid argument count"));
}

BindingMap default_bindings() {
    const std::array definitions{
        std::pair{"Super+D", "toggle_window_tiling"},
        std::pair{"Super+Left", "focus left"},
        std::pair{"Super+Right", "focus right"},
        std::pair{"Super+Up", "focus up"},
        std::pair{"Super+Down", "focus down"},
        std::pair{"Alt+Tab", "cycle_focus_mru"},
        std::pair{"Super+MouseLeftDrag", "move_window"},
        std::pair{"Super+MouseRightDrag", "resize_window"},
        std::pair{"Super+Return", "exec foot"},
        std::pair{"XF86AudioLowerVolume", "exec wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"},
        std::pair{"XF86AudioRaiseVolume",
                  "exec wpctl set-volume --limit 1 @DEFAULT_AUDIO_SINK@ 5%+"},
        std::pair{"XF86AudioMute", "exec wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"},
    };
    BindingMap result;
    for (const auto &[key, action] : definitions) {
        auto chord = parse_chord(key);
        auto parsed = parse_action(action, *chord);
        result.emplace(*chord, std::move(*parsed));
    }
    return result;
}

} // namespace seaside

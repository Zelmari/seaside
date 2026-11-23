#include "bindings.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <vector>
#include <xkbcommon/xkbcommon.h>

namespace seaside {
namespace {
using Modifier = std::pair<std::string_view, std::uint8_t>;
constexpr std::array modifiers{Modifier{"Super", 1}, Modifier{"Ctrl", 2}, Modifier{"Alt", 4},
                               Modifier{"Shift", 8}};

using ActionName = std::pair<std::string_view, ActionKind>;
constexpr std::array actions{
    ActionName{"toggle_window_tiling", ActionKind::toggle_window_tiling},
    ActionName{"focus", ActionKind::focus},
    ActionName{"cycle_focus_mru", ActionKind::cycle_focus_mru},
    ActionName{"move_window", ActionKind::move_window},
    ActionName{"resize_window", ActionKind::resize_window},
    ActionName{"swap_window", ActionKind::swap_window},
    ActionName{"workspace", ActionKind::workspace},
    ActionName{"move_to_workspace", ActionKind::move_to_workspace},
    ActionName{"transfer_workspace", ActionKind::transfer_workspace},
    ActionName{"toggle_window_pin", ActionKind::toggle_window_pin},
    ActionName{"toggle_window_maximize", ActionKind::toggle_window_maximize},
    ActionName{"toggle_window_fullscreen", ActionKind::toggle_window_fullscreen},
    ActionName{"close_window", ActionKind::close_window},
    ActionName{"night_light", ActionKind::night_light},
    ActionName{"exit", ActionKind::exit},
    ActionName{"exec", ActionKind::exec},
};

using DirectionName = std::pair<std::string_view, Direction>;
constexpr std::array directions{
    DirectionName{"left", Direction::left}, DirectionName{"right", Direction::right},
    DirectionName{"up", Direction::up}, DirectionName{"down", Direction::down}};

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
    const auto found = std::ranges::find(directions, value, &DirectionName::first);
    return found == directions.end() ? Direction::none : found->second;
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
        const auto modifier =
            std::ranges::find(modifiers, value.substr(0, split), &Modifier::first);
        if (modifier == modifiers.end() || (result.modifiers & modifier->second)) {
            return std::unexpected(error("keybinds", "Unknown or duplicate modifier"));
        }
        result.modifiers |= modifier->second;
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
        if (value == "Space") {
            value = "space";
        }
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
    const auto command = tokens[0];
    const auto operation = std::ranges::find(actions, command, &ActionName::first);
    if (operation == actions.end()) {
        return std::unexpected(error("keybinds", "Unknown action or invalid argument count"));
    }
    Action result;
    result.kind = operation->second;
    const bool keyboard = chord.kind == ChordKind::keyboard;
    if (result.kind == ActionKind::exec) {
        auto position = value.find(command) + command.size();
        while (position < value.size() && whitespace(value[position]))
            ++position;
        if (position == value.size() || !keyboard) {
            return std::unexpected(
                error("keybinds", "exec requires a keyboard chord and shell command"));
        }
        result.text = value.substr(position);
        return result;
    }
    if (result.kind == ActionKind::move_window || result.kind == ActionKind::resize_window) {
        if (!keyboard) {
            const bool matched = result.kind == ActionKind::move_window
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
    switch (result.kind) {
    case ActionKind::focus:
    case ActionKind::swap_window:
    case ActionKind::move_window:
    case ActionKind::resize_window: {
        const bool resize = result.kind == ActionKind::resize_window;
        if ((tokens.size() != 2 && !(resize && tokens.size() == 3)) ||
            (result.direction = direction(tokens[1])) == Direction::none) {
            return std::unexpected(
                error("keybinds", "Expected a direction and permitted arguments"));
        }
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
    case ActionKind::workspace:
    case ActionKind::move_to_workspace: {
        if (tokens.size() != 2)
            break;
        auto number = positive(tokens[1], std::numeric_limits<std::int32_t>::max());
        if (!number)
            return std::unexpected(error("keybinds", "Workspace ID is outside 1..2147483647"));
        result.argument = *number;
        return result;
    }
    case ActionKind::transfer_workspace: {
        auto position = value.find(command) + command.size();
        while (position < value.size() && whitespace(value[position]))
            ++position;
        // An opaque selector may contain spaces. Its whole suffix is one
        // argument; do not impose a whitespace-free identity format here.
        auto suffix = value.substr(position);
        auto selector = parse_output_selector(suffix);
        if (!selector)
            return std::unexpected(selector.error());
        result.text = suffix;
        return result;
    }
    case ActionKind::night_light:
        if (tokens.size() != 2)
            break;
        if (tokens[1] != "on" && tokens[1] != "off" && tokens[1] != "toggle") {
            return std::unexpected(error("keybinds", "Expected night_light on, off or toggle"));
        }
        result.night_light = tokens[1] == "on"    ? NightLightAction::on
                             : tokens[1] == "off" ? NightLightAction::off
                                                  : NightLightAction::toggle;
        return result;
    default:
        if (tokens.size() == 1)
            return result;
        break;
    }
    return std::unexpected(error("keybinds", "Unknown action or invalid argument count"));
}

std::string format_chord(const Chord &chord) {
    std::string result;
    for (const auto &[name, bit] : modifiers) {
        if (chord.modifiers & bit) {
            result += name;
            result += '+';
        }
    }
    if (chord.kind != ChordKind::keyboard) {
        result += chord.kind == ChordKind::mouse_left_drag ? "MouseLeftDrag" : "MouseRightDrag";
    } else if (chord.keysym == XKB_KEY_space) {
        result += "Space";
    } else {
        std::array<char, 64> name{};
        xkb_keysym_get_name(chord.keysym, name.data(), name.size());
        result += name.data();
    }
    return result;
}

std::string format_action(const Action &action) {
    const auto operation = std::ranges::find(actions, action.kind, &ActionName::second);
    std::string result(operation->first);
    if (action.direction != Direction::none) {
        const auto named = std::ranges::find(directions, action.direction, &DirectionName::second);
        result += ' ';
        result += named->first;
    }
    if (action.argument > 0 &&
        (action.kind != ActionKind::resize_window || action.argument != 32)) {
        result += ' ';
        result += std::to_string(action.argument);
    }
    if (!action.text.empty()) {
        result += ' ';
        result += action.text;
    }
    if (action.kind == ActionKind::night_light) {
        result += action.night_light == NightLightAction::on    ? " on"
                  : action.night_light == NightLightAction::off ? " off"
                                                                : " toggle";
    }
    return result;
}

BindingMap default_bindings() {
    const std::array definitions{
        std::pair{"Super+Space", "exec wofi --show drun"},
        std::pair{"Super+Q", "close_window"},
        std::pair{"Super+P", "toggle_window_pin"},
        std::pair{"Super+Shift+E", "exit"},
        std::pair{"Super+D", "toggle_window_tiling"},
        std::pair{"Super+F", "toggle_window_maximize"},
        std::pair{"Super+Shift+F", "toggle_window_fullscreen"},
        std::pair{"Super+1", "workspace 1"},
        std::pair{"Super+2", "workspace 2"},
        std::pair{"Super+3", "workspace 3"},
        std::pair{"Super+4", "workspace 4"},
        std::pair{"Super+5", "workspace 5"},
        std::pair{"Super+6", "workspace 6"},
        std::pair{"Super+7", "workspace 7"},
        std::pair{"Super+8", "workspace 8"},
        std::pair{"Super+9", "workspace 9"},
        std::pair{"Super+0", "workspace 10"},
        std::pair{"Super+Shift+1", "move_to_workspace 1"},
        std::pair{"Super+Shift+2", "move_to_workspace 2"},
        std::pair{"Super+Shift+3", "move_to_workspace 3"},
        std::pair{"Super+Shift+4", "move_to_workspace 4"},
        std::pair{"Super+Shift+5", "move_to_workspace 5"},
        std::pair{"Super+Shift+6", "move_to_workspace 6"},
        std::pair{"Super+Shift+7", "move_to_workspace 7"},
        std::pair{"Super+Shift+8", "move_to_workspace 8"},
        std::pair{"Super+Shift+9", "move_to_workspace 9"},
        std::pair{"Super+Shift+0", "move_to_workspace 10"},
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

#ifndef SEASIDE_BINDINGS_HPP
#define SEASIDE_BINDINGS_HPP

#include <compare>
#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <string_view>

namespace seaside {

struct Diagnostic {
    std::string path;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string field;
    std::string message;
    bool operator==(const Diagnostic &) const = default;
};

enum class SelectorKind { identity, connector };
struct OutputSelector {
    SelectorKind kind = SelectorKind::connector;
    std::string value;
    bool operator==(const OutputSelector &) const = default;
};

enum class ChordKind { keyboard, mouse_left_drag, mouse_right_drag };
struct Chord {
    std::uint8_t modifiers = 0;
    ChordKind kind = ChordKind::keyboard;
    std::uint32_t keysym = 0;
    auto operator<=>(const Chord &) const = default;
};

enum class ActionKind {
    toggle_window_tiling,
    focus,
    cycle_focus_mru,
    move_window,
    resize_window,
    swap_window,
    workspace,
    move_to_workspace,
    transfer_workspace,
    toggle_window_pin,
    toggle_window_maximize,
    toggle_window_fullscreen,
    close_window,
    night_light,
    exit,
    exec,
};
enum class Direction { none, left, right, up, down };
enum class NightLightAction { on, off, toggle };
struct Action {
    ActionKind kind = ActionKind::exit;
    Direction direction = Direction::none;
    std::int64_t argument = 0;
    std::string text;
    NightLightAction night_light = NightLightAction::toggle;
    bool operator==(const Action &) const = default;
};
using BindingMap = std::map<Chord, Action>;

std::expected<OutputSelector, Diagnostic> parse_output_selector(std::string_view value);
std::expected<Chord, Diagnostic> parse_chord(std::string_view value);
std::expected<Action, Diagnostic> parse_action(std::string_view value, const Chord &chord);
BindingMap default_bindings();

} // namespace seaside

#endif

#include "ipc_protocol.hpp"

#include "bindings.hpp"
#include "state.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <ostream>
#include <set>
#include <streambuf>
#include <sys/socket.h>

namespace seaside::ipc {
namespace {

constexpr int nesting_limit = 16;
constexpr auto logical_limit = std::numeric_limits<std::int32_t>::max();

struct NestingLimit {};
struct RecordTooLarge {};

class RecordBuffer : public std::streambuf {
  public:
    std::string take() { return std::move(record_); }

  private:
    std::streamsize xsputn(const char *data, std::streamsize length) override {
        const auto bytes = static_cast<std::size_t>(length);
        if (bytes > record_limit - record_.size()) {
            throw RecordTooLarge{};
        }
        record_.append(data, bytes);
        return length;
    }

    int_type overflow(int_type character) override {
        if (!traits_type::eq_int_type(character, traits_type::eof())) {
            const char byte = traits_type::to_char_type(character);
            xsputn(&byte, 1);
        }
        return traits_type::not_eof(character);
    }

    std::string record_;
};

enum Parameter : unsigned {
    direction = 1 << 0,
    workspace = 1 << 1,
    window = 1 << 2,
    delta = 1 << 4,
    enabled = 1 << 5,
};

struct Method {
    std::string_view name;
    unsigned required = 0;
    unsigned optional = 0;
};

constexpr std::array methods{
    Method{"query.status"},
    Method{"query.bindings"},
    Method{"query.outputs"},
    Method{"query.workspaces"},
    Method{"query.windows"},
    Method{"query.focus"},
    Method{"query.config"},
    Method{"query.diagnostics"},
    Method{"query.night_light"},
    Method{"query.settings"},
    Method{"action.focus", direction},
    Method{"action.cycle_focus_mru"},
    Method{"action.workspace_select", workspace},
    Method{"action.window_to_workspace", workspace, window},
    Method{"action.window_toggle_tiling", 0, window},
    Method{"action.window_toggle_pin", 0, window},
    Method{"action.window_toggle_maximize", 0, window},
    Method{"action.window_toggle_fullscreen", 0, window},
    Method{"action.window_close", 0, window},
    Method{"action.window_move", direction, window},
    Method{"action.window_swap", direction, window},
    Method{"action.window_resize", direction, window | delta},
    Method{"action.night_light_set", enabled},
    Method{"action.settings_update"},
    Method{"action.exit"},
    Method{"trace.start"},
    Method{"trace.stop"},
    Method{"trace.export"},
    Method{"events.subscribe"},
};

struct Field {
    std::string_view name;
    Parameter bit;
    std::string_view expectation;
};

constexpr std::array fields{
    Field{"direction", direction, "Expected direction left, right, up or down"},
    Field{"workspace_id", workspace, "Expected workspace_id in 1..10"},
    Field{"window_id", window, "Expected window_id in 1..9007199254740991"},
    Field{"delta", delta, "Expected delta in 1..2147483647 logical pixels"},
    Field{"enabled", enabled, "Expected enabled to be a boolean"},
};

bool integer_in_range(const IpcJson &value, std::uint64_t min, std::uint64_t max) {
    if (!value.is_number_unsigned()) {
        return false;
    }
    const auto number = value.get<std::uint64_t>();
    return number >= min && number <= max;
}

bool valid_field(Parameter field, const IpcJson &value) {
    switch (field) {
    case direction:
        return value == "left" || value == "right" || value == "up" || value == "down";
    case workspace:
        return integer_in_range(value, 1, workspace_count);
    case delta:
        return integer_in_range(value, 1, logical_limit);
    case window:
        return integer_in_range(value, 1, id_limit);
    case enabled:
        return value.is_boolean();
    }
    return false;
}

} // namespace

ParsedRecord parse_record(std::string_view record) {
    ParsedRecord parsed;
    std::array<std::set<std::string>, nesting_limit> keys;
    bool duplicate = false;
    bool duplicate_id = false;
    bool root_id_value = false;
    using Event = IpcJson::parse_event_t;

    const auto inspect = [&](int depth, Event event, IpcJson &value) {
        // Aborting here bounds the parser itself, including rejected input.
        if (event == Event::object_start || event == Event::array_start) {
            if (depth >= nesting_limit) {
                throw NestingLimit{};
            }
            keys[depth].clear();
        }
        if (event == Event::key) {
            const auto &key = value.get_ref<const std::string &>();
            const bool repeated = !keys[depth - 1].insert(key).second;
            duplicate |= repeated;
            if (depth == 1) {
                root_id_value = key == "id";
                duplicate_id |= root_id_value && repeated;
            }
        } else if (depth == 1 && root_id_value) {
            // Keep a recoverable request ID for a later malformed field or an
            // incomplete object, provided the key was unambiguous.
            if (event == Event::value && integer_in_range(value, 0, id_limit)) {
                parsed.id = value.get<std::uint64_t>();
            }
            root_id_value = false;
        }
        return true;
    };

    const auto nul = record.find('\0');
    const bool bom = record.starts_with("\xef\xbb\xbf");
    const auto prefix = record.substr(0, nul);
    try {
        // The JSON lexer treats a raw NUL as end-of-input and accepts a BOM.
        // JSONL records may contain neither extension.
        parsed.object = IpcJson::parse(prefix.begin(), prefix.end(), inspect, false);
        if (parsed.object.is_discarded() || nul != std::string_view::npos || bom) {
            parsed.error = "Malformed UTF-8 JSON record";
        } else if (duplicate) {
            parsed.error = "Duplicate JSON object field";
        } else if (!parsed.object.is_object()) {
            parsed.error = "Expected one JSON object per line";
        }
    } catch (const NestingLimit &) {
        parsed.error = "JSON nesting exceeds 16 containers";
    }
    if (duplicate_id) {
        parsed.id.reset();
    }
    return parsed;
}

std::optional<IpcError> validate_request(const ParsedRecord &record) {
    if (!record.error.empty()) {
        return IpcError{"invalid_request", std::string(record.error)};
    }
    const auto &object = record.object;
    if (object.size() != 4 || !object.contains("v") || !record.id || !object.contains("method") ||
        !object.contains("params") || !object["v"].is_number_integer() ||
        !object["method"].is_string() || !object["params"].is_object()) {
        return IpcError{"invalid_request", "Expected exactly v, id, method and object params"};
    }
    if (object["v"] != 1) {
        return IpcError{"unsupported_version", "Supported protocol version is 1"};
    }

    const auto &name = object["method"].get_ref<const std::string &>();
    const auto method = std::ranges::find(methods, name, &Method::name);
    if (method == methods.end()) {
        return IpcError{"unknown_method", "Method is outside the Seaside IPC allowlist"};
    }
    if (name == "action.settings_update") {
        if (const auto decoded = decode_setting_edit(object["params"]); !decoded)
            return decoded.error();
        return {};
    }

    unsigned received = 0;
    for (const auto &[key, value] : object["params"].items()) {
        const auto field = std::ranges::find(fields, key, &Field::name);
        if (field == fields.end() || !(field->bit & (method->required | method->optional))) {
            return IpcError{"invalid_args", "Unexpected parameter for this method"};
        }
        if (!valid_field(field->bit, value)) {
            return IpcError{"invalid_args", std::string(field->expectation)};
        }
        received |= field->bit;
    }
    if (received != (received | method->required)) {
        return IpcError{"invalid_args", "Required parameter is missing"};
    }
    return {};
}

std::expected<SettingEdit, IpcError> decode_setting_edit(const IpcJson &params) {
    const auto invalid = [](std::string message) {
        return std::unexpected(IpcError{"invalid_args", std::move(message)});
    };
    for (const auto &[field, _] : params.items()) {
        if (field != "setting" && field != "value" && field != "chord" && field != "revision")
            return invalid("Unexpected parameter for a settings update");
    }
    if (!params.contains("setting") || !params["setting"].is_string() || !params.contains("value"))
        return invalid("Expected a setting name and value");
    const auto setting = parse_setting(params["setting"].get_ref<const std::string &>());
    if (!setting)
        return invalid("Unknown setting");
    SettingEdit edit{*setting};
    if (params.contains("revision")) {
        if (!integer_in_range(params["revision"], 0, id_limit))
            return invalid("Expected a nonnegative settings revision");
        edit.revision = params["revision"].get<std::uint64_t>();
    }
    if (params.contains("chord")) {
        if (*setting != Setting::keybinds || !params["chord"].is_string())
            return invalid("Only keybinds accept a chord string");
        const auto chord = parse_chord(params["chord"].get_ref<const std::string &>());
        if (!chord)
            return invalid(chord.error().message);
        edit.chord = *chord;
    }
    const auto &value = params["value"];
    switch (*setting) {
    case Setting::keybinds:
        if (value.is_string() && edit.chord) {
            auto action = value.get<std::string>();
            if (const auto valid = parse_action(action, *edit.chord); !valid)
                return invalid(valid.error().message);
            edit.value = std::move(action);
        } else if (!value.is_null()) {
            return invalid("Expected a chord and action string, or null to remove or reset");
        }
        break;
    case Setting::night_light_enabled:
    case Setting::allow_edge_drag:
        if (value.is_boolean())
            edit.value = value.get<bool>();
        else if (*setting == Setting::night_light_enabled && value == "toggle")
            edit.value = NightLightAction::toggle;
        else
            return invalid("Expected a boolean or, for night light, toggle");
        break;
    case Setting::night_light_temperature:
        if (!integer_in_range(value, 1000, 10000))
            return invalid("Expected temperature_kelvin in 1000..10000");
        edit.value = value.get<int>();
        break;
    case Setting::idle_minutes:
        if (!integer_in_range(value, 0, 1440))
            return invalid("Expected display_off_after_minutes in 0..1440");
        edit.value = value.get<int>();
        break;
    case Setting::power_profile:
        if (value == "saver" || value == "balanced" || value == "performance")
            edit.value = value.get<std::string>();
        else if (!value.is_null())
            return invalid("Expected saver, balanced, performance, or null for system policy");
        break;
    }
    return edit;
}

std::optional<std::string> serialize(const IpcJson &object) {
    RecordBuffer buffer;
    std::ostream output(&buffer);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        output << object << '\n';
    } catch (const RecordTooLarge &) {
        return {};
    }
    return buffer.take();
}

std::expected<std::string, std::string> runtime_path(std::string_view display) {
    const char *runtime_env = std::getenv("XDG_RUNTIME_DIR");
    const std::string_view runtime = runtime_env ? runtime_env : "";
    if (!runtime.starts_with('/') || runtime == "/") {
        return std::unexpected("XDG_RUNTIME_DIR must name a private absolute directory");
    }
    if (display.starts_with(runtime) && display.substr(runtime.size()).starts_with('/')) {
        display.remove_prefix(runtime.size() + 1);
    }
    if (display.empty() || display == "." || display == ".." ||
        display.find('/') != std::string_view::npos ||
        display.find('\0') != std::string_view::npos) {
        return std::unexpected("WAYLAND_DISPLAY must name a Wayland socket in XDG_RUNTIME_DIR");
    }
    return std::string(runtime) + "/seaside/" + std::string(display) + ".sock";
}

std::expected<sockaddr_un, std::string> address(std::string_view path) {
    sockaddr_un result{};
    if (!path.starts_with('/') || path.size() >= sizeof(result.sun_path) ||
        path.find('\0') != std::string_view::npos) {
        return std::unexpected(
            "Seaside socket path must be absolute and fit a Unix socket address");
    }
    result.sun_family = AF_UNIX;
    std::ranges::copy(path, result.sun_path);
    return result;
}

} // namespace seaside::ipc

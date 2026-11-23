#include "config.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <toml++/toml.hpp>

#if TOML_ENABLE_UNRELEASED_FEATURES
#error "Seaside configuration requires strict released TOML 1.0 syntax"
#endif

namespace seaside {
namespace {

std::string bounded(std::string_view text, std::size_t size) {
    auto end = std::min(size, text.size());
    // Diagnostics will later cross UTF-8 JSON/text boundaries. Do not truncate
    // a valid source path/key in the middle of a multibyte code point.
    while (end && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
        --end;
    return std::string(text.substr(0, end));
}

// toml++ bounds nested values, but not arbitrarily long dotted table paths.
// This lexical guard limits such paths before constructing a recursive DOM.
// Strings/comments are skipped; the actual TOML parser remains the authority.
bool safe_table_depth(std::string_view text) {
    enum class Quote { none, basic, literal, multi_basic, multi_literal };
    Quote quote = Quote::none;
    bool escaped = false;
    bool comment = false;
    unsigned dots = 0;
    unsigned pending_path = 0;
    unsigned depth = 0;
    std::array<unsigned, 64> nesting{};
    std::size_t level = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (comment) {
            if (c == '\n') {
                comment = false;
                dots = 0;
            }
            continue;
        }
        if (quote != Quote::none) {
            bool basic = quote == Quote::basic || quote == Quote::multi_basic;
            if (basic && escaped) {
                escaped = false;
                continue;
            }
            if (basic && c == '\\') {
                escaped = true;
                continue;
            }
            char ending = basic ? '"' : '\'';
            bool multi = quote == Quote::multi_basic || quote == Quote::multi_literal;
            if (c == ending && (!multi || text.substr(i, 3) == std::string(3, ending))) {
                if (multi) {
                    // TOML permits one/two content quotes before the closing
                    // triple. Consume the whole run so later keys stay visible.
                    while (i + 1 < text.size() && text[i + 1] == ending)
                        ++i;
                }
                quote = Quote::none;
            }
            continue;
        }
        if (c == '#') {
            comment = true;
            continue;
        }
        if (c == '"' || c == '\'') {
            bool multi = text.substr(i, 3) == std::string(3, c);
            quote = c == '"' ? (multi ? Quote::multi_basic : Quote::basic)
                             : (multi ? Quote::multi_literal : Quote::literal);
            if (multi)
                i += 2;
        } else if (c == '.') {
            if (++dots + depth > 64)
                return false;
        } else if (c == '=') {
            pending_path = dots;
            dots = 0;
        } else if (c == '[' || c == '{') {
            // Dotted fields followed by nested values multiply DOM depth even
            // when each individual path is short. Bound their combined cost.
            const auto cost = pending_path + 1;
            if (level == nesting.size() || depth + cost > 64)
                return false;
            nesting[level++] = cost;
            depth += cost;
            pending_path = dots = 0;
        } else if (c == ']' || c == '}') {
            if (level)
                depth -= nesting[--level];
            pending_path = dots = 0;
        } else if (c == ',' || c == '\n') {
            pending_path = 0;
            dots = 0;
        }
    }
    return true;
}

class Validator {
  public:
    explicit Validator(std::string_view path) : path_(bounded(path, 4096)) {}
    std::optional<Diagnostic> failure;

    void fail(const toml::node *node, std::string_view field, std::string_view message) {
        if (failure)
            return;
        Diagnostic diagnostic{path_, 0, 0, bounded(field, 256), bounded(message, 512)};
        if (node) {
            diagnostic.line = node->source().begin.line;
            diagnostic.column = node->source().begin.column;
        }
        failure = std::move(diagnostic);
    }

    const toml::table *table(const toml::table &parent, std::string_view key,
                             std::string_view field) {
        auto *node = parent.get(key);
        if (!node)
            return nullptr;
        if (!node->is_table()) {
            fail(node, field, "Expected a table");
            return nullptr;
        }
        return node->as_table();
    }

    void keys(const toml::table &table, std::initializer_list<std::string_view> allowed,
              std::string_view prefix) {
        for (const auto &[key, node] : table) {
            if (std::find(allowed.begin(), allowed.end(), key.str()) == allowed.end()) {
                auto field = std::string(prefix);
                if (!field.empty())
                    field += '.';
                field += key.str();
                fail(&node, field, "Unknown configuration key");
            }
        }
    }

    std::optional<std::string> string(const toml::node *node, std::string_view field) {
        if (!node)
            return {};
        if (!node->is_string()) {
            fail(node, field, "Expected a string");
            return {};
        }
        auto result = node->as_string()->get();
        if (result.find('\0') != std::string::npos) {
            fail(node, field, "NUL bytes are not permitted");
            return {};
        }
        return result;
    }

    void boolean(const toml::table &table, std::string_view key, std::string_view field,
                 bool &target) {
        auto *node = table.get(key);
        if (!node)
            return;
        if (!node->is_boolean()) {
            fail(node, field, "Expected a boolean");
            return;
        }
        target = node->as_boolean()->get();
    }

    void integer(const toml::table &table, std::string_view key, std::string_view field,
                 std::int32_t &target, std::int32_t minimum, std::int32_t maximum) {
        auto *node = table.get(key);
        if (!node)
            return;
        if (!node->is_integer()) {
            fail(node, field, "Expected an integer");
            return;
        }
        auto value = node->as_integer()->get();
        if (value < minimum || value > maximum) {
            fail(node, field, "Integer outside permitted range");
            return;
        }
        target = static_cast<std::int32_t>(value);
    }

    std::optional<double> number(const toml::node *node, std::string_view field, double minimum,
                                 double maximum) {
        if (!node)
            return {};
        if (!node->is_integer() && !node->is_floating_point()) {
            fail(node, field, "Expected a finite number");
            return {};
        }
        auto converted = node->value<double>();
        if (!converted) {
            fail(node, field, "Number cannot be represented safely");
            return {};
        }
        auto value = *converted;
        if (!std::isfinite(value) || value < minimum || value > maximum) {
            fail(node, field, "Number outside permitted finite range");
            return {};
        }
        return value;
    }

    template <typename Enum>
    void enumeration(const toml::table &table, std::string_view key, std::string_view field,
                     Enum &target,
                     std::initializer_list<std::pair<std::string_view, Enum>> values) {
        auto *node = table.get(key);
        auto value = string(node, field);
        if (!value)
            return;
        for (auto [name, item] : values) {
            if (*value == name) {
                target = item;
                return;
            }
        }
        fail(node, field, "Unknown enum value");
    }

    std::optional<Resolution> resolution(const toml::node *node, std::string_view field) {
        auto value = string(node, field);
        if (!value || *value == "auto")
            return {};
        auto split = value->find('x');
        Resolution result;
        bool valid = split != std::string::npos && split != 0 && split + 1 < value->size();
        if (valid) {
            auto [wend, wc] = std::from_chars(value->data(), value->data() + split, result.width);
            auto [hend, hc] = std::from_chars(value->data() + split + 1,
                                              value->data() + value->size(), result.height);
            valid = wc == std::errc{} && hc == std::errc{} && wend == value->data() + split &&
                    hend == value->data() + value->size() && result.width > 0 && result.height > 0;
        }
        if (!valid) {
            fail(node, field, "Expected auto or positive WIDTHxHEIGHT");
            return {};
        }
        return result;
    }

    std::optional<double> refresh(const toml::node *node, std::string_view field) {
        if (node && node->is_string()) {
            auto value = string(node, field);
            if (value && *value == "auto")
                return {};
            fail(node, field, "Expected auto or a positive finite refresh rate");
            return {};
        }
        return number(node, field, std::numeric_limits<double>::denorm_min(),
                      std::numeric_limits<double>::max());
    }

    std::optional<Position> position(const toml::node *node, std::string_view field) {
        if (!node)
            return {};
        if (node->is_string()) {
            auto value = string(node, field);
            if (value && *value == "auto")
                return {};
            fail(node, field, "Expected auto or two signed integer coordinates");
            return {};
        }
        auto *array = node->as_array();
        if (!array || array->size() != 2) {
            fail(node, field, "Expected two signed integer coordinates");
            return {};
        }
        Position result;
        std::array<std::int32_t *, 2> targets{&result.x, &result.y};
        for (std::size_t i = 0; i < 2; ++i) {
            const auto &coordinate = (*array)[i];
            if (!coordinate.is_integer()) {
                fail(&coordinate, field, "Expected integer coordinate");
                return {};
            }
            auto value = coordinate.as_integer()->get();
            if (value < std::numeric_limits<std::int32_t>::min() ||
                value > std::numeric_limits<std::int32_t>::max()) {
                fail(&coordinate, field, "Coordinate outside representable range");
                return {};
            }
            *targets[i] = static_cast<std::int32_t>(value);
        }
        return result;
    }

    std::int32_t rotation(const toml::table &table, std::string_view prefix) {
        std::int32_t value = 0;
        integer(table, "rotation", std::string(prefix) + ".rotation", value, 0, 270);
        if (value != 0 && value != 90 && value != 180 && value != 270) {
            fail(table.get("rotation"), std::string(prefix) + ".rotation",
                 "Expected 0, 90, 180 or 270");
        }
        return value;
    }

  private:
    std::string path_;
};

void parse_monitors(Validator &v, const toml::table &table, Config::Monitors &target) {
    v.keys(table,
           {"primary", "resolution", "refresh_rate", "scale", "rotation", "position", "output"},
           "monitors");
    if (auto primary = v.string(table.get("primary"), "monitors.primary")) {
        if (*primary != "auto") {
            auto parsed = parse_output_selector(*primary);
            if (parsed)
                target.primary = std::move(*parsed);
            else
                v.fail(table.get("primary"), "monitors.primary", parsed.error().message);
        }
    }
    target.defaults.resolution = v.resolution(table.get("resolution"), "monitors.resolution");
    target.defaults.refresh_rate = v.refresh(table.get("refresh_rate"), "monitors.refresh_rate");
    target.defaults.position = v.position(table.get("position"), "monitors.position");
    if (auto value = v.number(table.get("scale"), "monitors.scale", 0.5, 4.0))
        target.defaults.scale = *value;
    target.defaults.rotation = v.rotation(table, "monitors");
    auto *node = table.get("output");
    if (!node)
        return;
    auto *outputs = node->as_array();
    if (!outputs) {
        v.fail(node, "monitors.output", "Expected an array of output tables");
        return;
    }
    for (const auto &item : *outputs) {
        auto *entry = item.as_table();
        if (!entry) {
            v.fail(&item, "monitors.output", "Expected output table");
            return;
        }
        v.keys(*entry, {"match", "resolution", "refresh_rate", "scale", "rotation", "position"},
               "monitors.output");
        MonitorOverride output;
        auto match = v.string(entry->get("match"), "monitors.output.match");
        if (!match) {
            v.fail(&item, "monitors.output.match", "Output override requires match");
            return;
        }
        auto parsed = parse_output_selector(*match);
        if (!parsed) {
            v.fail(entry->get("match"), "monitors.output.match", parsed.error().message);
            return;
        }
        output.match = std::move(*parsed);
        if (entry->contains("resolution"))
            output.resolution.emplace(
                v.resolution(entry->get("resolution"), "monitors.output.resolution"));
        if (entry->contains("refresh_rate"))
            output.refresh_rate.emplace(
                v.refresh(entry->get("refresh_rate"), "monitors.output.refresh_rate"));
        if (entry->contains("position"))
            output.position.emplace(v.position(entry->get("position"), "monitors.output.position"));
        output.scale = v.number(entry->get("scale"), "monitors.output.scale", 0.5, 4.0);
        if (entry->contains("rotation"))
            output.rotation = v.rotation(*entry, "monitors.output");
        if (v.failure)
            return;
        target.output.push_back(std::move(output));
    }
}

} // namespace

Config default_config() {
    Config config;
    config.keybinds = default_bindings();
    return config;
}

std::string_view default_template() noexcept {
    static constexpr std::string_view text =
#include "config_template.inc"
        ;
    return text;
}

std::expected<Config, Diagnostic> parse_config(std::string_view text, std::string_view path) {
    if (text.size() > max_config_bytes || !safe_table_depth(text)) {
        return std::unexpected(Diagnostic{bounded(path, 4096), 0, 0, "",
                                          "Configuration exceeds size or nesting limit"});
    }
    try {
        auto parsed = toml::parse(text, bounded(path, 4096));
        Validator v(path);
        Config config = default_config();
        v.keys(parsed,
               {"autostart", "wallpaper", "window", "workspaces", "monitors", "dwindle",
                "scrolling", "input", "idle", "render", "night_light", "keybinds"},
               "");
        if (auto *table = v.table(parsed, "autostart", "autostart")) {
            v.keys(*table, {"commands"}, "autostart");
            if (auto *node = table->get("commands")) {
                auto *array = node->as_array();
                if (!array || array->size() > 128)
                    v.fail(node, "autostart.commands", "Expected at most 128 commands");
                else
                    for (const auto &item : *array) {
                        auto value = v.string(&item, "autostart.commands");
                        if (value && !value->empty() && value->size() <= 4096)
                            config.autostart.commands.push_back(std::move(*value));
                        else
                            v.fail(&item, "autostart.commands",
                                   "Expected a nonempty command of at most 4096 bytes");
                    }
            }
        }
        if (auto *table = v.table(parsed, "wallpaper", "wallpaper")) {
            v.keys(*table, {"path"}, "wallpaper");
            if (auto value = v.string(table->get("path"), "wallpaper.path")) {
                if (value->empty() || value->front() != '/' || value->size() > 4096)
                    v.fail(table->get("path"), "wallpaper.path",
                           "Expected an absolute path of at most 4096 bytes");
                else
                    config.wallpaper.path = std::move(*value);
            }
        }
        if (auto *table = v.table(parsed, "window", "window")) {
            v.keys(*table,
                   {"tiling", "titlebars", "layout", "gaps_outer", "gaps_inner", "border_width"},
                   "window");
            v.boolean(*table, "tiling", "window.tiling", config.window.tiling);
            v.boolean(*table, "titlebars", "window.titlebars", config.window.titlebars);
            v.enumeration<Layout>(*table, "layout", "window.layout", config.window.layout,
                                  {{"dwindle", Layout::dwindle}, {"scrolling", Layout::scrolling}});
            v.integer(*table, "gaps_outer", "window.gaps_outer", config.window.gaps_outer, 0, 512);
            v.integer(*table, "gaps_inner", "window.gaps_inner", config.window.gaps_inner, 0, 512);
            v.integer(*table, "border_width", "window.border_width", config.window.border_width, 0,
                      64);
        }
        if (auto *table = v.table(parsed, "workspaces", "workspaces")) {
            v.keys(*table, {"mode"}, "workspaces");
            v.enumeration<WorkspaceMode>(
                *table, "mode", "workspaces.mode", config.workspaces.mode,
                {{"global", WorkspaceMode::global}, {"per-monitor", WorkspaceMode::per_monitor}});
        }
        if (auto *table = v.table(parsed, "monitors", "monitors"))
            parse_monitors(v, *table, config.monitors);
        if (auto *table = v.table(parsed, "dwindle", "dwindle")) {
            v.keys(*table, {"insertion", "split_ratio"}, "dwindle");
            v.enumeration<Insertion>(
                *table, "insertion", "dwindle.insertion", config.dwindle.insertion,
                {{"focused", Insertion::focused}, {"largest", Insertion::largest}});
            if (auto value = v.number(table->get("split_ratio"), "dwindle.split_ratio", 0.05, 0.95))
                config.dwindle.split_ratio = *value;
        }
        if (auto *table = v.table(parsed, "scrolling", "scrolling")) {
            v.keys(*table, {"default_column_width"}, "scrolling");
            if (auto value = v.number(table->get("default_column_width"),
                                      "scrolling.default_column_width", 0.10, 4.0))
                config.scrolling.default_column_width = *value;
        }
        if (auto *table = v.table(parsed, "input", "input")) {
            v.keys(*table, {"mouse"}, "input");
            if (auto *mouse = v.table(*table, "mouse", "input.mouse")) {
                v.keys(*mouse, {"profile", "sensitivity"}, "input.mouse");
                if (auto profile = v.string(mouse->get("profile"), "input.mouse.profile")) {
                    if (*profile != "flat")
                        v.fail(mouse->get("profile"), "input.mouse.profile",
                               "Only flat is supported");
                }
                if (auto value =
                        v.number(mouse->get("sensitivity"), "input.mouse.sensitivity", -1.0, 1.0))
                    config.input.mouse.sensitivity = *value;
            }
        }
        if (auto *table = v.table(parsed, "idle", "idle")) {
            v.keys(*table, {"display_off_after_minutes"}, "idle");
            v.integer(*table, "display_off_after_minutes", "idle.display_off_after_minutes",
                      config.idle.display_off_after_minutes, 0, 1440);
        }
        if (auto *table = v.table(parsed, "render", "render")) {
            v.keys(*table,
                   {"direct_scanout", "adaptive_sync", "allow_tearing", "fullscreen_aspect"},
                   "render");
            v.boolean(*table, "direct_scanout", "render.direct_scanout",
                      config.render.direct_scanout);
            v.boolean(*table, "allow_tearing", "render.allow_tearing", config.render.allow_tearing);
            v.enumeration<AdaptiveSync>(*table, "adaptive_sync", "render.adaptive_sync",
                                        config.render.adaptive_sync,
                                        {{"off", AdaptiveSync::off},
                                         {"fullscreen", AdaptiveSync::fullscreen},
                                         {"always", AdaptiveSync::always}});
            v.enumeration<FullscreenAspect>(
                *table, "fullscreen_aspect", "render.fullscreen_aspect",
                config.render.fullscreen_aspect,
                {{"preserve", FullscreenAspect::preserve}, {"stretch", FullscreenAspect::stretch}});
        }
        if (auto *table = v.table(parsed, "night_light", "night_light")) {
            v.keys(*table, {"enabled", "temperature_kelvin"}, "night_light");
            v.boolean(*table, "enabled", "night_light.enabled", config.night_light.enabled);
            v.integer(*table, "temperature_kelvin", "night_light.temperature_kelvin",
                      config.night_light.temperature_kelvin, 1000, 10000);
        }
        if (auto *table = v.table(parsed, "keybinds", "keybinds")) {
            config.keybinds.clear();
            for (const auto &[key, node] : *table) {
                auto action_text = v.string(&node, "keybinds");
                if (!action_text)
                    break;
                auto chord = parse_chord(key.str());
                if (!chord) {
                    v.fail(&node, "keybinds", chord.error().message);
                    break;
                }
                auto action = parse_action(*action_text, *chord);
                if (!action) {
                    v.fail(&node, "keybinds", action.error().message);
                    break;
                }
                if (!config.keybinds.emplace(*chord, std::move(*action)).second) {
                    v.fail(&node, "keybinds", "Duplicate normalized chord");
                    break;
                }
            }
        }
        if (v.failure)
            return std::unexpected(std::move(*v.failure));
        return config;
    } catch (const toml::parse_error &error) {
        return std::unexpected(Diagnostic{bounded(path, 4096), error.source().begin.line,
                                          error.source().begin.column, "",
                                          bounded(error.description(), 512)});
    }
}

MonitorSettings effective_monitor_settings(const MonitorSettings &defaults,
                                           const MonitorOverride &override) {
    auto result = defaults;
    if (override.resolution)
        result.resolution = *override.resolution;
    if (override.refresh_rate)
        result.refresh_rate = *override.refresh_rate;
    if (override.scale)
        result.scale = *override.scale;
    if (override.rotation)
        result.rotation = *override.rotation;
    if (override.position)
        result.position = *override.position;
    return result;
}

std::expected<void, Diagnostic>
validate_monitor_matches(const Config &config, const std::vector<LiveOutputIdentity> &outputs) {
    const auto matching = [&](const OutputSelector &selector, const LiveOutputIdentity &output) {
        return selector.value ==
               (selector.kind == SelectorKind::identity ? output.identity : output.connector);
    };
    const auto failure = [](std::string_view field, std::string_view message) {
        return std::unexpected(Diagnostic{"", 0, 0, std::string(field), std::string(message)});
    };
    if (config.monitors.primary) {
        auto count = std::count_if(outputs.begin(), outputs.end(), [&](const auto &output) {
            return matching(*config.monitors.primary, output);
        });
        if (count > 1)
            return failure("monitors.primary", "Ambiguous primary output selector");
    }
    std::set<std::size_t> claimed;
    for (const auto &entry : config.monitors.output) {
        std::optional<std::size_t> matched;
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            if (matching(entry.match, outputs[i])) {
                if (matched)
                    return failure("monitors.output.match", "Ambiguous output selector");
                matched = i;
            }
        }
        if (matched && !claimed.insert(*matched).second)
            return failure("monitors.output.match", "Duplicate overrides for one connected output");
    }
    return {};
}

} // namespace seaside

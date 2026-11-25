#include "config_edit.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <toml++/toml.hpp>

namespace seaside {
namespace {

struct SettingName {
    Setting setting;
    std::string_view name;
};

constexpr std::array settings{
    SettingName{Setting::keybinds, "keybinds"},
    SettingName{Setting::night_light_enabled, "night_light.enabled"},
    SettingName{Setting::night_light_temperature, "night_light.temperature_kelvin"},
    SettingName{Setting::power_profile, "power.profile"},
    SettingName{Setting::idle_minutes, "idle.display_off_after_minutes"},
    SettingName{Setting::allow_edge_drag, "window.allow_edge_drag"},
};

SettingError invalid(std::string message) {
    return {"invalid_args", std::move(message)};
}

std::string quoted(std::string value) {
    std::ostringstream out;
    out << toml::toml_formatter(toml::value{std::move(value)});
    return std::move(out).str();
}

bool is_value_line(const toml::table &table, unsigned line) {
    return std::ranges::any_of(table, [&](const auto &entry) {
        const auto &node = entry.second;
        if (const auto *nested = node.as_table(); nested && !nested->is_inline())
            return is_value_line(*nested, line);
        const auto &region = node.source();
        return region.begin.line <= line && line <= region.end.line;
    });
}

// Keep TOML formatting and comments outside the edited assignments. toml++
// reports columns in Unicode code points; translate those to UTF-8 byte offsets.
class TextEdit {
  public:
    explicit TextEdit(std::string_view text)
        : text_(text), newline_(text.find("\r\n") == text.npos ? "\n" : "\r\n") {
        lines_.push_back(text.starts_with("\xef\xbb\xbf") ? 3 : 0);
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n')
                lines_.push_back(i + 1);
        }
    }

    void replace(toml::source_region region, std::string value) {
        add(offset(region.begin), offset(region.end), std::move(value));
    }

    void erase_line(toml::source_region region) {
        const auto start = lines_.at(region.begin.line - 1);
        const auto end = offset(region.end);
        const auto tail = text_.find_first_not_of(" \t", end);
        if (tail != text_.npos && text_[tail] == '#') {
            const auto first = text_.find_first_not_of(" \t", start);
            add(first, tail, {});
        } else {
            add(start, after_line(end), {});
        }
    }

    bool explicit_table(const toml::table &table) const {
        return !table.is_inline() && text_[offset(table.source().begin)] == '[';
    }

    void retire_binding_example(const toml::table &root) {
        if (!text_.contains(legacy_move_binding_example))
            return;
        for (std::size_t line = 0; line < lines_.size(); ++line) {
            const auto begin = lines_[line];
            const auto end = text_.find_first_of("\r\n", begin);
            const auto count = end == text_.npos ? text_.npos : end - begin;
            if (text_.substr(begin, count) == legacy_move_binding_example &&
                !is_value_line(root, line + 1))
                add(begin + 2, begin + 2, "Example: ");
        }
    }

    void append_table(std::string_view group, std::string body = {}) {
        std::string value = "[" + std::string(group) + "]" + std::string(newline_);
        value += body;
        insert_line(text_.size(), std::move(value));
    }

    void insert(const toml::table &table, std::string_view group, std::string key,
                std::string value) {
        const auto position = table.source().end;
        if (explicit_table(table)) {
            insert_line(after_line(offset(position)), key + " = " + value);
            return;
        }
        // Dotted keys must stay in the enclosing root scope.
        auto last = position;
        for (const auto &[existing, node] : table)
            last = std::max(last, node.source().end);
        insert_line(after_line(offset(last)), std::string(group) + "." + key + " = " + value);
    }

    std::string finish(std::string text) && {
        std::ranges::stable_sort(edits_, std::greater<>{}, &Edit::begin);
        for (const auto &edit : edits_)
            text.replace(edit.begin, edit.end - edit.begin, edit.value);
        return text;
    }

    std::string_view newline() const { return newline_; }
    bool empty() const { return edits_.empty(); }

  private:
    struct Edit {
        std::size_t begin, end;
        std::string value;
    };

    std::size_t offset(toml::source_position position) const {
        auto at = lines_.at(position.line - 1);
        for (unsigned column = 1; column < position.column && at < text_.size(); ++column) {
            do {
                ++at;
            } while (at < text_.size() && (static_cast<unsigned char>(text_[at]) & 0xc0U) == 0x80U);
        }
        return at;
    }

    std::size_t after_line(std::size_t position) const {
        const auto newline = text_.find('\n', position);
        return newline == text_.npos ? text_.size() : newline + 1;
    }

    void add(std::size_t begin, std::size_t end, std::string value) {
        edits_.push_back({begin, end, std::move(value)});
    }

    void insert_line(std::size_t position, std::string value) {
        if (position > lines_.front() && text_[position - 1] != '\n')
            value.insert(0, newline_);
        if (!value.ends_with('\n'))
            value += newline_;
        add(position, position, std::move(value));
    }

    std::string_view text_, newline_;
    std::vector<std::size_t> lines_;
    std::vector<Edit> edits_;
};

void rewrite_inline(TextEdit &patch, const toml::table &original, std::string_view key,
                    const auto &value) {
    auto table = original;
    if (value)
        table.insert_or_assign(key, *value);
    else
        table.erase(key);
    std::ostringstream out;
    out << toml::toml_formatter(table);
    patch.replace(original.source(), std::move(out).str());
}

std::expected<BindingMap, SettingError> rewrite_binding(TextEdit &patch, const toml::table &root,
                                                        const BindingMap &active,
                                                        const SettingEdit &edit) {
    const auto *action = std::get_if<std::string>(&edit.value);
    if ((!action && !std::holds_alternative<std::monostate>(edit.value)) || (action && !edit.chord))
        return std::unexpected(invalid("Expected a keybind chord and action, or a removal"));
    constexpr std::string_view group = "keybinds";
    const auto *table = root[group].as_table();
    if (!edit.chord) {
        if (table && table->is_inline())
            patch.erase_line(table->source());
        else if (table) {
            if (patch.explicit_table(*table))
                patch.erase_line(table->source());
            for (const auto &[key, node] : *table)
                patch.erase_line({key.source().begin, node.source().end, {}});
        }
        return default_bindings();
    }

    auto desired = active;
    if (action) {
        auto parsed = parse_action(*action, *edit.chord);
        if (!parsed)
            return std::unexpected(invalid(parsed.error().message));
        desired.insert_or_assign(*edit.chord, std::move(*parsed));
    } else {
        desired.erase(*edit.chord);
    }
    const auto name = format_chord(*edit.chord);
    if (!table) {
        if (desired == active)
            return desired;
        std::string body;
        for (const auto &[chord, binding] : desired)
            body += quoted(format_chord(chord)) + " = " +
                    quoted(action && chord == *edit.chord ? *action : format_action(binding)) +
                    std::string(patch.newline());
        patch.append_table(group, std::move(body));
        return desired;
    }

    auto existing = table->end();
    auto implicit = active;
    for (auto it = table->begin(); it != table->end(); ++it) {
        const auto chord = parse_chord(it->first.str());
        if (chord) {
            implicit.erase(*chord);
            if (*chord == *edit.chord)
                existing = it;
        }
    }
    if (!action && existing == table->end() && !active.contains(*edit.chord))
        return desired;
    if (action && existing != table->end() && existing->second.value<std::string_view>() == *action)
        return desired;

    // Old stock tables get additional active bindings on read. Preserve
    // those chords when editing and retire the template's migration marker.
    patch.retire_binding_example(root);
    implicit.erase(*edit.chord);
    if (table->is_inline() && !implicit.empty()) {
        auto updated = *table;
        const auto key = existing == table->end() ? name : std::string(existing->first.str());
        if (action)
            updated.insert_or_assign(key, *action);
        else
            updated.erase(key);
        for (const auto &[chord, binding] : implicit)
            updated.insert(format_chord(chord), format_action(binding));
        std::ostringstream out;
        out << toml::toml_formatter(updated);
        patch.replace(table->source(), std::move(out).str());
        return desired;
    }
    if (existing != table->end()) {
        const auto &[key, node] = *existing;
        if (action) {
            patch.replace(node.source(), quoted(*action));
        } else if (table->is_inline()) {
            rewrite_inline(patch, *table, key.str(), std::optional<std::string>{});
        } else if (table->size() == 1 && !patch.explicit_table(*table)) {
            patch.erase_line({key.source().begin, node.source().end, {}});
            patch.append_table(group);
        } else {
            patch.erase_line({key.source().begin, node.source().end, {}});
        }
    } else if (action) {
        if (table->is_inline())
            rewrite_inline(patch, *table, name, std::optional{*action});
        else
            patch.insert(*table, group, quoted(name), quoted(*action));
    }
    for (const auto &[chord, binding] : implicit)
        patch.insert(*table, group, quoted(format_chord(chord)), quoted(format_action(binding)));
    return desired;
}

using Scalar = std::variant<bool, int, std::string>;

std::expected<std::optional<Scalar>, SettingError> scalar_value(const Config &active,
                                                                const SettingEdit &edit) {
    if (edit.chord)
        return std::unexpected(invalid("Only keybind settings accept a chord"));
    if (edit.setting == Setting::night_light_enabled) {
        if (const auto *action = std::get_if<NightLightAction>(&edit.value))
            return *action == NightLightAction::toggle ? !active.night_light.enabled
                                                       : *action == NightLightAction::on;
    }
    switch (edit.setting) {
    case Setting::night_light_enabled:
    case Setting::allow_edge_drag:
        if (const auto *value = std::get_if<bool>(&edit.value))
            return *value;
        break;
    case Setting::night_light_temperature:
    case Setting::idle_minutes:
        if (const auto *value = std::get_if<int>(&edit.value))
            return *value;
        break;
    case Setting::power_profile:
        if (const auto *value = std::get_if<std::string>(&edit.value))
            return *value;
        if (std::holds_alternative<std::monostate>(edit.value))
            return std::nullopt;
        break;
    case Setting::keybinds:
        break;
    }
    return std::unexpected(invalid("Invalid value for this setting"));
}

void rewrite_scalar(TextEdit &patch, const toml::table &root, std::string_view name,
                    const std::optional<Scalar> &value) {
    const auto separator = name.find('.');
    const auto group = name.substr(0, separator);
    const auto key = name.substr(separator + 1);
    const auto *table = root[group].as_table();
    const auto *node = table ? table->get(key) : nullptr;
    if (!value) {
        if (!node)
            return;
        if (table->is_inline())
            rewrite_inline(patch, *table, key, std::optional<std::string>{});
        else
            patch.erase_line(node->source());
        return;
    }
    std::visit(
        [&](const auto &typed) {
            if (node && node->value<std::decay_t<decltype(typed)>>() == typed)
                return;
            std::ostringstream out;
            out << toml::toml_formatter(toml::value{typed});
            auto encoded = std::move(out).str();
            if (node)
                patch.replace(node->source(), std::move(encoded));
            else if (!table)
                patch.append_table(group, std::string(key) + " = " + encoded);
            else if (table->is_inline())
                rewrite_inline(patch, *table, key, std::optional{typed});
            else
                patch.insert(*table, group, std::string(key), std::move(encoded));
        },
        *value);
}
} // namespace

std::optional<Setting> parse_setting(std::string_view name) noexcept {
    const auto setting = std::ranges::find(settings, name, &SettingName::name);
    return setting == settings.end() ? std::nullopt : std::optional{setting->setting};
}

std::expected<std::string, SettingError> rewrite_setting(std::string text, const Config &active,
                                                         const SettingEdit &edit) {
    std::optional<BindingMap> bindings;
    TextEdit patch(text);
    {
        const auto root = toml::parse(text);
        if (edit.setting == Setting::keybinds) {
            auto result = rewrite_binding(patch, root, active.keybinds, edit);
            if (!result)
                return std::unexpected(std::move(result.error()));
            bindings = std::move(*result);
        } else {
            const auto setting = std::ranges::find(settings, edit.setting, &SettingName::setting);
            if (setting == settings.end())
                return std::unexpected(invalid("Unknown setting"));
            const auto value = scalar_value(active, edit);
            if (!value)
                return std::unexpected(value.error());
            rewrite_scalar(patch, root, setting->name, *value);
        }
    }
    if (patch.empty())
        return text;
    // Complete schema validation happens before the user's file is replaced.
    auto result = std::move(patch).finish(std::move(text));
    const auto validated = parse_config(result);
    if (!validated)
        return std::unexpected(invalid(validated.error().message));
    if (bindings && validated->keybinds != *bindings)
        return std::unexpected(invalid("Could not preserve the remaining active keybinds"));
    return result;
}

} // namespace seaside

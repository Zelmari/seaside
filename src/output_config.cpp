#include "output_config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace seaside {
namespace {
struct Change {
    OutputConfig::Output *output;
    std::string_view connector;
    MonitorSettings settings;
    seaside_drm_output_settings previous, next;
    bool adaptive_sync_rejected;
    bool adaptive_sync_requested;
    int width, height;
};

bool matches(const OutputSelector &selector, const std::string &identity,
             std::string_view connector) {
    return selector.value == (selector.kind == SelectorKind::connector ? connector : identity);
}

std::unexpected<Diagnostic> failure(std::string field, std::string message) {
    return std::unexpected(
        Diagnostic{.path = {}, .field = std::move(field), .message = std::move(message)});
}

bool intersects(const Change &a, const Change &b) {
    return std::int64_t{a.next.x} < std::int64_t{b.next.x} + b.width &&
           std::int64_t{b.next.x} < std::int64_t{a.next.x} + a.width &&
           std::int64_t{a.next.y} < std::int64_t{b.next.y} + b.height &&
           std::int64_t{b.next.y} < std::int64_t{a.next.y} + a.height;
}

bool fits(const Change &change) {
    constexpr auto limit = std::numeric_limits<int>::max();
    return change.width > 0 && change.height > 0 && change.next.x <= limit - change.width &&
           change.next.y <= limit - change.height;
}

auto rank(const OutputConfig::Output &output) {
    return std::tuple{-output.max_refresh_millihz, -output.max_pixels,
                      std::string_view(output.connector)};
}
} // namespace

void OutputConfig::observe(wlr_output *handle, std::string_view connector,
                           std::string_view identity, bool connected, const char *disabled_reason) {
    auto *output = connected_output(handle);
    if (!connected) {
        if (output) {
            output->handle = nullptr;
            output->enabled = false;
            output->status.settings.mode = nullptr;
        }
        return;
    }
    if (!output) {
        const auto description = seaside_tty_drm_describe_output(handle);
        const std::string_view make = description.make ? description.make : "";
        const std::string_view model = description.model ? description.model : "";
        const Resolution preferred{description.preferred_width, description.preferred_height};
        const auto remembered = [&](auto same_display) -> Output * {
            Output *detached = nullptr;
            Output *same_connector = nullptr;
            std::size_t matches = 0, connector_matches = 0;
            for (auto &[id, existing] : outputs_) {
                if (!same_display(existing)) {
                    continue;
                }
                ++matches;
                if (!existing.handle) {
                    detached = &existing;
                    if (existing.connector == connector) {
                        ++connector_matches;
                        same_connector = &existing;
                    }
                }
            }
            // Duplicate serials and identical models are disambiguated only
            // by a unique remembered connector. Never reuse a live record.
            return matches == 1 ? detached : connector_matches == 1 ? same_connector : nullptr;
        };
        if (!identity.empty()) {
            output =
                remembered([&](const Output &existing) { return existing.identity == identity; });
        }
        if (!output) {
            output = remembered([&](const Output &existing) {
                if (!identity.empty() && !existing.identity.empty()) {
                    return false;
                }
                const bool exact_model = make == existing.make && model == existing.model;
                const bool exact_mode = preferred == existing.preferred;
                if (existing.connector != connector) {
                    return !model.empty() && exact_model && preferred.width > 0 && exact_mode;
                }
                return (exact_model || model.empty() || existing.model.empty()) &&
                       (exact_mode || !preferred.width || !existing.preferred.width);
            });
        }
        if (!output) {
            auto id = "out-" + std::to_string(outputs_.size() + 1);
            output = &outputs_.try_emplace(id).first->second;
            output->id = std::move(id);
        }
        output->connector = connector;
        if (!identity.empty()) {
            output->identity = identity;
        }
        output->make = make;
        output->model = model;
        output->preferred = preferred;
        output->max_refresh_millihz = description.max_refresh_millihz;
        output->max_pixels = description.max_pixels;
        output->adaptive_sync_requested = false;
        output->adaptive_sync_rejected = false;
        output->adaptive_sync_supported = description.adaptive_sync_supported;
        output->handle = handle;
    }
    output->disabled_reason = disabled_reason ? disabled_reason : "";
}

const OutputConfig::Output *OutputConfig::enabled(wlr_output *handle, bool usable) {
    auto *output = connected_output(handle);
    if (output) {
        output->enabled = usable;
        if (usable) {
            seaside_tty_drm_output_status(drm_, handle, &output->status);
        }
    }
    return output;
}

OutputConfig::Output *OutputConfig::connected_output(wlr_output *handle) noexcept {
    if (!handle) {
        return nullptr;
    }
    for (auto &[id, output] : outputs_) {
        if (output.handle == handle) {
            return &output;
        }
    }
    return nullptr;
}

std::string_view OutputConfig::ranked_fallback() const noexcept {
    const Output *best = nullptr;
    for (const auto &[id, output] : outputs_) {
        if (output.enabled && (!best || rank(output) < rank(*best))) {
            best = &output;
        }
    }
    return best ? std::string_view(best->id) : std::string_view{};
}

void OutputConfig::refresh() {
    for (auto &[id, output] : outputs_) {
        if (output.enabled) {
            seaside_tty_drm_output_status(drm_, output.handle, &output.status);
        }
    }
}

std::expected<void, Diagnostic> OutputConfig::apply(const Config &config, bool project_geometry) {
    std::vector<LiveOutputIdentity> connected;
    for (const auto &[id, output] : outputs_) {
        if (output.handle) {
            connected.push_back({output.identity, output.connector});
        }
    }
    if (auto valid = validate_monitor_matches(config, connected); !valid) {
        return valid;
    }
    refresh();
    std::vector<Change> changes;
    for (auto &[id, output] : outputs_) {
        if (!output.handle) {
            continue;
        }
        const auto &connector = output.connector;
        auto settings = config.monitors.defaults;
        for (const auto &entry : config.monitors.output) {
            if (matches(entry.match, output.identity, connector)) {
                settings = effective_monitor_settings(settings, entry);
                break;
            }
        }
        const auto refresh = settings.refresh_rate.value_or(0) * 1000;
        if (refresh > std::numeric_limits<int>::max() || (refresh > 0 && refresh < .5)) {
            return failure("monitors.refresh_rate", "Unsupported refresh rate for " + connector);
        }
        const auto size = settings.resolution.value_or(Resolution{});
        seaside_drm_output_settings next{.mode = nullptr,
                                         .width = size.width,
                                         .height = size.height,
                                         .refresh_millihz = static_cast<int>(std::lround(refresh)),
                                         .x = 0,
                                         .y = 0,
                                         .rotation = settings.rotation,
                                         .scale = static_cast<float>(settings.scale),
                                         .adaptive_sync = false};
        if (!seaside_tty_drm_select_mode(output.handle, &next)) {
            auto automatic = next;
            automatic.refresh_millihz = 0;
            if (next.refresh_millihz && seaside_tty_drm_select_mode(output.handle, &automatic)) {
                return failure("monitors.refresh_rate",
                               "Unsupported refresh rate for " + connector + " at " +
                                   std::to_string(automatic.width) + "x" +
                                   std::to_string(automatic.height) +
                                   ". Use auto or select a supported rate for this output");
            }
            return failure("monitors.resolution", "Unsupported resolution for " + connector);
        }
        if (!output.enabled) {
            continue;
        }
        if (!seaside_tty_drm_check_settings(output.handle, &next)) {
            return failure("monitors",
                           "Output test rejected mode, scale or rotation for " + connector);
        }
        const bool should_enable = config.render.adaptive_sync == AdaptiveSync::always ||
                                   (config.render.adaptive_sync == AdaptiveSync::fullscreen &&
                                    desktop_.windows().has_fullscreen(id));
        bool sync_rejected = false;
        if (should_enable && output.adaptive_sync_supported.value_or(false)) {
            next.adaptive_sync = true;
            if (!seaside_tty_drm_check_settings(output.handle, &next)) {
                next.adaptive_sync = false;
                sync_rejected = true;
            }
        }
        const bool rotated = settings.rotation == 90 || settings.rotation == 270;
        const auto width =
            static_cast<int>(static_cast<float>(rotated ? next.height : next.width) / next.scale);
        const auto height =
            static_cast<int>(static_cast<float>(rotated ? next.width : next.height) / next.scale);
        changes.push_back({&output, connector, settings, output.status.settings, next,
                           sync_rejected, should_enable, width, height});
    }
    const auto configured = std::ranges::find_if(changes, [&](const auto &change) {
        return config.monitors.primary &&
               matches(*config.monitors.primary, change.output->identity, change.connector);
    });
    const Output *preferred = configured == changes.end() ? nullptr : configured->output;
    if (!preferred && primary_ && (config.monitors.primary || !configured_primary_)) {
        // The window manager has already migrated workspaces on removal and
        // selected a fallback using focus history. Keep that primary stable.
        const auto remembered = primary_->enabled ? std::string_view(primary_->id)
                                                  : desktop_.windows().primary_output();
        const auto previous = std::ranges::find_if(
            changes, [&](const auto &change) { return change.output->id == remembered; });
        if (previous != changes.end()) {
            preferred = previous->output;
        }
    }
    const auto placement_order = [preferred](const Change &change) {
        return std::tuple{change.output != preferred, rank(*change.output)};
    };
    std::ranges::sort(changes, [&](const Change &a, const Change &b) {
        return placement_order(a) < placement_order(b);
    });
    std::vector<const Change *> placed;
    for (auto &change : changes) {
        if (const auto position = change.settings.position) {
            change.next.x = position->x;
            change.next.y = position->y;
            if (!fits(change) || std::ranges::any_of(placed, [&](const Change *other) {
                    return intersects(change, *other);
                })) {
                return failure("monitors.position", "Invalid or overlapping position for " +
                                                        std::string(change.connector));
            }
            placed.push_back(&change);
        }
    }
    int next_x = 0;
    for (auto &change : changes) {
        if (!change.settings.position) {
            change.next.x = next_x;
            for (std::size_t pass = 0; pass < placed.size(); ++pass) {
                for (const auto *other : placed) {
                    if (intersects(change, *other)) {
                        change.next.x = other->next.x + other->width;
                    }
                }
            }
            if (!fits(change)) {
                return failure("monitors.position", "Automatic output arrangement is out of range");
            }
            next_x = change.next.x + change.width;
            placed.push_back(&change);
        }
    }

    std::size_t applied = 0;
    for (auto &change : changes) {
        bool committed = seaside_tty_drm_apply_settings(drm_, change.output->handle, &change.next);
        if (!committed && change.next.adaptive_sync) {
            // Adaptive sync is optional for a usable monitor. Retry the same
            // transaction with the fixed-refresh state already tested above.
            change.next.adaptive_sync = false;
            change.adaptive_sync_rejected = true;
            committed = seaside_tty_drm_apply_settings(drm_, change.output->handle, &change.next);
        }
        if (!committed) {
            const bool failed =
                !seaside_tty_drm_apply_settings(drm_, change.output->handle, &change.previous);
            if (failed) {
                seaside_tty_drm_reject_unrestorable(drm_, change.output->handle);
            }
            while (applied) {
                const auto &prior = changes[--applied];
                if (!seaside_tty_drm_apply_settings(drm_, prior.output->handle, &prior.previous)) {
                    seaside_tty_drm_reject_unrestorable(drm_, prior.output->handle);
                }
            }
            seaside_tty_drm_reconcile_cursor(drm_);
            return failure("monitors", "Output commit failed for " + std::string(change.connector));
        }
        ++applied;
    }
    seaside_tty_drm_reconcile_cursor(drm_);
    primary_ = changes.empty() ? nullptr : changes.front().output;
    configured_primary_ = config.monitors.primary.has_value();
    if (project_geometry) {
        set_primary();
    }
    refresh();
    for (const auto &change : changes) {
        auto &output = *change.output;
        const auto &status = output.status;
        output.adaptive_sync_rejected = change.adaptive_sync_rejected;
        output.adaptive_sync_requested = change.adaptive_sync_requested;
        seaside_tty_drm_set_direct_scanout(drm_, output.handle, config.render.direct_scanout);
        const WindowRect bounds{status.settings.x, status.settings.y, status.logical_width,
                                status.logical_height};
        if (project_geometry && bounds != desktop_.output_bounds(output.handle)) {
            desktop_.update_output(output.handle, bounds);
        }
    }
    return {};
}

void OutputConfig::set_primary() {
    if (primary_) {
        desktop_.set_primary_output(primary_->id);
    }
}

void OutputConfig::sync_fullscreen(const Config &config) {
    if (config.render.adaptive_sync != AdaptiveSync::fullscreen) {
        return;
    }
    for (auto &[id, output] : outputs_) {
        if (!output.enabled) {
            continue;
        }
        const bool desired = desktop_.windows().has_fullscreen(id);
        if (desired == output.adaptive_sync_requested) {
            continue;
        }
        output.adaptive_sync_requested = desired;
        output.adaptive_sync_rejected = false;
        if (!output.adaptive_sync_supported.value_or(false)) {
            continue;
        }
        if (!seaside_tty_drm_output_status(drm_, output.handle, &output.status) ||
            output.status.settings.adaptive_sync == desired) {
            continue;
        }
        auto settings = output.status.settings;
        settings.adaptive_sync = desired;
        if (seaside_tty_drm_check_settings(output.handle, &settings) &&
            seaside_tty_drm_apply_settings(drm_, output.handle, &settings)) {
            seaside_tty_drm_output_status(drm_, output.handle, &output.status);
        } else {
            output.adaptive_sync_rejected = true;
        }
    }
}

} // namespace seaside

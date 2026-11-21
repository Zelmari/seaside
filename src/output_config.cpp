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
    std::optional<bool> adaptive_sync_supported;
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
} // namespace

void OutputConfig::observe(wlr_output *handle, std::string_view connector,
                           std::string_view identity, bool connected, const char *disabled_reason) {
    auto [entry, inserted] = outputs_.try_emplace(std::string(connector));
    auto &output = entry->second;
    if (inserted) {
        output.id = "out-" + std::to_string(outputs_.size());
    }
    if (connected) {
        output.handle = handle;
        output.identity = identity;
        output.disabled_reason = disabled_reason ? disabled_reason : "";
    } else {
        output.handle = nullptr;
        output.enabled = false;
        output.status.settings.mode = nullptr;
    }
}

void OutputConfig::enabled(std::string_view connector, bool usable) {
    auto found = outputs_.find(connector);
    if (found != outputs_.end()) {
        found->second.enabled = usable;
    }
}

const std::string *OutputConfig::id(std::optional<std::string_view> connector) const noexcept {
    if (connector) {
        const auto found = outputs_.find(*connector);
        if (found != outputs_.end()) {
            return &found->second.id;
        }
    }
    return nullptr;
}

void OutputConfig::refresh() {
    for (auto &[connector, output] : outputs_) {
        if (output.enabled) {
            seaside_tty_drm_output_status(drm_, output.handle, &output.status);
        }
    }
}

std::expected<void, Diagnostic> OutputConfig::apply(const Config &config, bool project_geometry) {
    std::vector<LiveOutputIdentity> connected;
    for (const auto &[connector, output] : outputs_) {
        if (output.handle) {
            connected.push_back({output.identity, connector});
        }
    }
    if (auto valid = validate_monitor_matches(config, connected); !valid) {
        return valid;
    }
    refresh();
    std::vector<Change> changes;
    for (auto &[connector, output] : outputs_) {
        if (!output.handle) {
            continue;
        }
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
            return failure("monitors.resolution",
                           "Unsupported resolution or refresh for " + connector);
        }
        if (!output.enabled) {
            continue;
        }
        if (!seaside_tty_drm_check_settings(output.handle, &next)) {
            return failure("monitors",
                           "Output test rejected mode, scale or rotation for " + connector);
        }
        auto capable = output.adaptive_sync_supported;
        const bool should_enable = config.render.adaptive_sync == AdaptiveSync::always ||
                                   (config.render.adaptive_sync == AdaptiveSync::fullscreen &&
                                    desktop_.windows().has_fullscreen(connector));
        if (should_enable) {
            next.adaptive_sync = true;
            capable = seaside_tty_drm_check_settings(output.handle, &next);
            next.adaptive_sync = *capable;
        }
        const bool rotated = settings.rotation == 90 || settings.rotation == 270;
        const auto width = static_cast<int>((rotated ? next.height : next.width) / next.scale);
        const auto height = static_cast<int>((rotated ? next.width : next.height) / next.scale);
        changes.push_back({&output, connector, settings, output.status.settings, next, capable,
                           should_enable, width, height});
    }
    const auto rank = [&config](const Change &change) {
        const bool primary =
            config.monitors.primary &&
            matches(*config.monitors.primary, change.output->identity, change.connector);
        return std::tuple{!primary, -change.next.refresh_millihz,
                          -std::int64_t{change.next.width} * change.next.height, change.connector};
    };
    std::ranges::sort(changes, [&](const Change &a, const Change &b) { return rank(a) < rank(b); });
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
    for (const auto &change : changes) {
        if (!seaside_tty_drm_apply_settings(drm_, change.output->handle, &change.next)) {
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
    primary_ = changes.empty() ? std::string_view{} : changes.front().connector;
    if (project_geometry) {
        set_primary();
    }
    refresh();
    for (const auto &change : changes) {
        auto &output = *change.output;
        const auto &status = output.status;
        output.adaptive_sync_supported = change.adaptive_sync_supported;
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
    if (!primary_.empty()) {
        desktop_.set_primary_output(primary_);
    }
}

void OutputConfig::sync_fullscreen(const Config &config) {
    if (config.render.adaptive_sync != AdaptiveSync::fullscreen) {
        return;
    }
    for (auto &[connector, output] : outputs_) {
        if (!output.enabled) {
            continue;
        }
        const bool desired = desktop_.windows().has_fullscreen(connector);
        if (desired == output.adaptive_sync_requested) {
            continue;
        }
        output.adaptive_sync_requested = desired;
        if (!seaside_tty_drm_output_status(drm_, output.handle, &output.status) ||
            output.status.adaptive_sync == desired) {
            continue;
        }
        auto settings = output.status.settings;
        settings.adaptive_sync = desired;
        if (seaside_tty_drm_check_settings(output.handle, &settings) &&
            seaside_tty_drm_apply_settings(drm_, output.handle, &settings)) {
            if (desired) {
                output.adaptive_sync_supported = true;
            }
            seaside_tty_drm_output_status(drm_, output.handle, &output.status);
        } else if (desired) {
            output.adaptive_sync_supported = false;
        }
    }
}

} // namespace seaside

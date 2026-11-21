#ifndef SEASIDE_OUTPUT_CONFIG_HPP
#define SEASIDE_OUTPUT_CONFIG_HPP

#include "config.hpp"
#include "desktop.hpp"
#include "tty_drm.h"

namespace seaside {

class OutputConfig {
  public:
    struct Output {
        std::string id, connector, identity, make, model, disabled_reason;
        Resolution preferred;
        int max_refresh_millihz = 0;
        std::int64_t max_pixels = 0;
        wlr_output *handle = nullptr;
        bool enabled = false;
        bool adaptive_sync_requested = false;
        bool adaptive_sync_rejected = false;
        std::optional<bool> adaptive_sync_supported;
        seaside_drm_output_status status{};
    };

    OutputConfig(seaside_tty_drm *drm, Desktop &desktop) : drm_(drm), desktop_(desktop) {}
    void observe(wlr_output *handle, std::string_view connector, std::string_view identity,
                 bool connected, const char *disabled_reason);
    const Output *enabled(wlr_output *handle, bool usable);
    std::expected<void, Diagnostic> apply(const Config &config, bool project_geometry = true);
    void sync_fullscreen(const Config &config);
    void set_primary();
    void refresh();
    const auto &outputs() const noexcept { return outputs_; }
    const Output *primary() const noexcept { return primary_; }
    std::string_view ranked_fallback() const noexcept;

  private:
    Output *connected_output(wlr_output *handle) noexcept;

    seaside_tty_drm *drm_;
    Desktop &desktop_;
    std::map<std::string, Output, std::less<>> outputs_;
    const Output *primary_ = nullptr;
    bool configured_primary_ = false;
};

} // namespace seaside
#endif

#ifndef SEASIDE_OUTPUT_CONFIG_HPP
#define SEASIDE_OUTPUT_CONFIG_HPP

#include "config.hpp"
#include "desktop.hpp"
#include "tty_drm.h"

namespace seaside {

class OutputConfig {
  public:
    struct Output {
        std::string id, identity, disabled_reason;
        wlr_output *handle = nullptr;
        bool enabled = false;
        bool adaptive_sync_requested = false;
        std::optional<bool> adaptive_sync_supported;
        seaside_drm_output_status status{};
    };

    OutputConfig(seaside_tty_drm *drm, Desktop &desktop) : drm_(drm), desktop_(desktop) {}
    void observe(wlr_output *handle, std::string_view connector, std::string_view identity,
                 bool connected, const char *disabled_reason);
    void enabled(std::string_view connector, bool usable);
    std::expected<void, Diagnostic> apply(const Config &config, bool project_geometry = true);
    void sync_fullscreen(const Config &config);
    void set_primary();
    void refresh();
    const auto &outputs() const noexcept { return outputs_; }
    const std::string *id(std::optional<std::string_view> connector) const noexcept;

  private:
    seaside_tty_drm *drm_;
    Desktop &desktop_;
    std::map<std::string, Output, std::less<>> outputs_;
    std::string_view primary_;
};

} // namespace seaside
#endif

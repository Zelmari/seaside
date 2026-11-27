#ifndef SEASIDE_SESSION_BROWSER_HPP
#define SEASIDE_SESSION_BROWSER_HPP

#include "config_store.hpp"

#include <array>

namespace seaside {

inline constexpr std::array browser_mime_types{"x-scheme-handler/http", "x-scheme-handler/https",
                                               "text/html", "application/xhtml+xml"};

struct BrowserStatus {
    std::string requested;
    std::string_view state = "checking";
    std::array<std::optional<std::string>, browser_mime_types.size()> effective;
    std::string message;
};

// Requires a validated application ID and XDG_CURRENT_DESKTOP=Seaside.
// Uses blocking storage, so call before dispatch or on a worker.
BrowserStatus apply_default_browser(const ConfigStore &store, const std::filesystem::path &path,
                                    std::string requested,
                                    const std::function<bool()> &still_current = {});

// Private entry point for a one-shot GIO query after updating the MIME file.
int report_browser_defaults();

} // namespace seaside
#endif

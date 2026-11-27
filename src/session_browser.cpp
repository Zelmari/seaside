#include "session_browser.hpp"

#include <cstdio>
#include <cstdlib>
#include <gio/gdesktopappinfo.h>
#include <nlohmann/json.hpp>

namespace seaside {
namespace {
constexpr char defaults[] = "Default Applications";
constexpr char ownership[] = "X-Seaside Browser";
constexpr char selection[] = "selection";

template <class T> using AppInfo = std::unique_ptr<T, decltype(&g_object_unref)>;
using Text = std::unique_ptr<char, decltype(&g_free)>;
using BrowserDefaults = std::array<std::string, browser_mime_types.size()>;
using BrowserSupport = std::array<bool, browser_mime_types.size()>;

struct BrowserEntry {
    BrowserSupport supported{};
    std::string message;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(BrowserEntry, supported, message)

std::optional<std::string> value(GKeyFile *file, const char *group, const char *key) {
    const Text text(g_key_file_get_value(file, group, key, nullptr), g_free);
    if (!text)
        return std::nullopt;
    return std::string(text.get());
}

bool set_value(GKeyFile *file, const char *group, const char *key,
               const std::optional<std::string> &next) {
    if (next == value(file, group, key))
        return false;
    if (next)
        g_key_file_set_value(file, group, key, next->c_str());
    else
        g_key_file_remove_key(file, group, key, nullptr);
    return true;
}

std::string with_browser(const std::string &id, const std::optional<std::string> &previous) {
    std::string result = id + ';';
    if (!previous)
        return result;
    // Keep the original precedence of fallback IDs and avoid duplicating the selected ID.
    std::string_view remaining(*previous);
    while (!remaining.empty()) {
        const auto end = remaining.find(';');
        const auto entry = remaining.substr(0, end);
        if (!entry.empty() && entry != id) {
            result.append(entry);
            result += ';';
        }
        if (end == remaining.npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    return result;
}

struct MimeEdit {
    bool changed = false;
    bool external_override = false;
};

MimeEdit update_defaults(GKeyFile *file, const std::string &id, const BrowserSupport &supported) {
    MimeEdit edit;
    const bool same_selection = value(file, ownership, selection) == id;
    bool owns_any = false;
    for (std::size_t i = 0; i < browser_mime_types.size(); ++i) {
        const auto *mime = browser_mime_types[i];
        const auto previous_key = std::string(mime) + "-previous";
        const auto current = value(file, defaults, mime);
        const auto written = value(file, ownership, mime);
        const bool owned = written && written == current;
        const auto previous = owned ? value(file, ownership, previous_key.c_str()) : current;
        if (!supported[i]) {
            if (owned)
                edit.changed |= set_value(file, defaults, mime, previous);
            edit.changed |= set_value(file, ownership, mime, std::nullopt);
            edit.changed |= set_value(file, ownership, previous_key.c_str(), std::nullopt);
            continue;
        }
        if (same_selection && written && !owned) {
            // An external save supersedes this selection for this type. A
            // different configured browser may use the new value as fallback.
            edit.external_override = true;
            owns_any = true;
            continue;
        }
        const auto next = with_browser(id, previous);
        edit.changed |= set_value(file, defaults, mime, next);
        edit.changed |= set_value(file, ownership, mime, next);
        edit.changed |= set_value(file, ownership, previous_key.c_str(), previous);
        owns_any = true;
    }
    edit.changed |=
        set_value(file, ownership, selection, owns_any ? std::optional{id} : std::nullopt);
    if (!owns_any && edit.changed) {
        const std::unique_ptr<char *, decltype(&g_strfreev)> keys(
            g_key_file_get_keys(file, ownership, nullptr, nullptr), g_strfreev);
        if (keys && !keys.get()[0])
            g_key_file_remove_group(file, ownership, nullptr);
    }
    return edit;
}

std::expected<MimeEdit, std::string>
apply_defaults(const ConfigStore &store, const std::filesystem::path &path, const std::string &id,
               const BrowserSupport &supported, const std::function<bool()> &still_current) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        if (still_current && !still_current())
            return std::unexpected("A newer browser selection is pending");
        // The desktop-specific file may share a directory with the general defaults.
        // Requiring a regular final component keeps a symlink from aliasing them.
        auto snapshot = store.read(path, true);
        if (!snapshot) {
            if (snapshot.error().error == std::errc::resource_unavailable_try_again)
                continue;
            return std::unexpected(snapshot.error().error.message());
        }
        const std::unique_ptr<GKeyFile, decltype(&g_key_file_unref)> file(g_key_file_new(),
                                                                          g_key_file_unref);
        if (*snapshot && !(*snapshot)->empty()) {
            GError *raw = nullptr;
            const auto flags =
                GKeyFileFlags(G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS);
            if (!g_key_file_load_from_data(file.get(), (*snapshot)->data(), (*snapshot)->size(),
                                           flags, &raw)) {
                const std::unique_ptr<GError, decltype(&g_error_free)> error(raw, g_error_free);
                return std::unexpected(error->message);
            }
        }
        const auto edit = update_defaults(file.get(), id, supported);
        if (!edit.changed)
            return edit;
        gsize length = 0;
        const Text output(g_key_file_to_data(file.get(), &length, nullptr), g_free);
        const std::string_view next(output.get(), length);
        const auto saved =
            *snapshot ? store.replace_regular(path, **snapshot, next, still_current)
                      : store.publish(path, next, PublishMode::create_user_file, still_current);
        if ((saved && *saved == PublishResult::written) ||
            (!saved && saved.error().publication_committed))
            return edit;
        if (!saved && saved.error().error != std::errc::resource_unavailable_try_again &&
            saved.error().error != std::errc::no_such_file_or_directory)
            return std::unexpected(saved.error().error.message());
    }
    return std::unexpected("MIME defaults kept changing while Seaside was applying the browser");
}

template <class Result>
std::expected<Result, std::string> query_browser_information(std::string desktop_id = {}) {
    // GIO indexes desktop entries once, including entries inside subdirectories
    // it does not watch. It also invalidates MIME defaults asynchronously. Use a
    // new process for each query so both views reflect the files on disk.
    char executable[] = "/proc/self/exe";
    char option[] = "--browser-query";
    char *args[] = {executable, option, desktop_id.empty() ? nullptr : desktop_id.data(), nullptr};
    char *raw_output = nullptr;
    GError *raw_error = nullptr;
    int wait_status = 0;
    const bool success = g_spawn_sync(nullptr, args, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr,
                                      &raw_output, nullptr, &wait_status, &raw_error) &&
                         g_spawn_check_wait_status(wait_status, &raw_error);
    const Text output(raw_output, g_free);
    const std::unique_ptr<GError, decltype(&g_error_free)> error(raw_error, g_error_free);
    if (!success)
        return std::unexpected(error->message);
    try {
        return nlohmann::json::parse(output.get()).get<Result>();
    } catch (const nlohmann::json::exception &) {
        return std::unexpected("Invalid response from the browser query");
    }
}

BrowserEntry inspect_browser_entry(const char *id) {
    const AppInfo<GDesktopAppInfo> entry(g_desktop_app_info_new(id), g_object_unref);
    if (!entry || g_desktop_app_info_get_is_hidden(entry.get()))
        return {{}, "The desktop entry is unavailable"};

    auto *app = G_APP_INFO(entry.get());
    const auto *types = g_app_info_get_supported_types(app);
    const bool accepts_urls = g_app_info_supports_uris(app) ||
                              g_desktop_app_info_get_boolean(entry.get(), "DBusActivatable");
    if (!types || !accepts_urls || !g_strv_contains(types, browser_mime_types[0]) ||
        !g_strv_contains(types, browser_mime_types[1]))
        return {{}, "The desktop entry must accept HTTP and HTTPS URLs"};

    BrowserEntry result;
    for (std::size_t i = 0; i < result.supported.size(); ++i)
        result.supported[i] = g_strv_contains(types, browser_mime_types[i]);
    return result;
}

BrowserDefaults resolve_browser_defaults() {
    BrowserDefaults defaults;
    for (std::size_t i = 0; i < browser_mime_types.size(); ++i) {
        const AppInfo<GAppInfo> app(g_app_info_get_default_for_type(browser_mime_types[i], false),
                                    g_object_unref);
        if (app) {
            if (const auto *id = g_app_info_get_id(app.get()))
                defaults[i] = id;
        }
    }
    return defaults;
}
} // namespace

BrowserStatus apply_default_browser(const ConfigStore &store, const std::filesystem::path &path,
                                    std::string requested,
                                    const std::function<bool()> &still_current) {
    const std::string id = requested == "firefox" ? "firefox.desktop" : requested;
    BrowserStatus status;
    status.requested = std::move(requested);
    const auto entry = query_browser_information<BrowserEntry>(id);
    if (!entry) {
        status.state = "lookup-error";
        status.message = entry.error();
    } else {
        const auto edit = apply_defaults(store, path, id, entry->supported, still_current);
        if (!edit) {
            status.state = "mimeapps-error";
            status.message = path.string() + ": " + edit.error();
        } else if (!entry->supported.front()) {
            status.state = "unavailable";
            status.message = entry->message;
        } else if (edit->external_override) {
            status.state = "external-override";
        } else {
            status.state = "applied";
        }
    }
    if (auto effective = query_browser_information<BrowserDefaults>(); effective) {
        for (std::size_t i = 0; i < browser_mime_types.size(); ++i) {
            if (status.state == "applied" && entry->supported[i] && (*effective)[i] != id) {
                status.state = "not-effective";
                status.message = "The desktop resolves a different default association";
            }
            if (!(*effective)[i].empty())
                status.effective[i] = std::move((*effective)[i]);
        }
    } else if (status.message.empty()) {
        status.message = effective.error();
    }
    return status;
}

int report_browser_information(const char *desktop_id) {
    const auto response = desktop_id ? nlohmann::json(inspect_browser_entry(desktop_id))
                                     : nlohmann::json(resolve_browser_defaults());
    const auto json = response.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return std::puts(json.c_str()) >= 0 && std::fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
} // namespace seaside

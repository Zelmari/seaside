#ifndef SEASIDE_LIVE_CONFIG_HPP
#define SEASIDE_LIVE_CONFIG_HPP

#include "config_transaction.hpp"

struct wl_event_loop;

namespace seaside {

class LiveConfig final {
  public:
    using Prepared = std::shared_ptr<const void>;
    // Runs on a worker. Capture only data that is safe to use after session shutdown.
    using Prepare = std::function<std::expected<Prepared, Diagnostic>(const Config &candidate,
                                                                      const Config &active)>;
    // Runs on the event loop. A failure must leave the old configuration effective.
    using Apply = std::function<std::expected<void, Diagnostic>(
        const Config &active, const Config &candidate, const Prepared &prepared)>;
    enum class Change { committed, rejected, persistence };
    using Changed = std::function<void(Change, const Prepared &)>;

    // The event loop must outlive the controller. All methods except Prepare
    // callbacks run on the event-loop thread.
    static std::expected<std::unique_ptr<LiveConfig>, std::string>
    create(wl_event_loop *loop, ConfigStore store, ConfigPaths paths, LoadedConfig startup,
           Prepare prepare, Apply apply, Changed changed);
    ~LiveConfig();
    LiveConfig(const LiveConfig &) = delete;
    LiveConfig &operator=(const LiveConfig &) = delete;

    const ConfigTransaction &transaction() const noexcept;
    ConfigSource source() const noexcept;
    void set_runtime_night_light(bool enabled) noexcept;
    void session_ready() noexcept;

  private:
    struct Impl;
    explicit LiveConfig(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace seaside
#endif

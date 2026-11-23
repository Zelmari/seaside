#ifndef SEASIDE_SESSION_PREFERENCES_HPP
#define SEASIDE_SESSION_PREFERENCES_HPP

#include "config.hpp"
#include "session_bus.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct sd_bus_slot;
struct sd_bus_error;

namespace seaside {

class SettingsPortal final {
  public:
    static std::unique_ptr<SettingsPortal> start(SessionBus &bus, ColorScheme scheme) noexcept;
    ~SettingsPortal();
    SettingsPortal(const SettingsPortal &) = delete;
    SettingsPortal &operator=(const SettingsPortal &) = delete;

    void set(ColorScheme scheme) noexcept;

  private:
    SettingsPortal(SessionBus &bus, ColorScheme scheme)
        : bus_(bus), scheme_(std::make_shared<ColorScheme>(scheme)) {}
    static int read(sd_bus_message *message, void *data, struct sd_bus_error *) noexcept;
    static int read_all(sd_bus_message *message, void *data, struct sd_bus_error *) noexcept;
    static int relay_changed(sd_bus_message *message, void *data, struct sd_bus_error *) noexcept;

    SessionBus &bus_;
    std::shared_ptr<ColorScheme> scheme_;
    sd_bus_slot *object_ = nullptr;
    sd_bus_slot *gtk_changes_ = nullptr;
    std::optional<SessionBus::CallId> name_call_;
    bool owns_name_ = false;
};

const char *power_profile_name(PowerProfile profile) noexcept;

class PowerProfiles final {
  public:
    struct Status {
        std::optional<std::string> effective;
        std::vector<std::string> available;
        const char *state = "checking";
    };

    explicit PowerProfiles(wl_event_loop *loop) : loop_(loop) {}
    ~PowerProfiles() = default;
    PowerProfiles(const PowerProfiles &) = delete;
    PowerProfiles &operator=(const PowerProfiles &) = delete;

    void configure(std::optional<PowerProfile> profile) noexcept;
    void tick(bool active) noexcept;
    void stop() noexcept;
    const Status &status() const noexcept { return status_; }

  private:
    using Clock = std::chrono::steady_clock;

    void query() noexcept;
    void read_state(const BusReply &reply) noexcept;
    void select() noexcept;
    void mark_unavailable() noexcept;

    wl_event_loop *loop_;
    std::unique_ptr<SessionBus> bus_;
    std::optional<SessionBus::CallId> pending_;
    std::optional<PowerProfile> requested_;
    Clock::time_point next_{};
    Status status_;
    bool profile_pending_ = false;
    bool active_ = false;
};

} // namespace seaside
#endif

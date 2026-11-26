#ifndef SEASIDE_IDLE_POWER_HPP
#define SEASIDE_IDLE_POWER_HPP

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>

struct wl_display;
struct wl_event_loop;
struct wl_event_source;
struct wlr_output;
struct wlr_surface;

namespace seaside {

class IdlePower {
  public:
    using Clock = std::chrono::steady_clock;
    using Now = std::function<Clock::time_point()>;
    using Power = std::function<bool(wlr_output *, bool)>;
    using VisibleOutput = std::function<wlr_output *(wlr_surface *)>;
    using Fullscreen = std::function<bool(wlr_output *)>;

    static std::unique_ptr<IdlePower> create(wl_event_loop *loop, wl_display *display, Power power,
                                             VisibleOutput visible_output, Fullscreen fullscreen,
                                             Now now = Clock::now);
    ~IdlePower();
    IdlePower(const IdlePower &) = delete;
    IdlePower &operator=(const IdlePower &) = delete;

    bool add(wlr_output *output) noexcept;
    void remove(wlr_output *output) noexcept;
    void set_timeout(std::chrono::minutes timeout) noexcept;
    void set_active(bool active) noexcept;
    void visibility_changed() noexcept;
    void activity() noexcept;

  private:
    struct Retry {
        Clock::time_point after;
        bool sleep;
        bool exhausted = false;
    };
    struct Output {
        std::optional<Retry> retry;
        bool sleeping = false;
    };

    struct Inhibitors;

    IdlePower(Power power, VisibleOutput visible_output, Fullscreen fullscreen, Now now);
    static int on_timer(void *data) noexcept;
    void update();
    void arm(int milliseconds) noexcept;

    Power power_;
    VisibleOutput visible_output_;
    Fullscreen fullscreen_;
    Now now_;
    wl_event_source *timer_ = nullptr;
    std::unique_ptr<Inhibitors> inhibitors_;
    std::map<wlr_output *, Output> outputs_;
    std::chrono::minutes timeout_{15};
    Clock::time_point last_activity_{};
    bool active_ = false;
    bool check_queued_ = false;
    bool sleeping_ = false;
    bool inhibited_ = false;
};

} // namespace seaside
#endif

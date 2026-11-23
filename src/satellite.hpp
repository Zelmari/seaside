#ifndef SEASIDE_SATELLITE_HPP
#define SEASIDE_SATELLITE_HPP

#include <expected>
#include <memory>
#include <string_view>

struct wl_event_loop;

namespace seaside {

// Owns the local X11 listeners, authority file and external satellite child.
// The Wayland event loop must outlive this object.
class Satellite final {
  public:
    static std::expected<std::unique_ptr<Satellite>, int>
    reserve(wl_event_loop *loop, std::string_view wayland_display, std::string_view runtime_dir,
            std::string_view executable = {}, std::string_view socket_root = "/tmp") noexcept;

    ~Satellite() noexcept;
    Satellite(const Satellite &) = delete;
    Satellite &operator=(const Satellite &) = delete;

    std::string_view display_name() const noexcept;
    std::string_view authority_path() const noexcept;

    // Enable connection-driven activation once the graphical session is ready.
    bool enable() noexcept;

  private:
    struct Impl;
    explicit Satellite(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace seaside

#endif

#ifndef SEASIDE_IPC_HPP
#define SEASIDE_IPC_HPP

#include <expected>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

struct wl_event_loop;

namespace seaside {

using IpcJson = nlohmann::json;

struct IpcError {
    std::string code;
    std::string message;
};

using IpcResult = std::expected<IpcJson, IpcError>;

// Construction, state callbacks, event publication, and teardown all belong to
// the Wayland event-loop thread. Destroy after dispatch has stopped.
class IpcServer final {
  public:
    using Handler = std::function<IpcResult(std::string_view method, const IpcJson &params)>;

    // The exit callback requests event-loop termination after the acknowledgement
    // has been written or its deadline expires.
    static std::expected<std::unique_ptr<IpcServer>, std::string>
    create(wl_event_loop *loop, std::string_view wayland_display, Handler handler,
           std::function<void()> on_exit);
    ~IpcServer() noexcept;
    IpcServer(const IpcServer &) = delete;
    IpcServer &operator=(const IpcServer &) = delete;

    const std::string &path() const noexcept;
    void event(std::string_view name, IpcJson data) noexcept;

  private:
    struct Impl;
    explicit IpcServer(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace seaside
#endif

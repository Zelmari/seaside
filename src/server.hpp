#ifndef SEASIDE_SERVER_HPP
#define SEASIDE_SERVER_HPP

#include <expected>
#include <memory>
#include <signal.h>
#include <string>
#include <string_view>

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;
struct wl_event_source;

namespace seaside {

enum class ServerError {
    allocation,
    display_creation,
    invalid_state,
    invalid_arguments,
    allocator_creation,
    renderer_initialization,
    compositor_creation,
    backend_start,
    socket_creation,
    signal_registration,
    shutdown_requested,
};

// All methods and borrowed handles belong to the owning event-loop thread.
class Server final {
  public:
    static std::expected<std::unique_ptr<Server>, ServerError> create() noexcept;
    ~Server() noexcept;
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(Server &&) = delete;

    // Valid non-null arguments transfer ownership, including if startup fails.
    // Invalid arguments/state leave ownership with the caller. A failed start
    // must be finished, not retried against partially initialized globals.
    std::expected<void, ServerError> start(wlr_backend *backend, wlr_renderer *renderer) noexcept;
    // Socket publication and event dispatch do not establish session readiness.
    // Named sockets copy their name; returned views remain valid until finish.
    std::expected<std::string_view, ServerError> listen(std::string_view name = {}) noexcept;
    // The standalone process owns these signal registrations exclusively.
    // Install before creating ANY worker/helper threads so they inherit the
    // blocked mask; join all such threads before finish restores this mask.
    std::expected<void, ServerError> install_shutdown_signals() noexcept;
    std::expected<void, ServerError> run() noexcept;
    void terminate() noexcept;
    bool shutdown_requested() const noexcept { return shutdown_requested_; }
    // Finish only after run returns, never from inside a dispatch callback.
    void finish() noexcept;

    wl_display *display() const noexcept { return display_.get(); }
    wlr_backend *backend() const noexcept { return backend_.get(); }
    wlr_renderer *renderer() const noexcept { return renderer_.get(); }
    wlr_allocator *allocator() const noexcept { return allocator_.get(); }
    wlr_compositor *compositor() const noexcept { return compositor_; }

  private:
    Server() noexcept = default;
    static int on_shutdown_signal(int signal, void *data) noexcept;
    void remove_shutdown_signal_sources() noexcept;
    void restore_shutdown_signal_mask() noexcept;
    struct DisplayDeleter {
        void operator()(wl_display *) const noexcept;
    };
    struct BackendDeleter {
        void operator()(wlr_backend *) const noexcept;
    };
    struct RendererDeleter {
        void operator()(wlr_renderer *) const noexcept;
    };
    struct AllocatorDeleter {
        void operator()(wlr_allocator *) const noexcept;
    };

    std::unique_ptr<wl_display, DisplayDeleter> display_;
    std::unique_ptr<wlr_backend, BackendDeleter> backend_;
    std::unique_ptr<wlr_renderer, RendererDeleter> renderer_;
    std::unique_ptr<wlr_allocator, AllocatorDeleter> allocator_;
    wlr_compositor *compositor_ = nullptr; // Owned by the display.
    std::string named_socket_;
    const char *socket_name_ = nullptr;
    wl_event_source *terminate_signal_ = nullptr;
    wl_event_source *interrupt_signal_ = nullptr;
    sigset_t previous_signal_mask_{};
    bool owns_signal_mask_ = false;
    bool running_ = false;
    bool shutdown_requested_ = false;
};

} // namespace seaside

#endif

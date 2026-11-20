#ifndef SEASIDE_SERVER_HPP
#define SEASIDE_SERVER_HPP

#include <expected>
#include <memory>

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;

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
    void finish() noexcept;

    wl_display *display() const noexcept { return display_.get(); }
    wlr_backend *backend() const noexcept { return backend_.get(); }
    wlr_renderer *renderer() const noexcept { return renderer_.get(); }
    wlr_allocator *allocator() const noexcept { return allocator_.get(); }
    wlr_compositor *compositor() const noexcept { return compositor_; }

  private:
    Server() noexcept = default;
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
};

} // namespace seaside

#endif

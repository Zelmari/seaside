#include "server.hpp"
#include "wlroots_bridge.h"

#include <new>
#include <pthread.h>
#include <wayland-server-core.h>

namespace seaside {

void Server::DisplayDeleter::operator()(wl_display *display) const noexcept {
    wl_display_destroy(display);
}
void Server::BackendDeleter::operator()(wlr_backend *backend) const noexcept {
    seaside_wlr_backend_destroy(backend);
}
void Server::RendererDeleter::operator()(wlr_renderer *renderer) const noexcept {
    seaside_wlr_renderer_destroy(renderer);
}
void Server::AllocatorDeleter::operator()(wlr_allocator *allocator) const noexcept {
    seaside_wlr_allocator_destroy(allocator);
}

std::expected<std::unique_ptr<Server>, ServerError> Server::create() noexcept {
    // This boundary reports allocation failure without throwing through C callers.
    std::unique_ptr<Server> server(new (std::nothrow) Server);
    if (!server) {
        return std::unexpected(ServerError::allocation);
    }
    server->display_.reset(wl_display_create());
    if (!server->display_) {
        return std::unexpected(ServerError::display_creation);
    }
    return server;
}

std::expected<void, ServerError> Server::start(wlr_backend *backend,
                                               wlr_renderer *renderer) noexcept {
    if (!display_ || backend_ || renderer_) {
        return std::unexpected(ServerError::invalid_state);
    }
    if (!backend || !renderer) {
        return std::unexpected(ServerError::invalid_arguments);
    }
    backend_.reset(backend);
    renderer_.reset(renderer);
    allocator_.reset(seaside_wlr_allocator_create(backend, renderer));
    if (!allocator_) {
        return std::unexpected(ServerError::allocator_creation);
    }
    if (!seaside_wlr_renderer_init_display(renderer, display_.get(), backend, &dmabuf_)) {
        return std::unexpected(ServerError::renderer_initialization);
    }
    compositor_ = seaside_wlr_compositor_create(display_.get(), renderer);
    if (!compositor_) {
        return std::unexpected(ServerError::compositor_creation);
    }
    if (!seaside_wlr_backend_start(backend)) {
        return std::unexpected(ServerError::backend_start);
    }
    return {};
}

Server::~Server() noexcept {
    finish();
}

std::expected<std::string_view, ServerError> Server::listen(std::string_view name) noexcept {
    if (!display_ || socket_name_) {
        return std::unexpected(ServerError::invalid_state);
    }
    if (name.find('\0') != std::string_view::npos) {
        return std::unexpected(ServerError::invalid_arguments);
    }
    if (name.empty()) {
        socket_name_ = wl_display_add_socket_auto(display_.get());
    } else {
        try {
            std::string copied(name);
            if (wl_display_add_socket(display_.get(), copied.c_str()) < 0) {
                return std::unexpected(ServerError::socket_creation);
            }
            named_socket_ = std::move(copied);
            socket_name_ = named_socket_.c_str();
        } catch (const std::bad_alloc &) {
            return std::unexpected(ServerError::allocation);
        }
    }
    if (!socket_name_) {
        return std::unexpected(ServerError::socket_creation);
    }
    return std::string_view(socket_name_);
}

int Server::on_shutdown_signal(int, void *data) noexcept {
    static_cast<Server *>(data)->terminate();
    return 0;
}

std::expected<void, ServerError> Server::install_shutdown_signals() noexcept {
    if (!display_ || owns_signal_mask_) {
        return std::unexpected(ServerError::invalid_state);
    }
    if (pthread_sigmask(SIG_SETMASK, nullptr, &previous_signal_mask_) != 0) {
        return std::unexpected(ServerError::signal_registration);
    }
    owns_signal_mask_ = true;
    auto *loop = wl_display_get_event_loop(display_.get());
    terminate_signal_ = wl_event_loop_add_signal(loop, SIGTERM, on_shutdown_signal, this);
    interrupt_signal_ = wl_event_loop_add_signal(loop, SIGINT, on_shutdown_signal, this);
    if (!terminate_signal_ || !interrupt_signal_) {
        remove_shutdown_signal_sources();
        restore_shutdown_signal_mask();
        return std::unexpected(ServerError::signal_registration);
    }
    return {};
}

void Server::remove_shutdown_signal_sources() noexcept {
    if (terminate_signal_) {
        wl_event_source_remove(terminate_signal_);
        terminate_signal_ = nullptr;
    }
    if (interrupt_signal_) {
        wl_event_source_remove(interrupt_signal_);
        interrupt_signal_ = nullptr;
    }
}

void Server::restore_shutdown_signal_mask() noexcept {
    if (owns_signal_mask_) {
        // Preserve the caller's mask across explicit signal-source ownership;
        // changing unrelated signal bits could interfere with another subsystem.
        for (const int signal : {SIGTERM, SIGINT}) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, signal);
            pthread_sigmask(sigismember(&previous_signal_mask_, signal) ? SIG_BLOCK : SIG_UNBLOCK,
                            &set, nullptr);
        }
        owns_signal_mask_ = false;
    }
}

std::expected<void, ServerError> Server::run() noexcept {
    if (!display_ || !socket_name_ || running_) {
        return std::unexpected(ServerError::invalid_state);
    }
    // Startup may briefly dispatch the loop to acquire an active seat. A
    // shutdown observed there must survive wl_display_run resetting its flag.
    if (shutdown_requested_) {
        return std::unexpected(ServerError::shutdown_requested);
    }
    running_ = true;
    wl_display_run(display_.get());
    running_ = false;
    return {};
}

void Server::terminate() noexcept {
    if (display_) {
        shutdown_requested_ = true;
        wl_display_terminate(display_.get());
    }
}

void Server::finish() noexcept {
    remove_shutdown_signal_sources();
    // Disconnect clients while their surface listeners and renderer still live.
    // Backend teardown releases output swapchains before allocator destruction.
    if (display_) {
        wl_display_destroy_clients(display_.get());
    }
    backend_.reset();
    allocator_.reset();
    renderer_.reset();
    display_.reset();
    // The display loop may still own the console seat. A second signal must
    // remain blocked until loop destruction has released the seat as well.
    restore_shutdown_signal_mask();
    compositor_ = nullptr;
    dmabuf_ = nullptr;
    socket_name_ = nullptr;
    named_socket_.clear();
}

} // namespace seaside

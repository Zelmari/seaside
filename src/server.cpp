#include "server.hpp"
#include "wlroots_bridge.h"

#include <new>
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
    if (!seaside_wlr_renderer_init_display(renderer, display_.get())) {
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

void Server::finish() noexcept {
    // Disconnect clients while their surface listeners and renderer still live.
    // Backend teardown releases output swapchains before allocator destruction.
    if (display_) {
        wl_display_destroy_clients(display_.get());
    }
    backend_.reset();
    allocator_.reset();
    renderer_.reset();
    display_.reset();
    compositor_ = nullptr;
}

} // namespace seaside

#include "server.h"
#include "server.hpp"

namespace {
void borrow_handles(seaside_server &facade, seaside::Server &owner) noexcept {
    facade.display = owner.display();
    facade.backend = owner.backend();
    facade.renderer = owner.renderer();
    facade.allocator = owner.allocator();
    facade.compositor = owner.compositor();
}
} // namespace

extern "C" bool seaside_server_init(seaside_server *server) noexcept {
    if (!server || server->owner || server->display || server->backend || server->renderer ||
        server->allocator || server->compositor) {
        return false;
    }
    auto created = seaside::Server::create();
    if (!created) {
        return false;
    }
    borrow_handles(*server, **created);
    server->owner = created->release();
    return true;
}

extern "C" bool seaside_server_start(seaside_server *server, wlr_backend *backend,
                                     wlr_renderer *renderer) noexcept {
    if (!server || !server->owner) {
        return false;
    }
    auto &owner = *static_cast<seaside::Server *>(server->owner);
    auto result = owner.start(backend, renderer);
    borrow_handles(*server, owner);
    return result.has_value();
}

extern "C" void seaside_server_finish(seaside_server *server) noexcept {
    if (!server) {
        return;
    }
    // The facade owns the pointer released by create; all mirrored handles borrow.
    delete static_cast<seaside::Server *>(server->owner);
    *server = {};
}

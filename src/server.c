#include "server.h"

#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>

bool seaside_server_init(struct seaside_server *server) {
	if (!server || server->display) {
		return false;
	}
	server->display = wl_display_create();
	return server->display != NULL;
}

bool seaside_server_start(struct seaside_server *server,
		struct wlr_backend *backend, struct wlr_renderer *renderer) {
	if (!server || !server->display || server->backend || server->renderer ||
			!backend || !renderer) {
		return false;
	}
	server->backend = backend;
	server->renderer = renderer;
	server->allocator = wlr_allocator_autocreate(backend, renderer);
	if (!server->allocator ||
			!wlr_renderer_init_wl_display(renderer, server->display)) {
		return false;
	}
	server->compositor = wlr_compositor_create(server->display, 6, renderer);
	if (!server->compositor || !wlr_subcompositor_create(server->display)) {
		return false;
	}
	return wlr_backend_start(backend);
}

void seaside_server_finish(struct seaside_server *server) {
	if (!server) {
		return;
	}
	if (server->display) {
		wl_display_destroy_clients(server->display);
	}
	if (server->backend) {
		wlr_backend_destroy(server->backend);
	}
	if (server->allocator) {
		wlr_allocator_destroy(server->allocator);
	}
	if (server->renderer) {
		wlr_renderer_destroy(server->renderer);
	}
	if (server->display) {
		wl_display_destroy(server->display);
	}
	memset(server, 0, sizeof(*server));
}

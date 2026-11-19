#ifndef SEASIDE_SERVER_H
#define SEASIDE_SERVER_H

#include <stdbool.h>

struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_compositor;
struct wlr_renderer;

/* Private event-loop-owned state; backend selection and session policy belong
 * to the caller. This is not a production launch path or a preflight bypass. */
struct seaside_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_compositor *compositor;
};

/* Initialize a zero-initialized server before creating a backend on its loop. */
bool seaside_server_init(struct seaside_server *server);

/* For an initialized, unstarted server, ownership of both non-null arguments
 * transfers even if startup fails. Finish releases partially started state.
 * Invalid preconditions do not transfer ownership. */
bool seaside_server_start(struct seaside_server *server,
	struct wlr_backend *backend, struct wlr_renderer *renderer);

/* Clients must release textures before renderer teardown; outputs must release
 * swapchains before allocator teardown. Safe after partial init and repeatable. */
void seaside_server_finish(struct seaside_server *server);

#endif

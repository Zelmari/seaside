#include "wlroots_fixture.hpp"
#include "wlr_compat.hpp"

#include <wayland-server-core.h>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/render/pixman.h>
}

/* Explicit software/headless selection stays in tests, never the core owner. */
struct wlr_backend *seaside_test_backend_create(struct wl_display *display) {
    return wlr_headless_backend_create(wl_display_get_event_loop(display));
}

struct wlr_renderer *seaside_test_renderer_create(void) {
    return wlr_pixman_renderer_create();
}

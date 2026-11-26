#ifndef SEASIDE_TEST_WLROOTS_FIXTURE_H
#define SEASIDE_TEST_WLROOTS_FIXTURE_H

struct wl_display;
struct wlr_backend;
struct wlr_renderer;

#ifdef __cplusplus
extern "C" {
#endif
struct wlr_backend *seaside_test_backend_create(struct wl_display *display);
struct wlr_renderer *seaside_test_renderer_create(void);
#ifdef __cplusplus
}
#endif

#endif

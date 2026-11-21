#ifndef SEASIDE_WINDOW_CAPTURE_H
#define SEASIDE_WINDOW_CAPTURE_H

#include <stdbool.h>
#include <sys/types.h>

struct wl_display;
struct wlr_allocator;
struct wlr_renderer;
struct wlr_xdg_toplevel;
struct seaside_window_capture;
struct seaside_window_source;

/* All calls belong to the display thread. Destroy sources before the manager
 * and tear the manager down before the borrowed display and renderer. */
struct seaside_window_capture *seaside_window_capture_create(struct wl_display *display,
                                                             struct wlr_allocator *allocator,
                                                             struct wlr_renderer *renderer);
void seaside_window_capture_destroy(struct seaside_window_capture *capture);
struct seaside_window_source *seaside_window_capture_map(struct seaside_window_capture *capture,
                                                         struct wlr_xdg_toplevel *toplevel,
                                                         bool presented);
void seaside_window_capture_unmap(struct seaside_window_source *source);
void seaside_window_capture_presented(struct seaside_window_source *source, bool presented);
bool seaside_window_capture_client_ready(const struct seaside_window_capture *capture, pid_t pid,
                                         uid_t uid);

#endif

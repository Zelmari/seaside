#ifndef SEASIDE_OUTPUT_CAPTURE_H
#define SEASIDE_OUTPUT_CAPTURE_H

#include <stdbool.h>
#include <sys/types.h>

struct wl_display;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_output;
struct seaside_output_capture;

#ifdef __cplusplus
extern "C" {
#endif

/* Install display-wide output image-source and image-copy globals. All calls
 * belong to the display event-loop thread. */
struct seaside_output_capture *seaside_output_capture_create(struct wl_display *display);
void seaside_output_capture_destroy(struct seaside_output_capture *capture);
/* The shared image-copy manager is owned by the display. */
struct wlr_ext_image_copy_capture_manager_v1 *
seaside_output_capture_copy_manager(const struct seaside_output_capture *capture);

/* Removing an output stops only its capture sessions. The global bindings
 * survive VT switches and output hotplug. */
bool seaside_output_capture_add_output(struct seaside_output_capture *capture,
                                       struct wlr_output *output, int x, int y);
bool seaside_output_capture_position(struct seaside_output_capture *capture,
                                     struct wlr_output *output, int x, int y);
void seaside_output_capture_remove_output(struct seaside_output_capture *capture,
                                          struct wlr_output *output);
/* A hardware cursor update may leave the underlying scene undamaged. */
void seaside_output_capture_cursor_changed(struct seaside_output_capture *capture);

/* Require one live client with the peer credentials and the relevant bound
 * resources on this display. This is a portal readiness check. */
bool seaside_output_capture_client_ready(const struct seaside_output_capture *capture, pid_t pid,
                                         uid_t uid);

#ifdef __cplusplus
}
#endif

#endif

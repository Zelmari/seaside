#ifndef SEASIDE_OUTPUT_CAPTURE_H
#define SEASIDE_OUTPUT_CAPTURE_H

#include <stdbool.h>
#include <sys/types.h>

struct wl_display;
struct wlr_output;
struct seaside_output_capture;

#ifdef __cplusplus
extern "C" {
#endif

/* Install output image-source and image-copy globals for an enabled output.
 * All calls belong to the display event-loop thread. */
struct seaside_output_capture *seaside_output_capture_create(struct wl_display *display,
                                                             struct wlr_output *output);
void seaside_output_capture_destroy(struct seaside_output_capture *capture);

/* The globals survive destruction of an output during a VT switch. Attach the
 * newly enabled output before accepting new capture sessions. */
bool seaside_output_capture_set_output(struct seaside_output_capture *capture,
                                       struct wlr_output *output);
bool seaside_output_capture_position(struct seaside_output_capture *capture, int x, int y);
void seaside_output_capture_clear_output(struct seaside_output_capture *capture);

/* Require one live client with the peer credentials and the relevant bound
 * resources on this display. This is a portal readiness check. */
bool seaside_output_capture_client_ready(const struct seaside_output_capture *capture, pid_t pid,
                                         uid_t uid);

#ifdef __cplusplus
}
#endif

#endif

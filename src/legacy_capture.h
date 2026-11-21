#ifndef SEASIDE_LEGACY_CAPTURE_H
#define SEASIDE_LEGACY_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct seaside_legacy_capture;
struct wl_display;
struct wlr_buffer;
struct wlr_output;

typedef bool (*seaside_legacy_output_fn)(void *data, struct wlr_output *output);
typedef void (*seaside_legacy_active_fn)(void *data, struct wlr_output *output, bool active);

/* The caller owns output rendering and cursor composition. This adapter only
 * holds resources for outstanding legacy screencopy requests. */
struct seaside_legacy_capture *seaside_legacy_capture_create(struct wl_display *display,
                                                             seaside_legacy_output_fn supports,
                                                             seaside_legacy_active_fn active,
                                                             void *data);
void seaside_legacy_capture_destroy(struct seaside_legacy_capture *capture);
void seaside_legacy_capture_output_lost(struct seaside_legacy_capture *capture,
                                        struct wlr_output *output);
bool seaside_legacy_capture_wants_cursor(const struct seaside_legacy_capture *capture,
                                         struct wlr_output *output);

/* Send a presented frame to waiting clients. cursor_buffer may reuse buffer
 * when the cursor is hidden, or be null if offscreen composition failed.
 * content_revision advances when the output's scene changes. */
void seaside_legacy_capture_present(struct seaside_legacy_capture *capture,
                                    struct wlr_output *output, struct wlr_buffer *buffer,
                                    struct wlr_buffer *cursor_buffer, const struct timespec *when,
                                    uint64_t content_revision);

#endif

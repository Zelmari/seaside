#ifndef SEASIDE_TEXT_INPUT_H
#define SEASIDE_TEXT_INPUT_H

#include "tty_input.hpp"

struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct seaside_text_input;

#ifdef __cplusplus
extern "C" {
#endif

struct seaside_text_input *
seaside_text_input_create(struct wl_display *display, struct wlr_seat *seat,
                          struct wlr_keyboard *keyboard,
                          const struct seaside_tty_input_callbacks *callbacks, void *data);
void seaside_text_input_destroy(struct seaside_text_input *relay);
void seaside_text_input_restore_keyboard(struct seaside_text_input *relay);
/* These run after compositor shortcuts have consumed their keys. */
bool seaside_text_input_key(struct seaside_text_input *relay, struct wlr_keyboard *keyboard,
                            const struct wlr_keyboard_key_event *event);
bool seaside_text_input_modifiers(struct seaside_text_input *relay, struct wlr_keyboard *keyboard);

#ifdef __cplusplus
}
#endif

#endif

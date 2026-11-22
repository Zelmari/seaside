#ifndef SEASIDE_IDLE_INHIBIT_H
#define SEASIDE_IDLE_INHIBIT_H

struct wl_display;
struct wlr_surface;
struct seaside_idle_inhibit;

#ifdef __cplusplus
extern "C" {
#endif

struct seaside_idle_inhibit *seaside_idle_inhibit_create(struct wl_display *display,
                                                         void (*changed)(void *), void *data);
void seaside_idle_inhibit_destroy(struct seaside_idle_inhibit *inhibit);
void seaside_idle_inhibit_each(const struct seaside_idle_inhibit *inhibit,
                               void (*visit)(void *, struct wlr_surface *), void *data);

#ifdef __cplusplus
}
#endif

#endif

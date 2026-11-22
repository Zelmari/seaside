#ifndef SEASIDE_CAPTURE_FENCE_FIXTURE_H
#define SEASIDE_CAPTURE_FENCE_FIXTURE_H

#include <stdbool.h>

struct wlr_renderer;
void capture_test_enable_fences(struct wlr_renderer *renderer);
bool capture_test_fence_pending(void);
void capture_test_complete_fence(void);
void capture_test_disable_fences(struct wlr_renderer *renderer);

#endif

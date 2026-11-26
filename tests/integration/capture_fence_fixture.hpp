#ifndef SEASIDE_CAPTURE_FENCE_FIXTURE_HPP
#define SEASIDE_CAPTURE_FENCE_FIXTURE_HPP

struct wlr_renderer;

#ifdef __cplusplus
extern "C" {
#endif

void capture_test_enable_fences(struct wlr_renderer *renderer);
bool capture_test_fence_pending(void);
void capture_test_complete_fence(void);
void capture_test_disable_fences(struct wlr_renderer *renderer);

#ifdef __cplusplus
}
#endif

#endif

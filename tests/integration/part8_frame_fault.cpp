#include "part3_bridge.hpp"
#include "wlr_compat.hpp"

extern "C" {
#include <wlr/types/wlr_output.h>
}

static struct wlr_output *reject_next;

extern "C" bool __real_wlr_output_commit_state(struct wlr_output *output,
                                               const struct wlr_output_state *state);

extern "C" bool __wrap_wlr_output_commit_state(struct wlr_output *output,
                                               const struct wlr_output_state *state) {
    if (output == reject_next && (state->committed & WLR_OUTPUT_STATE_BUFFER)) {
        reject_next = nullptr;
        return false;
    }
    return __real_wlr_output_commit_state(output, state);
}

void part8_reject_next_frame(struct wlr_output *output) {
    reject_next = output;
    wlr_output_schedule_frame(output);
}

#include "tty_drm.h"

#include <stdio.h>
#include <wlr/types/wlr_output.h>

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "DRM mode fixture at line %d: %s\n", __LINE__, #condition);            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    struct wlr_output output = {.adaptive_sync_supported = true};
    wl_list_init(&output.modes);
    struct seaside_drm_output_settings automatic = {0};
    CHECK(!seaside_tty_drm_select_mode(&output, &automatic));

    struct wlr_output_mode modes[] = {
        {.width = 1280, .height = 720, .refresh = 240000},
        {.width = 1920, .height = 1080, .refresh = 60000, .preferred = true},
        {.width = 1920, .height = 1080, .refresh = 143856},
        {.width = 2560, .height = 1440, .refresh = 120000},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        wl_list_insert(output.modes.prev, &modes[i].link);
    }

    const struct seaside_drm_output_description description =
        seaside_tty_drm_describe_output(&output);
    CHECK(description.preferred_width == 1920 && description.preferred_height == 1080);
    CHECK(description.max_refresh_millihz == 240000);
    CHECK(description.max_pixels == 2560 * 1440);
    CHECK(description.adaptive_sync_supported);

    CHECK(seaside_tty_drm_select_mode(&output, &automatic));
    CHECK(automatic.mode == &modes[2]);
    CHECK(automatic.width == 1920 && automatic.height == 1080);
    CHECK(automatic.refresh_millihz == 143856);

    struct seaside_drm_output_settings resolution = {.width = 2560, .height = 1440};
    CHECK(seaside_tty_drm_select_mode(&output, &resolution));
    CHECK(resolution.mode == &modes[3]);
    CHECK(resolution.refresh_millihz == 120000);

    struct seaside_drm_output_settings refresh = {.refresh_millihz = 60000};
    CHECK(seaside_tty_drm_select_mode(&output, &refresh));
    CHECK(refresh.mode == &modes[1]);

    struct seaside_drm_output_settings unsupported = {.refresh_millihz = 240000};
    CHECK(!seaside_tty_drm_select_mode(&output, &unsupported));
    CHECK(!unsupported.mode);
    return 0;
}

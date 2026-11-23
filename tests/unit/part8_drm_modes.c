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

    modes[1].refresh = 59951;
    modes[2].refresh = 143998;
    modes[3].refresh = 164999;
    const int nominal[] = {60000, 144000, 165000};
    int matched = 0;
    for (size_t i = 0; i < 3; ++i) {
        struct seaside_drm_output_settings request = {.width = modes[i + 1].width,
                                                      .height = modes[i + 1].height,
                                                      .refresh_millihz = nominal[i]};
        if (seaside_tty_drm_select_mode(&output, &request) && request.mode == &modes[i + 1] &&
            request.refresh_millihz == modes[i + 1].refresh) {
            ++matched;
        } else {
            fprintf(stderr, "Nominal %d mHz did not select advertised %d mHz\n", nominal[i],
                    modes[i + 1].refresh);
        }
    }
    CHECK(matched == 3);
    modes[2].refresh = 180003;
    struct seaside_drm_output_settings nominal_180 = {.refresh_millihz = 180000};
    automatic.refresh_millihz = 0;
    CHECK(seaside_tty_drm_select_mode(&output, &nominal_180));
    CHECK(seaside_tty_drm_select_mode(&output, &automatic));
    CHECK(nominal_180.mode == &modes[2] && nominal_180.mode == automatic.mode);
    CHECK(nominal_180.refresh_millihz == 180003);
    modes[2].refresh = 60000;
    refresh.refresh_millihz = 60000;
    CHECK(seaside_tty_drm_select_mode(&output, &refresh) && refresh.mode == &modes[2]);
    refresh.refresh_millihz = 59951;
    CHECK(seaside_tty_drm_select_mode(&output, &refresh) && refresh.mode == &modes[1]);
    refresh.refresh_millihz = 59950;
    CHECK(seaside_tty_drm_select_mode(&output, &refresh) && refresh.mode == &modes[1]);
    refresh.refresh_millihz = 61000;
    CHECK(!seaside_tty_drm_select_mode(&output, &refresh));

    struct seaside_drm_output_settings unsupported = {.refresh_millihz = 240000};
    CHECK(!seaside_tty_drm_select_mode(&output, &unsupported));
    CHECK(!unsupported.mode);
    return 0;
}

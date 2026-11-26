#include "tty_drm.hpp"
#include "wlr_compat.hpp"

#include <stdio.h>

extern "C" {
#include <wlr/types/wlr_output.h>
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "DRM mode fixture at line %d: %s\n", __LINE__, #condition);            \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void) {
    wlr_output output{};
    output.adaptive_sync_supported = true;
    wl_list_init(&output.modes);
    seaside::DrmOutputSettings automatic = {};
    CHECK(!seaside::select_drm_mode(output, automatic));

    struct wlr_output_mode modes[] = {
        {.width = 1280,
         .height = 720,
         .refresh = 240000,
         .preferred = false,
         .picture_aspect_ratio = {},
         .link = {}},
        {.width = 1920,
         .height = 1080,
         .refresh = 60000,
         .preferred = true,
         .picture_aspect_ratio = {},
         .link = {}},
        {.width = 1920,
         .height = 1080,
         .refresh = 143856,
         .preferred = false,
         .picture_aspect_ratio = {},
         .link = {}},
        {.width = 2560,
         .height = 1440,
         .refresh = 120000,
         .preferred = false,
         .picture_aspect_ratio = {},
         .link = {}},
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(*modes); ++i) {
        wl_list_insert(output.modes.prev, &modes[i].link);
    }

    const auto description = seaside::describe_drm_output(output);
    CHECK(description.preferred_width == 1920 && description.preferred_height == 1080);
    CHECK(description.max_refresh_millihz == 240000);
    CHECK(description.max_pixels == 2560 * 1440);
    CHECK(description.adaptive_sync_supported);

    CHECK(seaside::select_drm_mode(output, automatic));
    CHECK(automatic.mode == &modes[2]);
    CHECK(automatic.width == 1920 && automatic.height == 1080);
    CHECK(automatic.refresh_millihz == 143856);

    seaside::DrmOutputSettings resolution{};
    resolution.width = 2560;
    resolution.height = 1440;
    CHECK(seaside::select_drm_mode(output, resolution));
    CHECK(resolution.mode == &modes[3]);
    CHECK(resolution.refresh_millihz == 120000);

    seaside::DrmOutputSettings refresh{};
    refresh.refresh_millihz = 60000;
    CHECK(seaside::select_drm_mode(output, refresh));
    CHECK(refresh.mode == &modes[1]);

    modes[1].refresh = 59951;
    modes[2].refresh = 143998;
    modes[3].refresh = 164999;
    const int nominal[] = {60000, 144000, 165000};
    int matched = 0;
    for (size_t i = 0; i < 3; ++i) {
        seaside::DrmOutputSettings request{};
        request.width = modes[i + 1].width;
        request.height = modes[i + 1].height;
        request.refresh_millihz = nominal[i];
        if (seaside::select_drm_mode(output, request) && request.mode == &modes[i + 1] &&
            request.refresh_millihz == modes[i + 1].refresh) {
            ++matched;
        } else {
            fprintf(stderr, "Nominal %d mHz did not select advertised %d mHz\n", nominal[i],
                    modes[i + 1].refresh);
        }
    }
    CHECK(matched == 3);
    modes[2].refresh = 180003;
    seaside::DrmOutputSettings nominal_180{};
    nominal_180.refresh_millihz = 180000;
    automatic.refresh_millihz = 0;
    CHECK(seaside::select_drm_mode(output, nominal_180));
    CHECK(seaside::select_drm_mode(output, automatic));
    CHECK(nominal_180.mode == &modes[2] && nominal_180.mode == automatic.mode);
    CHECK(nominal_180.refresh_millihz == 180003);
    modes[2].refresh = 60000;
    refresh.refresh_millihz = 60000;
    CHECK(seaside::select_drm_mode(output, refresh) && refresh.mode == &modes[2]);
    refresh.refresh_millihz = 59951;
    CHECK(seaside::select_drm_mode(output, refresh) && refresh.mode == &modes[1]);
    refresh.refresh_millihz = 59950;
    CHECK(seaside::select_drm_mode(output, refresh) && refresh.mode == &modes[1]);
    refresh.refresh_millihz = 61000;
    CHECK(!seaside::select_drm_mode(output, refresh));

    seaside::DrmOutputSettings unsupported{};
    unsupported.refresh_millihz = 240000;
    CHECK(!seaside::select_drm_mode(output, unsupported));
    CHECK(!unsupported.mode);
    return 0;
}

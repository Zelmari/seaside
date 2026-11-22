#ifndef SEASIDE_CURSOR_LAYOUT_H
#define SEASIDE_CURSOR_LAYOUT_H

#include <stdbool.h>

struct seaside_cursor_box {
    int x, y, width, height;
};

struct seaside_cursor_point {
    double x, y;
};

struct seaside_cursor_gap {
    struct seaside_cursor_point point;
    double crossing, gap;
    bool found;
};

#ifdef __cplusplus
extern "C" {
#endif

bool seaside_cursor_contains(struct seaside_cursor_box box, struct seaside_cursor_point point);
struct seaside_cursor_point seaside_cursor_clamp(struct seaside_cursor_box box,
                                                 struct seaside_cursor_point point);

/* Consider one destination for relative motion beyond an output edge. The
 * caller preserves any target already inside an output before using the
 * accumulated gap result. Geometry uses logical pixels. */
void seaside_cursor_consider_gap(struct seaside_cursor_box current,
                                 struct seaside_cursor_box destination,
                                 struct seaside_cursor_point origin,
                                 struct seaside_cursor_point requested,
                                 struct seaside_cursor_gap *best);

#ifdef __cplusplus
}
#endif

#endif

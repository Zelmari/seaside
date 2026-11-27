#ifndef SEASIDE_CURSOR_LAYOUT_H
#define SEASIDE_CURSOR_LAYOUT_H

#include <stdbool.h>

struct seaside_cursor_box {
    int x, y, width, height;
};

struct seaside_cursor_point {
    double x, y;
};

struct seaside_pointer_content {
    struct seaside_cursor_box bounds;
    double layout_per_surface_x, layout_per_surface_y;
    bool toplevel, fullscreen;
};

enum seaside_pointer_motion {
    SEASIDE_POINTER_RELATIVE,
    SEASIDE_POINTER_ABSOLUTE,
    SEASIDE_POINTER_CONFINED,
};

bool seaside_cursor_contains(struct seaside_cursor_box box, struct seaside_cursor_point point);
struct seaside_cursor_point seaside_cursor_clamp(struct seaside_cursor_box box,
                                                 struct seaside_cursor_point point);

/* Map the full logical height of a side-by-side pair. A NULL destination
 * represents one active output. All coordinates use logical pixels. */
struct seaside_cursor_point
seaside_cursor_route_relative(struct seaside_cursor_box current, struct seaside_cursor_point origin,
                              struct seaside_cursor_point requested,
                              const struct seaside_cursor_box *destination);

#endif

#include "cursor_layout.h"

#include <math.h>

bool seaside_cursor_contains(struct seaside_cursor_box box, struct seaside_cursor_point point) {
    return point.x >= box.x && point.x < (double)box.x + box.width && point.y >= box.y &&
           point.y < (double)box.y + box.height;
}

struct seaside_cursor_point seaside_cursor_clamp(struct seaside_cursor_box box,
                                                 struct seaside_cursor_point point) {
    // Clamp to a half-open extent while keeping subpixel motion at its edge.
    const double right = (double)box.x + box.width - 0.000001;
    const double bottom = (double)box.y + box.height - 0.000001;
    return (struct seaside_cursor_point){fmax(box.x, fmin(right, point.x)),
                                         fmax(box.y, fmin(bottom, point.y))};
}

static void consider_axis(struct seaside_cursor_box current, struct seaside_cursor_box destination,
                          struct seaside_cursor_point origin, struct seaside_cursor_point requested,
                          bool horizontal, struct seaside_cursor_gap *best) {
    const double along = horizontal ? requested.x : requested.y;
    const double near = horizontal ? current.x : current.y;
    const double far = near + (horizontal ? current.width : current.height);
    const double next_near = horizontal ? destination.x : destination.y;
    const double next_far = next_near + (horizontal ? destination.width : destination.height);
    const bool forward = along >= far;
    if (forward ? next_near < far || along >= next_far
                : along >= near || next_far > near || along < next_near) {
        return;
    }
    const double edge = forward ? far : near;
    const double gap = forward ? next_near - edge : edge - next_far;
    const double start = horizontal ? origin.x : origin.y;
    const double crossing = (edge - start) / (along - start);
    struct seaside_cursor_point point = requested;
    double *coordinate = horizontal ? &point.x : &point.y;
    if (forward ? along < next_near : along >= next_far) {
        // Carry the residual delta across the gap so small mouse events reach
        // an adjacent output regardless of how far apart the boxes are.
        *coordinate += forward ? gap : -gap;
    }
    point = seaside_cursor_clamp(destination, point);
    const double offset = horizontal ? fabs(point.y - requested.y) : fabs(point.x - requested.x);
    // Prefer an aligned edge. On diagonals, use the first such edge crossed,
    // and choose the closest edge when multiple outputs are equally aligned.
    if (!best->found || offset < best->offset ||
        (offset == best->offset &&
         (crossing < best->crossing || (crossing == best->crossing && gap < best->gap)))) {
        *best = (struct seaside_cursor_gap){point, offset, crossing, gap, true};
    }
}

void seaside_cursor_consider_gap(struct seaside_cursor_box current,
                                 struct seaside_cursor_box destination,
                                 struct seaside_cursor_point origin,
                                 struct seaside_cursor_point requested,
                                 struct seaside_cursor_gap *best) {
    consider_axis(current, destination, origin, requested, true, best);
    consider_axis(current, destination, origin, requested, false, best);
}

#include "cursor_layout.hpp"

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
    return seaside_cursor_point{fmax(box.x, fmin(right, point.x)),
                                fmax(box.y, fmin(bottom, point.y))};
}

static bool map_side(struct seaside_cursor_box current, struct seaside_cursor_box destination,
                     struct seaside_cursor_point origin, struct seaside_cursor_point requested,
                     struct seaside_cursor_point *mapped) {
    const double source_right = (double)current.x + current.width;
    const double source_bottom = (double)current.y + current.height;
    const double destination_right = (double)destination.x + destination.width;
    const double destination_bottom = (double)destination.y + destination.height;
    const bool right = requested.x >= source_right && destination.x >= source_right;
    const bool left = requested.x < current.x && destination_right <= current.x;
    if ((!left && !right) || current.y >= destination_bottom || destination.y >= source_bottom ||
        requested.x == origin.x) {
        return false;
    }

    const double source_edge = right ? source_right : current.x;
    const double destination_edge = right ? destination.x : destination_right;
    const double crossing = (source_edge - origin.x) / (requested.x - origin.x);
    const double seam_y = origin.y + crossing * (requested.y - origin.y);
    if (crossing < 0 || crossing > 1 || seam_y < current.y || seam_y > source_bottom) {
        return false;
    }

    const double y_fraction = (seam_y - current.y) / current.height;
    const struct seaside_cursor_point target = {
        .x = destination_edge + (requested.x - source_edge),
        .y = destination.y + y_fraction * destination.height + (requested.y - seam_y),
    };
    *mapped = seaside_cursor_clamp(destination, target);
    return true;
}

struct cursor_gap {
    struct seaside_cursor_point point;
    double crossing, gap;
    bool found;
};

static void consider_axis(struct seaside_cursor_box current, struct seaside_cursor_box destination,
                          struct seaside_cursor_point origin, struct seaside_cursor_point requested,
                          bool horizontal, struct cursor_gap *best) {
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
    const double across = horizontal ? point.y : point.x;
    const double across_min = horizontal ? destination.y : destination.x;
    if (across < across_min ||
        across >= across_min + (horizontal ? destination.height : destination.width)) {
        return;
    }
    // Cross an aligned edge without shifting the pointer along the other axis.
    if (!best->found || crossing < best->crossing ||
        (crossing == best->crossing && gap < best->gap)) {
        *best = cursor_gap{seaside_cursor_clamp(destination, point), crossing, gap, true};
    }
}

struct seaside_cursor_point
seaside_cursor_route_relative(struct seaside_cursor_box current, struct seaside_cursor_point origin,
                              struct seaside_cursor_point requested,
                              const struct seaside_cursor_box *destination) {
    if (!destination || seaside_cursor_contains(current, requested)) {
        return seaside_cursor_clamp(current, requested);
    }
    struct seaside_cursor_point mapped;
    if (map_side(current, *destination, origin, requested, &mapped)) {
        return mapped;
    }
    if (seaside_cursor_contains(*destination, requested)) {
        return requested;
    }
    cursor_gap gap{};
    consider_axis(current, *destination, origin, requested, true, &gap);
    consider_axis(current, *destination, origin, requested, false, &gap);
    return gap.found ? gap.point : seaside_cursor_clamp(current, requested);
}

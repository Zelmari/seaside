#include "cursor_layout.h"

#include <gtest/gtest.h>
#include <initializer_list>
#include <limits>

namespace {

seaside_cursor_point move(seaside_cursor_box current, seaside_cursor_point origin,
                          seaside_cursor_point requested,
                          std::initializer_list<seaside_cursor_box> destinations) {
    if (seaside_cursor_contains(current, requested)) {
        return requested;
    }
    seaside_cursor_gap best{};
    for (const auto destination : destinations) {
        if (seaside_cursor_contains(destination, requested)) {
            return requested;
        }
        seaside_cursor_consider_gap(current, destination, origin, requested, &best);
    }
    return best.found ? best.point : seaside_cursor_clamp(current, requested);
}

TEST(CursorLayout, PreservesDensePlacementAndRelativeOvershoot) {
    constexpr seaside_cursor_box left{0, 0, 160, 90};
    constexpr seaside_cursor_box adjacent{160, 0, 80, 60};
    const auto dense = move(left, {158, 10}, {163, 12}, {adjacent});
    EXPECT_EQ(dense.x, 163);
    EXPECT_EQ(dense.y, 12);
    const auto shorter = move(left, {158, 80}, {163, 80}, {adjacent});
    EXPECT_FALSE(seaside_cursor_contains(adjacent, shorter));
    EXPECT_TRUE(seaside_cursor_contains(left, shorter));
    EXPECT_EQ(shorter.y, 80);
    const auto right_edge = move(adjacent, {180, 59}, {180, 63}, {left});
    EXPECT_FALSE(seaside_cursor_contains(left, right_edge));
    EXPECT_EQ(right_edge.x, 180);

    constexpr seaside_cursor_box right{400, 0, 160, 90};
    const auto forwards = move(left, {158, 35}, {161, 35.5}, {right});
    EXPECT_EQ(forwards.x, 401);
    EXPECT_EQ(forwards.y, 35.5);
    const auto backwards = move(right, {401, 20}, {399, 20}, {left});
    EXPECT_EQ(backwards.x, 159);
    EXPECT_EQ(backwards.y, 20);
}

TEST(CursorLayout, CrossesVerticalGapsInBothDirections) {
    constexpr seaside_cursor_box top{-100, -300, 160, 90};
    constexpr seaside_cursor_box bottom{-100, 0, 160, 90};
    const auto down = move(top, {-25, -212}, {-26.5, -208}, {bottom});
    EXPECT_EQ(down.x, -26.5);
    EXPECT_EQ(down.y, 2);
    const auto up = move(bottom, {-25, 1}, {-24, -2}, {top});
    EXPECT_EQ(up.x, -24);
    EXPECT_EQ(up.y, -212);
}

TEST(CursorLayout, SelectsTheAlignedNeighborOnAnIrregularEdge) {
    constexpr seaside_cursor_box origin{0, 0, 160, 100};
    constexpr seaside_cursor_box top{210, 0, 160, 40};
    constexpr seaside_cursor_box bottom{260, 60, 160, 40};
    const auto movement = move(origin, {159, 74}, {162, 76}, {top, bottom});
    EXPECT_EQ(movement.x, 262);
    EXPECT_EQ(movement.y, 76);
    const auto reverse_order = move(origin, {159, 74}, {162, 76}, {bottom, top});
    EXPECT_EQ(reverse_order.x, movement.x);
    EXPECT_EQ(reverse_order.y, movement.y);
}

TEST(CursorLayout, ExtremeCoordinateDifferencesStayRepresentable) {
    constexpr auto minimum = std::numeric_limits<int>::min();
    constexpr auto maximum = std::numeric_limits<int>::max();
    constexpr seaside_cursor_box left{minimum, minimum, 160, 90};
    constexpr seaside_cursor_box right{maximum - 160, minimum, 160, 90};
    constexpr double y = static_cast<double>(minimum) + 40;
    const auto east = move(left, {minimum + 159, y}, {minimum + 161, y}, {right});
    EXPECT_EQ(east.x, maximum - 159);
    const auto west = move(right, {maximum - 160, y}, {maximum - 161, y}, {left});
    EXPECT_EQ(west.x, minimum + 159);
    EXPECT_EQ(west.y, y);
    EXPECT_TRUE(seaside_cursor_contains(left, west));
}

} // namespace

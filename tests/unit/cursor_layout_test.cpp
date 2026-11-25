#include "cursor_layout.h"

#include <gtest/gtest.h>
#include <limits>

namespace {

seaside_cursor_point move(seaside_cursor_box current, seaside_cursor_point origin,
                          seaside_cursor_point requested, const seaside_cursor_box &destination) {
    return seaside_cursor_route_relative(current, origin, requested, &destination);
}

TEST(CursorLayout, MapsTheWholeSideEdgeInBothDirections) {
    constexpr seaside_cursor_box left{0, 0, 160, 90};
    constexpr seaside_cursor_box adjacent{160, 0, 80, 45};
    const auto dense = move(left, {158, 10}, {163, 12}, adjacent);
    EXPECT_EQ(dense.x, 163);
    EXPECT_DOUBLE_EQ(dense.y, 6.6);
    for (const double y : {0.0, 45.0, 88.0}) {
        const auto east = move(left, {158, y}, {160, y}, adjacent);
        EXPECT_EQ(east.x, 160);
        EXPECT_DOUBLE_EQ(east.y, y / 2);
        const auto west = move(adjacent, east, {158, east.y}, left);
        EXPECT_EQ(west.x, 158);
        EXPECT_DOUBLE_EQ(west.y, y);
    }
    const auto bottom_edge = move(adjacent, {180, 44}, {180, 48}, left);
    EXPECT_TRUE(seaside_cursor_contains(adjacent, bottom_edge));
    EXPECT_EQ(bottom_edge.x, 180);
}

TEST(CursorLayout, PreservesMotionOvershootAcrossSideBySideGaps) {
    constexpr seaside_cursor_box left{0, 0, 160, 90};
    constexpr seaside_cursor_box right{400, 0, 160, 90};
    const auto forwards = move(left, {158, 35}, {161, 35.5}, right);
    EXPECT_EQ(forwards.x, 401);
    EXPECT_EQ(forwards.y, 35.5);
    const auto backwards = move(right, {401, 20}, {399, 20}, left);
    EXPECT_EQ(backwards.x, 159);
    EXPECT_EQ(backwards.y, 20);
}

TEST(CursorLayout, CrossesVerticalGapsInBothDirections) {
    constexpr seaside_cursor_box top{-100, -300, 160, 90};
    constexpr seaside_cursor_box bottom{-100, 0, 160, 90};
    const auto down = move(top, {-25, -212}, {-26.5, -208}, bottom);
    EXPECT_EQ(down.x, -26.5);
    EXPECT_EQ(down.y, 2);
    const auto up = move(bottom, {-25, 1}, {-24, -2}, top);
    EXPECT_EQ(up.x, -24);
    EXPECT_EQ(up.y, -212);
}

TEST(CursorLayout, MapsLogicalHeightAcrossOffsetOutputs) {
    constexpr seaside_cursor_box left{-160, -50, 160, 100};
    constexpr seaside_cursor_box right{0, 25, 80, 200};
    const auto east = move(left, {-1, -25}, {4, -25}, right);
    EXPECT_EQ(east.x, 4);
    EXPECT_EQ(east.y, 75);
    const auto west = move(right, east, {-1, east.y}, left);
    EXPECT_EQ(west.x, -1);
    EXPECT_EQ(west.y, -25);
}

TEST(CursorLayout, DoesNotUseTheSideWhenTheMotionFirstExitsTheTop) {
    constexpr seaside_cursor_box left{0, 0, 160, 100};
    constexpr seaside_cursor_box right{160, 0, 100, 200};
    const auto movement = move(left, {157, 1}, {161, -9}, right);
    EXPECT_TRUE(seaside_cursor_contains(left, movement));
    EXPECT_EQ(movement.y, 0);
}

TEST(CursorLayout, ClampsRelativeMovementToASingleOutput) {
    constexpr seaside_cursor_box output{-70, 0, 90, 160};
    const auto movement = seaside_cursor_route_relative(output, {12, 50}, {28, 42}, nullptr);
    EXPECT_TRUE(seaside_cursor_contains(output, movement));
    EXPECT_NEAR(movement.x, 20, 0.00001);
    EXPECT_EQ(movement.y, 42);
}

TEST(CursorLayout, ExtremeCoordinateDifferencesStayRepresentable) {
    constexpr auto minimum = std::numeric_limits<int>::min();
    constexpr auto maximum = std::numeric_limits<int>::max();
    constexpr seaside_cursor_box left{minimum, minimum, 160, 90};
    constexpr seaside_cursor_box right{maximum - 160, minimum, 160, 90};
    constexpr double y = static_cast<double>(minimum) + 40;
    const auto east = move(left, {minimum + 159, y}, {minimum + 161, y}, right);
    EXPECT_EQ(east.x, maximum - 159);
    const auto west = move(right, {maximum - 160, y}, {maximum - 161, y}, left);
    EXPECT_EQ(west.x, minimum + 159);
    EXPECT_EQ(west.y, y);
    EXPECT_TRUE(seaside_cursor_contains(left, west));
}

} // namespace

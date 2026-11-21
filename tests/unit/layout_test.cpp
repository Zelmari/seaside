#include "layout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <random>

namespace seaside {
namespace {

LayoutSettings no_gaps(Layout kind = Layout::dwindle) {
    return {.kind = kind, .outer_gap = 0, .inner_gap = 0, .border_width = 0};
}

TEST(DwindleLayout, UsesLogicalGapsAndBorderInsideTheWorkArea) {
    WorkspaceLayout layout({.outer_gap = 4, .inner_gap = 6, .border_width = 2});
    layout.set_area({20, -30, 160, 90});
    ASSERT_TRUE(layout.add({1, 1}));
    EXPECT_EQ(layout.geometry(1), (WindowRect{26, -24, 148, 78}));
    ASSERT_TRUE(layout.add({2, 2}, 1));
    EXPECT_EQ(layout.geometry(1), (WindowRect{26, -24, 69, 78}));
    EXPECT_EQ(layout.geometry(2), (WindowRect{105, -24, 69, 78}));
}

TEST(DwindleLayout, SplitsTheFocusedRegionAndPromotesTheSiblingSubtree) {
    WorkspaceLayout layout(no_gaps());
    layout.set_area({0, 0, 1000, 700});
    ASSERT_TRUE(layout.add({1, 1}));
    ASSERT_TRUE(layout.add({2, 2}, 1));
    ASSERT_TRUE(layout.add({3, 3}, 2));
    ASSERT_TRUE(layout.add({4, 4}, 3));
    EXPECT_EQ(layout.geometry(2), (WindowRect{500, 0, 500, 350}));
    EXPECT_EQ(layout.geometry(4), (WindowRect{750, 350, 250, 350}));
    EXPECT_EQ(layout.close_focus(2), 3U);

    ASSERT_TRUE(layout.erase(2));
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 500, 700}));
    EXPECT_EQ(layout.geometry(3), (WindowRect{500, 0, 500, 350}));
    EXPECT_EQ(layout.geometry(4), (WindowRect{500, 350, 500, 350}));
    layout.set_area({0, 0, 500, 900});
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 500, 450}));
    EXPECT_EQ(layout.geometry(3), (WindowRect{0, 450, 250, 450}));
    EXPECT_EQ(layout.geometry(4), (WindowRect{250, 450, 250, 450}));
}

TEST(DwindleLayout, ResolvesEqualAreasByOpeningOrderAndMissingFocusByLargestArea) {
    auto settings = no_gaps();
    settings.insertion = Insertion::largest;
    WorkspaceLayout layout(settings);
    layout.set_area({0, 0, 700, 700});
    ASSERT_TRUE(layout.add({25, 1}));
    ASSERT_TRUE(layout.add({1, 2}, 25));
    ASSERT_TRUE(layout.add({3, 3}, 1));
    EXPECT_EQ(layout.geometry(3), (WindowRect{0, 350, 350, 350}));

    settings.insertion = Insertion::focused;
    layout.set_settings(settings);
    ASSERT_TRUE(layout.add({4, 4}, 0));
    EXPECT_EQ(layout.geometry(4), (WindowRect{350, 350, 350, 350}));
}

TEST(DwindleLayout, ResizeMovesTheSplitAndRespectsBothClientMinima) {
    WorkspaceLayout layout(no_gaps());
    layout.set_area({0, 0, 1000, 600});
    ASSERT_TRUE(layout.add({1, 1, {.min_width = 600}}));
    ASSERT_TRUE(layout.add({2, 2, {.min_width = 200}}, 1));
    EXPECT_EQ(layout.geometry(1)->width, 600);
    EXPECT_FALSE(layout.resize(1, Direction::left, 32));
    ASSERT_TRUE(layout.resize(1, Direction::right, 900));
    EXPECT_EQ(layout.geometry(1)->width, 800);
    EXPECT_EQ(layout.geometry(2)->width, 200);
    EXPECT_FALSE(layout.resize(1, Direction::right, 1));

    ASSERT_TRUE(layout.resize_by(2, -47, 0));
    EXPECT_EQ(layout.geometry(1)->width, 753);
    EXPECT_EQ(layout.geometry(2)->width, 247);
    ASSERT_TRUE(layout.update_limits(2, {.min_width = 300, .max_height = 500}));
    EXPECT_EQ(layout.geometry(1)->width, 700);
    EXPECT_EQ(layout.geometry(2), (WindowRect{700, 0, 300, 500}));
}

TEST(DwindleLayout, ShrinksInnerGapsAndSquareBordersToFitFeasibleClientMinima) {
    WorkspaceLayout layout({.outer_gap = 0, .inner_gap = 10, .border_width = 1});
    layout.set_area({0, 0, 20, 10});
    ASSERT_TRUE(layout.add({1, 1, {.min_width = 6, .min_height = 8}}));
    ASSERT_TRUE(layout.add({2, 2, {.min_width = 6, .min_height = 8}}, 1));
    EXPECT_EQ(layout.geometry(1), (WindowRect{1, 1, 6, 8}));
    EXPECT_EQ(layout.geometry(2), (WindowRect{13, 1, 6, 8}));
    EXPECT_EQ(layout.border_width(1), 1);
    EXPECT_EQ(layout.border_width(2), 1);

    layout.set_area({0, 0, 12, 10});
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 6, 10}));
    EXPECT_EQ(layout.geometry(2), (WindowRect{6, 0, 6, 10}));
    EXPECT_EQ(layout.border_width(1), 0);
    EXPECT_EQ(layout.border_width(2), 0);

    layout.set_area({});
    EXPECT_FALSE(layout.geometry(1));
    EXPECT_EQ(layout.border_width(1), 0);
}

TEST(DwindleLayout, FitsNestedMinimaAcrossADynamicSplitAxisChange) {
    WorkspaceLayout layout(no_gaps());
    layout.set_area({0, 0, 1000, 400});
    ASSERT_TRUE(layout.add({1, 1, {.min_width = 350}}));
    ASSERT_TRUE(layout.add({2, 2, {.min_width = 600}}, 1));
    ASSERT_TRUE(layout.add({3, 3, {.min_width = 350}}, 1));
    // The left region becomes portrait one pixel below the square threshold,
    // giving its leaves the same width while leaving 600+ pixels on the right.
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 399, 200}));
    EXPECT_EQ(layout.geometry(3), (WindowRect{0, 200, 399, 200}));
    EXPECT_EQ(layout.geometry(2), (WindowRect{399, 0, 601, 400}));
}

TEST(DwindleLayout, DropHonorsTheSelectedEdgeAndCanRestoreAnExactSnapshot) {
    WorkspaceLayout layout(no_gaps());
    layout.set_area({0, 0, 800, 600});
    ASSERT_TRUE(layout.add({1, 1}));
    ASSERT_TRUE(layout.add({2, 2}, 1));
    const auto original = layout;
    const auto moved = layout.erase(2);
    ASSERT_TRUE(moved);
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 800, 600}));
    EXPECT_FALSE(layout.drop_at(810, 300));
    EXPECT_FALSE(layout.add(*moved, {}, TileDrop{999, Direction::up}));

    const auto top = layout.drop_at(420, 15);
    ASSERT_EQ(top, (TileDrop{1, Direction::up}));
    ASSERT_TRUE(layout.add(*moved, {}, top));
    EXPECT_EQ(layout.geometry(2), (WindowRect{0, 0, 800, 300}));
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 300, 800, 300}));
    layout.set_area({0, 0, 900, 600});
    EXPECT_EQ(layout.geometry(2), (WindowRect{0, 0, 450, 600}));

    layout = original;
    EXPECT_EQ(layout.geometry(2), (WindowRect{400, 0, 400, 600}));
    ASSERT_TRUE(layout.swap(1, 2));
    EXPECT_EQ(layout.geometry(2), (WindowRect{0, 0, 400, 600}));
    EXPECT_FALSE(layout.move(2, Direction::left));
    ASSERT_TRUE(layout.move(2, Direction::right));
    EXPECT_EQ(layout.geometry(2), (WindowRect{400, 0, 400, 600}));
}

TEST(ScrollingLayout, RevealsMinimallyAndStopsAtTheStripEdges) {
    WorkspaceLayout layout(no_gaps(Layout::scrolling));
    layout.set_area({20, -30, 400, 292});
    ASSERT_TRUE(layout.add({1, 1}));
    ASSERT_TRUE(layout.add({2, 2, {.min_width = 300}}, 1));
    ASSERT_TRUE(layout.add({3, 3}, 2));
    EXPECT_EQ(layout.geometry(1), (WindowRect{20, -30, 200, 292}));
    EXPECT_EQ(layout.geometry(2), (WindowRect{220, -30, 300, 292}));
    EXPECT_EQ(layout.geometry(3), (WindowRect{520, -30, 200, 292}));
    EXPECT_FALSE(layout.reveal(1));
    ASSERT_TRUE(layout.reveal(2));
    EXPECT_EQ(layout.geometry(2)->x, 120);
    EXPECT_FALSE(layout.reveal(2));
    ASSERT_TRUE(layout.reveal(3));
    EXPECT_EQ(layout.geometry(3)->x, 220);

    EXPECT_EQ(layout.neighbor(1, Direction::right), 2U);
    EXPECT_EQ(layout.neighbor(3, Direction::left), 2U);
    EXPECT_FALSE(layout.neighbor(3, Direction::right));
    EXPECT_FALSE(layout.neighbor(3, Direction::up));
    EXPECT_FALSE(layout.neighbor(1, Direction::left));
    EXPECT_EQ(layout.close_focus(2), 3U);
    EXPECT_EQ(layout.close_focus(3), 2U);
}

TEST(ScrollingLayout, PreservesColumnWidthsAndOffsetsAcrossResizeDragAndOutputLoss) {
    auto settings = no_gaps(Layout::scrolling);
    settings.inner_gap = 10;
    WorkspaceLayout layout(settings);
    layout.set_area({0, 0, 400, 300});
    ASSERT_TRUE(layout.add({1, 1}));
    ASSERT_TRUE(layout.add({2, 2}, 1));
    ASSERT_TRUE(layout.add({3, 3}, 2));
    ASSERT_TRUE(layout.resize(2, Direction::right, 80));
    EXPECT_EQ(layout.geometry(2)->width, 280);
    EXPECT_EQ(layout.geometry(3)->x, 500);
    EXPECT_EQ(layout.geometry(3)->width, 200);
    EXPECT_FALSE(layout.resize(2, Direction::up, 10));
    ASSERT_TRUE(layout.reveal(2));
    EXPECT_EQ(layout.geometry(2)->x, 120);

    layout.set_area({});
    EXPECT_FALSE(layout.geometry(2));
    EXPECT_TRUE(layout.contains(2));
    layout.set_area({0, 0, 400, 300});
    EXPECT_EQ(layout.geometry(2), (WindowRect{120, 0, 280, 300}));
    const auto moved = layout.erase(2);
    ASSERT_TRUE(moved);
    EXPECT_EQ(layout.geometry(3)->x, 200);
    EXPECT_EQ(layout.geometry(3)->width, 200);
    EXPECT_EQ(moved->column_width, 280);
    ASSERT_TRUE(layout.add(*moved, {}, TileDrop{1, Direction::left}));
    ASSERT_TRUE(layout.reveal(2));
    EXPECT_EQ(layout.geometry(2), (WindowRect{0, 0, 280, 300}));
    EXPECT_EQ(layout.geometry(1)->x, 290);
    ASSERT_TRUE(layout.move(2, Direction::right));
    EXPECT_EQ(layout.geometry(2)->x, 210);
    EXPECT_FALSE(layout.move(2, Direction::down));
}

TEST(ScrollingLayout, ExposesTheApproachedEdgeOfAColumnWiderThanTheViewport) {
    WorkspaceLayout layout(no_gaps(Layout::scrolling));
    layout.set_area({-400, 5, 400, 300});
    ASSERT_TRUE(layout.add({1, 1}));
    ASSERT_TRUE(layout.add({2, 2, {.min_width = 605}}, 1));
    ASSERT_TRUE(layout.add({3, 3}, 2));

    ASSERT_TRUE(layout.reveal(2, Direction::right));
    EXPECT_EQ(layout.geometry(2), (WindowRect{-400, 5, 605, 300}));
    ASSERT_TRUE(layout.reveal(3, Direction::right));
    ASSERT_TRUE(layout.reveal(2, Direction::left));
    EXPECT_EQ(layout.geometry(2)->x, -605);
    EXPECT_EQ(layout.drop_at(-5, 150), (TileDrop{2, Direction::right}));
    EXPECT_FALSE(layout.drop_at(5, 150));
    ASSERT_TRUE(layout.reveal(1, Direction::left));
    EXPECT_EQ(layout.geometry(1)->x, -400);
}

TEST(ScrollingLayout, NewDefaultsAffectNewColumnsAndLayoutSwitchDiscardsTheStrip) {
    auto settings = no_gaps(Layout::scrolling);
    WorkspaceLayout layout(settings);
    layout.set_area({0, 0, 600, 500});
    ASSERT_TRUE(layout.add({1, 1}));
    settings.default_column_width = 0.25;
    layout.set_settings(settings);
    ASSERT_TRUE(layout.add({2, 2}, 1));
    EXPECT_EQ(layout.geometry(1)->width, 300);
    EXPECT_EQ(layout.geometry(2)->width, 150);
    ASSERT_TRUE(layout.resize(1, Direction::left, 51));
    settings.kind = Layout::dwindle;
    layout.set_settings(settings);
    EXPECT_FALSE(layout.contains(1));
    ASSERT_TRUE(layout.add({1, 1}));
    EXPECT_EQ(layout.geometry(1)->width, 600);
    settings.kind = Layout::scrolling;
    layout.set_settings(settings);
    ASSERT_TRUE(layout.add({1, 1}));
    EXPECT_EQ(layout.geometry(1), (WindowRect{0, 0, 150, 500}));
}

class LayoutInvariants : public testing::TestWithParam<Layout> {};

TEST_P(LayoutInvariants, SeededOperationsPreserveMembershipAndNonoverlappingAllocations) {
    constexpr unsigned seed = 0x5EA51DE;
    SCOPED_TRACE(seed);
    std::mt19937 random(seed);
    const auto kind = GetParam();
    WorkspaceLayout layout(no_gaps(kind));
    layout.set_area({-100, -250, 1200, 850});
    std::vector<WindowId> members;
    WindowId next = 1;
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
        SCOPED_TRACE(iteration);
        const auto select = [&] { return members[random() % members.size()]; };
        const auto action = random() % 8;
        if (members.empty() || (action < 2 && members.size() < 18)) {
            const auto focused = members.empty() ? std::nullopt : std::optional{select()};
            ASSERT_TRUE(layout.add({next, next}, focused));
            members.push_back(next++);
        } else if (action == 2 && members.size() > 1) {
            const auto removed = select();
            ASSERT_TRUE(layout.erase(removed));
            EXPECT_FALSE(layout.contains(removed));
            std::erase(members, removed);
        } else if (action == 3) {
            const auto target = select();
            const auto previous = layout;
            layout.resize_by(target, 15 * (iteration % 2 ? -1 : 1), 15);
            if (kind == Layout::scrolling) {
                for (const auto id : members) {
                    if (id != target) {
                        EXPECT_EQ(layout.geometry(id)->width, previous.geometry(id)->width);
                    }
                }
            }
        } else if (action == 4) {
            layout.move(select(), iteration % 2 ? Direction::left : Direction::right);
        } else if (action == 5) {
            layout.swap(select(), select());
        } else if (action == 6) {
            layout.reveal(select());
        } else {
            layout.set_area(iteration % 2 ? WindowRect{-100, -250, 1200, 850}
                                          : WindowRect{-100, -250, 850, 1200});
        }
        const auto bounds = layout.viewport();
        for (const auto id : members) {
            ASSERT_TRUE(layout.contains(id));
            const auto box = layout.geometry(id);
            ASSERT_TRUE(box);
            EXPECT_GE(box->width, 0);
            EXPECT_GE(box->height, 0);
            EXPECT_GE(box->y, bounds.y);
            EXPECT_LE(box->y + box->height, bounds.y + bounds.height);
            if (kind == Layout::dwindle) {
                EXPECT_GE(box->x, bounds.x);
                EXPECT_LE(box->x + box->width, bounds.x + bounds.width);
            }
            for (const auto other : members) {
                if (other <= id) {
                    continue;
                }
                const auto candidate = layout.geometry(other);
                ASSERT_TRUE(candidate);
                const auto overlap_width =
                    std::min(box->x + box->width, candidate->x + candidate->width) -
                    std::max(box->x, candidate->x);
                const auto overlap_height =
                    std::min(box->y + box->height, candidate->y + candidate->height) -
                    std::max(box->y, candidate->y);
                EXPECT_FALSE(overlap_width > 0 && overlap_height > 0);
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(WorkspaceLayouts, LayoutInvariants,
                         testing::Values(Layout::dwindle, Layout::scrolling));

TEST(WorkspaceLayouts, LargeLogicalPositionsAndTinyWorkAreasRemainRepresentable) {
    WorkspaceLayout dwindle({.outer_gap = 512, .inner_gap = 512, .border_width = 64});
    dwindle.set_area({-2, -3, 1, 1});
    ASSERT_TRUE(dwindle.add({1, 1, {.min_width = 1700, .min_height = 1200}}));
    ASSERT_TRUE(dwindle.add({2, 2, {.min_width = 1700, .min_height = 1200}}, 1));
    EXPECT_EQ(dwindle.geometry(1)->width + dwindle.geometry(2)->width, 1);
    EXPECT_EQ(dwindle.border_width(1), 0);
    EXPECT_EQ(dwindle.border_width(2), 0);
    EXPECT_FALSE(dwindle.resize_by(2, std::numeric_limits<int>::min(), 0));

    WorkspaceLayout scrolling(no_gaps(Layout::scrolling));
    constexpr auto limit = std::numeric_limits<int>::max();
    scrolling.set_area({-limit, 0, limit, 1000});
    ASSERT_TRUE(scrolling.add({1, 1, {.min_width = limit}}));
    ASSERT_TRUE(scrolling.add({2, 2, {.min_width = limit}}, 1));
    ASSERT_TRUE(scrolling.add({3, 3, {.min_width = limit}}, 2));
    ASSERT_TRUE(scrolling.reveal(3));
    EXPECT_EQ(scrolling.geometry(3)->x, -limit);
    EXPECT_EQ(scrolling.geometry(3)->width, limit);
}

} // namespace
} // namespace seaside

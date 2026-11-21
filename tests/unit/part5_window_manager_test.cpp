#include "config_transaction.hpp"
#include "window_manager.hpp"

#include <gtest/gtest.h>

namespace seaside {
namespace {

constexpr WindowRect output{0, 0, 1000, 600};
constexpr WindowRect client{0, 0, 320, 160};

Config layout_config(Layout kind = Layout::dwindle) {
    Config config;
    config.window.layout = kind;
    config.window.gaps_outer = 0;
    config.window.gaps_inner = 0;
    config.window.border_width = 0;
    return config;
}

WindowId open(WindowManager &windows) {
    const auto id = windows.create_window();
    EXPECT_TRUE(id);
    if (!id) {
        return 0;
    }
    EXPECT_TRUE(windows.initialize_window(*id));
    EXPECT_TRUE(windows.map_window(*id, client));
    return *id;
}

TEST(Part5WindowManager, TileRestoresFloatingGeometryAndSpecialStatePreservesMembership) {
    WindowManager windows(layout_config());
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto left = open(windows);
    const auto right = open(windows);
    const WindowRect left_tile{0, 0, 500, 600};
    const WindowRect right_tile{500, 0, 500, 600};
    EXPECT_EQ(windows.view(left).geometry, left_tile);
    EXPECT_EQ(windows.view(right).geometry, right_tile);

    ASSERT_TRUE(windows.commit_geometry(right, right_tile));
    ASSERT_TRUE(windows.set_maximized(right, true));
    EXPECT_FALSE(windows.view(right).tile_layout);
    EXPECT_EQ(windows.view(right).geometry, output);
    EXPECT_EQ(windows.view(left).geometry, left_tile);
    ASSERT_TRUE(windows.set_fullscreen(right, true));
    ASSERT_TRUE(windows.set_fullscreen(right, false));
    ASSERT_TRUE(windows.set_maximized(right, false));
    EXPECT_EQ(windows.view(right).geometry, right_tile);

    ASSERT_TRUE(windows.toggle_tiling(right));
    const WindowRect floating = windows.view(right).geometry;
    EXPECT_EQ(floating.width, client.width);
    EXPECT_EQ(floating.height, client.height);
    EXPECT_EQ(windows.view(left).geometry, output);
    ASSERT_TRUE(windows.toggle_tiling(right));
    ASSERT_TRUE(windows.toggle_pin(right));
    EXPECT_EQ(windows.view(right).geometry, floating);
    EXPECT_EQ(windows.view(left).geometry, output);
    ASSERT_TRUE(windows.toggle_pin(right));
    EXPECT_EQ(windows.view(right).geometry, right_tile);
}

TEST(Part5WindowManager, MoveDragRestoresOnInvalidDropAndSurvivesTargetRemoval) {
    WindowManager windows(layout_config());
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto left = open(windows);
    const auto right = open(windows);
    const auto start_left = windows.view(left).geometry;
    const auto start_right = windows.view(right).geometry;
    ASSERT_TRUE(windows.begin_move(right));
    ASSERT_TRUE(windows.move_window_by(right, 30, 12));
    EXPECT_FALSE(windows.view(right).tile_layout);
    EXPECT_EQ(windows.view(right).geometry.x, start_right.x + 30);
    EXPECT_EQ(windows.view(left).geometry, output);
    EXPECT_FALSE(windows.finish_move(right, -10, 300));
    EXPECT_EQ(windows.view(left).geometry, start_left);
    EXPECT_EQ(windows.view(right).geometry, start_right);

    ASSERT_TRUE(windows.begin_move(right));
    ASSERT_TRUE(windows.finish_move(right, 30, 300));
    EXPECT_EQ(windows.view(right).geometry, start_left);
    EXPECT_EQ(windows.view(left).geometry, start_right);

    ASSERT_TRUE(windows.begin_move(right));
    ASSERT_TRUE(windows.destroy_window(left));
    EXPECT_FALSE(windows.finish_move(right, 800, 300));
    EXPECT_EQ(windows.view(right).geometry, output);
    EXPECT_TRUE(windows.view(right).tile_layout);

    ASSERT_TRUE(windows.begin_move(right));
    ASSERT_TRUE(windows.remove_output("main"));
    ASSERT_TRUE(windows.add_output("main", output, output));
    EXPECT_TRUE(windows.view(right).tile_layout);
    EXPECT_EQ(windows.view(right).geometry, output);
    EXPECT_TRUE(windows.state().valid());
}

TEST(Part5WindowManager, ScrollingRemembersCustomWidthAndViewportAndClosesToTheRight) {
    WindowManager windows(layout_config(Layout::scrolling));
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto home = *windows.focused_workspace();
    const auto first = open(windows);
    const auto second = open(windows);
    const auto third = open(windows);
    EXPECT_LT(windows.view(first).geometry.x, 0);
    const auto rightmost = windows.view(third).geometry;
    EXPECT_FALSE(windows.focus_direction(Direction::right));
    EXPECT_EQ(windows.focused_window(), third);
    EXPECT_EQ(windows.view(third).geometry, rightmost);
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    EXPECT_EQ(windows.focused_window(), second);
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    EXPECT_EQ(windows.focused_window(), first);
    EXPECT_EQ(windows.view(first).geometry.x, 0);
    EXPECT_FALSE(windows.focus_direction(Direction::left));
    EXPECT_EQ(windows.focused_window(), first);
    EXPECT_EQ(windows.view(first).geometry.x, 0);
    ASSERT_TRUE(windows.focus_direction(Direction::right));
    EXPECT_EQ(windows.focused_window(), second);
    ASSERT_TRUE(windows.resize_window(second, Direction::right, 160));
    EXPECT_EQ(windows.view(second).geometry.width, 660);
    EXPECT_EQ(windows.view(third).geometry.width, 500);
    EXPECT_FALSE(windows.resize_window(second, Direction::down, 100));
    const auto positions = {windows.view(first).geometry, windows.view(second).geometry,
                            windows.view(third).geometry};
    ASSERT_TRUE(windows.select_workspace(27));
    open(windows);
    ASSERT_TRUE(windows.select_workspace(home));
    EXPECT_EQ(windows.focused_window(), second);
    EXPECT_EQ(windows.view(first).geometry, *positions.begin());
    EXPECT_EQ(windows.view(second).geometry, *(positions.begin() + 1));
    EXPECT_EQ(windows.view(third).geometry, *(positions.begin() + 2));
    ASSERT_TRUE(windows.unmap_window(second));
    EXPECT_EQ(windows.focused_window(), third);
    ASSERT_TRUE(windows.destroy_window(first));
    EXPECT_EQ(windows.focused_window(), third);
    EXPECT_EQ(windows.view(third).geometry.x, 0);
}

TEST(Part5WindowManager, CommittedGlobalLayoutChangeRebuildsAllWorkspacesInOpeningOrder) {
    auto config = layout_config();
    WindowManager windows(config);
    ConfigTransaction transaction({config, {}, ConfigSource::defaults, {}});
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto home = *windows.focused_workspace();
    const auto first = open(windows);
    const auto second = open(windows);
    ASSERT_TRUE(windows.swap_window(second, Direction::left));
    EXPECT_LT(windows.view(second).geometry.x, windows.view(first).geometry.x);
    ASSERT_TRUE(windows.toggle_tiling(first));
    ASSERT_TRUE(windows.select_workspace(2));
    const auto hidden_override = open(windows);
    ASSERT_TRUE(windows.toggle_tiling(hidden_override));
    ASSERT_TRUE(windows.toggle_pin(hidden_override));
    ASSERT_TRUE(windows.select_workspace(home));

    const auto commit = [&](const Config &candidate) {
        const auto generation = transaction.begin_reload();
        EXPECT_TRUE(generation);
        if (!generation) {
            return std::expected<ConfigEffects, Diagnostic>(std::unexpected(generation.error()));
        }
        const auto effects =
            transaction.commit({*generation, candidate, ""}, [](const Config &, const Config &) {
                return std::expected<void, Diagnostic>{};
            });
        EXPECT_TRUE(effects);
        if (effects) {
            windows.apply_layout_config(transaction.active());
        }
        return effects;
    };
    config.scrolling.default_column_width = 0.4;
    const auto unrelated = commit(config);
    ASSERT_TRUE(unrelated);
    EXPECT_FALSE(unrelated->reset_tiling_overrides);
    EXPECT_EQ(windows.state().window(first)->tiling_override, false);
    EXPECT_EQ(windows.state().window(hidden_override)->tiling_override, false);

    config.window.layout = Layout::scrolling;
    const auto changed = commit(config);
    ASSERT_TRUE(changed);
    EXPECT_TRUE(changed->reset_tiling_overrides);
    EXPECT_FALSE(windows.state().window(first)->tiling_override.has_value());
    EXPECT_FALSE(windows.state().window(hidden_override)->tiling_override.has_value());
    EXPECT_TRUE(windows.view(hidden_override).pinned);
    EXPECT_EQ(windows.view(first).geometry, (WindowRect{0, 0, 400, 600}));
    EXPECT_EQ(windows.view(second).geometry, (WindowRect{400, 0, 400, 600}));

    config.window.tiling = false;
    ASSERT_TRUE(commit(config));
    EXPECT_FALSE(windows.view(first).tile_layout);
    ASSERT_TRUE(windows.toggle_pin(hidden_override));
    EXPECT_FALSE(windows.view(hidden_override).tile_layout);
}

TEST(Part5WindowManager, ScrollingRebuildStartsAtLeftWithVisibleKeyboardFocus) {
    auto config = layout_config();
    WindowManager windows(config);
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto oldest = open(windows);
    open(windows);
    const auto newest = open(windows);
    ASSERT_EQ(windows.focused_window(), newest);
    const auto dialog = windows.create_window(newest);
    ASSERT_TRUE(dialog);
    ASSERT_TRUE(windows.initialize_window(*dialog));
    ASSERT_TRUE(windows.map_window(*dialog, client));
    ASSERT_EQ(windows.focused_window(), dialog);
    config.window.layout = Layout::scrolling;
    windows.apply_layout_config(config);
    EXPECT_EQ(windows.view(oldest).geometry.x, 0);
    EXPECT_EQ(windows.view(newest).geometry.x, output.width);
    EXPECT_EQ(windows.focused_window(), oldest);
}

TEST(Part5WindowManager, RemovingAnEarlierScrollingColumnKeepsFocusedColumnRevealed) {
    WindowManager windows(layout_config(Layout::scrolling));
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto first = open(windows);
    open(windows);
    open(windows);
    const auto fourth = open(windows);
    open(windows);
    open(windows);
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_EQ(windows.focused_window(), fourth);
    ASSERT_EQ(windows.view(fourth).geometry.x, 0);
    ASSERT_TRUE(windows.destroy_window(first));
    EXPECT_EQ(windows.focused_window(), fourth);
    EXPECT_EQ(windows.view(fourth).geometry.x, 0);
}

TEST(Part5WindowManager, InvalidDropKeepsAChangedScrollingFocusOnScreen) {
    WindowManager windows(layout_config(Layout::scrolling));
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto first = open(windows);
    open(windows);
    open(windows);
    const auto fourth = open(windows);
    open(windows);
    open(windows);
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_TRUE(windows.begin_move(fourth));
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_TRUE(windows.focus_direction(Direction::left));
    ASSERT_EQ(windows.focused_window(), first);
    EXPECT_FALSE(windows.finish_move(fourth, -1, -1));
    EXPECT_EQ(windows.focused_window(), first);
    EXPECT_EQ(windows.view(first).geometry.x, 0);
    EXPECT_TRUE(windows.view(fourth).tile_layout);
}

TEST(Part5WindowManager, LeavingMaximizeRestoresFocusWhenTileNoLongerFits) {
    WindowManager windows(layout_config());
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto first = open(windows);
    const auto second = open(windows);
    ASSERT_TRUE(windows.set_maximized(second, true));
    constexpr WindowRect tiny{0, 0, 1, 1};
    ASSERT_TRUE(windows.update_output("main", tiny, tiny));
    ASSERT_TRUE(windows.set_maximized(second, false));
    EXPECT_FALSE(windows.view(second).visible);
    EXPECT_EQ(windows.focused_window(), first);
}

TEST(Part5WindowManager, RestoringAParentRevealsItsFocusedDialog) {
    WindowManager windows(layout_config(Layout::scrolling));
    ASSERT_TRUE(windows.add_output("main", output, output));
    const auto first = open(windows);
    open(windows);
    open(windows);
    open(windows);
    ASSERT_LT(windows.view(first).geometry.x, 0);
    ASSERT_TRUE(windows.set_maximized(first, true));
    ASSERT_TRUE(windows.click(first));
    const auto dialog = windows.create_window(first);
    ASSERT_TRUE(dialog);
    ASSERT_TRUE(windows.initialize_window(*dialog));
    ASSERT_TRUE(windows.map_window(*dialog, client));
    ASSERT_EQ(windows.focused_window(), dialog);
    ASSERT_TRUE(windows.set_maximized(first, false));
    EXPECT_EQ(windows.focused_window(), dialog);
    EXPECT_EQ(windows.view(first).geometry.x, 0);
    EXPECT_GE(windows.view(*dialog).geometry.x, 0);
}

} // namespace
} // namespace seaside

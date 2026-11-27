#include "window_manager.hpp"

#include <gtest/gtest.h>

namespace seaside {
namespace {

constexpr WindowRect left = {0, 0, 1000, 700};
constexpr WindowRect right = {1000, 0, 800, 600};
constexpr WindowRect top = {0, -600, 800, 600};
constexpr WindowRect usable = {8, 32, 984, 660};
constexpr WindowRect maximized = {20, 44, 960, 636};
constexpr WindowRect client = {0, 0, 320, 200};

WindowId open_window(WindowManager &manager, std::optional<WindowId> parent = {},
                     WindowRect rectangle = client) {
    const auto id = manager.create_window(parent);
    EXPECT_TRUE(id);
    if (!id) {
        return 0;
    }
    EXPECT_TRUE(manager.initialize_window(*id));
    EXPECT_TRUE(manager.map_window(*id, rectangle));
    return *id;
}

TEST(WindowManager, SelectsAndFollowsAcrossOutputsWithoutTransferringDestination) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto left_workspace = manager.focused_workspace();
    ASSERT_TRUE(left_workspace);
    const auto moved = open_window(manager);
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.focus_output_at(1150, 100));
    const auto right_workspace = manager.focused_workspace();
    ASSERT_TRUE(right_workspace);
    ASSERT_NE(left_workspace, right_workspace);
    const auto existing = open_window(manager);

    ASSERT_TRUE(manager.select_workspace(*left_workspace));
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.focused_window(), moved);
    ASSERT_TRUE(manager.move_to_workspace(moved, *right_workspace));
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), moved);
    EXPECT_EQ(manager.state().output("left")->shown, left_workspace);
    EXPECT_EQ(manager.state().output("right")->shown, right_workspace);
    EXPECT_EQ(manager.state().window(existing)->home_workspace, *right_workspace);
    EXPECT_FALSE(manager.state().workspace(*left_workspace)->windows.contains(moved));
    EXPECT_TRUE(manager.state().workspace(*right_workspace)->windows.contains(moved));

    ASSERT_TRUE(manager.select_workspace(3));
    ASSERT_TRUE(manager.focus_output("left"));
    ASSERT_TRUE(manager.select_workspace(*right_workspace));
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), moved);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, KeepsTenWorkspacesAndAdmitsAtMostTwoOutputs) {
    WindowManager manager;
    ASSERT_EQ(manager.state().workspaces().size(), workspace_count);
    for (WorkspaceId id = 1; id <= workspace_count; ++id) {
        ASSERT_NE(manager.state().workspace(id), nullptr);
        EXPECT_FALSE(manager.workspace_output(id));
    }
    EXPECT_FALSE(manager.select_workspace(1));
    ASSERT_TRUE(manager.add_output("left", left, left));
    EXPECT_EQ(manager.focused_workspace(), 1U);
    for (WorkspaceId id = 1; id <= workspace_count; ++id) {
        EXPECT_EQ(manager.workspace_output(id), "left");
    }
    EXPECT_FALSE(manager.select_workspace(0));
    EXPECT_FALSE(manager.select_workspace(workspace_count + 1));

    ASSERT_TRUE(manager.add_output("right", right, right));
    EXPECT_EQ(manager.state().output("left")->shown, 1U);
    EXPECT_EQ(manager.state().output("right")->shown, primary_workspace_count + 1);
    for (WorkspaceId id = 1; id <= workspace_count; ++id) {
        EXPECT_EQ(manager.workspace_output(id), id <= primary_workspace_count ? "left" : "right");
    }
    EXPECT_FALSE(manager.add_output("top", top, top, true));
    EXPECT_EQ(manager.primary_output(), "left");
    EXPECT_EQ(manager.state().output_count(), 2U);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, TogglesPrimaryScratchpadAndRestoresFocusOnBothOutputs) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.select_workspace(3));
    const auto normal = open_window(manager);
    ASSERT_TRUE(manager.select_workspace(8));
    const auto secondary = open_window(manager);
    ASSERT_FALSE(manager.workspace_owner(scratchpad_workspace));
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.focused_workspace(), scratchpad_workspace);
    EXPECT_FALSE(manager.focused_window());
    EXPECT_FALSE(manager.view(normal).visible);
    const auto scratch = open_window(manager);
    EXPECT_EQ(manager.state().window(scratch)->home_workspace, scratchpad_workspace);
    EXPECT_TRUE(manager.view(scratch).visible);
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), secondary);
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), scratch);
    ASSERT_TRUE(manager.select_workspace(8));
    EXPECT_EQ(manager.state().output("left")->shown, scratchpad_workspace);
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), secondary);
    EXPECT_EQ(manager.state().output("left")->shown, 3U);
    EXPECT_FALSE(manager.view(scratch).visible);
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), normal);
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), scratch);
    ASSERT_TRUE(manager.select_workspace(2));
    EXPECT_EQ(manager.state().output("left")->shown, 2U);
    EXPECT_FALSE(manager.view(scratch).visible);
    ASSERT_TRUE(manager.toggle_scratchpad());
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_workspace(), 2U);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, MovesTransientFamilyIntoScratchpadAndKeepsPinsNumbered) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto sibling = open_window(manager);
    const auto keep_pinned = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(keep_pinned));
    const auto parent = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(parent));
    const auto dialog = open_window(manager, parent);
    ASSERT_TRUE(manager.move_to_scratchpad(dialog));
    EXPECT_EQ(manager.focused_window(), keep_pinned);
    EXPECT_FALSE(manager.view(parent).visible);
    EXPECT_FALSE(manager.view(dialog).visible);
    EXPECT_FALSE(manager.view(parent).pinned);
    EXPECT_EQ(manager.state().window(parent)->home_workspace, scratchpad_workspace);
    EXPECT_EQ(manager.state().window(dialog)->home_workspace, scratchpad_workspace);
    EXPECT_FALSE(manager.toggle_pin(parent));
    EXPECT_FALSE(manager.click(dialog));
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_GT(manager.view(dialog).stacking, manager.view(parent).stacking);
    EXPECT_FALSE(manager.view(keep_pinned).visible);
    EXPECT_FALSE(manager.view(sibling).visible);
    const auto geometry = manager.view(parent).geometry;
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_TRUE(manager.view(keep_pinned).visible);
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_EQ(manager.view(parent).geometry, geometry);
    ASSERT_TRUE(manager.move_to_workspace(dialog, 8));
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_EQ(manager.state().window(parent)->home_workspace, 8U);
    EXPECT_EQ(manager.state().window(dialog)->home_workspace, 8U);
    EXPECT_EQ(manager.state().output("left")->shown, scratchpad_workspace);
    ASSERT_TRUE(manager.move_to_scratchpad(dialog));
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_EQ(manager.state().output("right")->shown, 8U);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, KeepsScratchpadClientsWhenThePrimaryChangesOrDisconnects) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.select_workspace(8));
    const auto normal = open_window(manager);
    ASSERT_TRUE(manager.toggle_scratchpad());
    const auto scratch = open_window(manager);
    ASSERT_TRUE(manager.add_output("right", right, right));
    EXPECT_EQ(manager.primary_output(), "left");
    EXPECT_EQ(manager.focused_window(), scratch);
    EXPECT_EQ(manager.state().output("right")->shown, 8U);
    ASSERT_TRUE(manager.set_primary_output("right"));
    EXPECT_EQ(manager.primary_output(), "right");
    EXPECT_EQ(manager.state().output("left")->shown, 8U);
    EXPECT_EQ(manager.state().output("right")->shown, 1U);
    EXPECT_FALSE(manager.view(scratch).visible);
    EXPECT_TRUE(manager.view(normal).visible);
    EXPECT_FALSE(manager.focused_window());
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), scratch);
    EXPECT_EQ(manager.window_output(scratch), "right");
    ASSERT_TRUE(manager.remove_output("right"));
    EXPECT_EQ(manager.primary_output(), "left");
    EXPECT_NE(manager.focused_workspace(), scratchpad_workspace);
    EXPECT_FALSE(manager.view(scratch).visible);
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), scratch);
    ASSERT_TRUE(manager.remove_output("left"));
    EXPECT_FALSE(manager.focused_output());
    EXPECT_FALSE(manager.toggle_scratchpad());
    EXPECT_FALSE(manager.view(scratch).visible);
    const auto unrelated = open_window(manager);
    EXPECT_NE(manager.state().window(unrelated)->home_workspace, scratchpad_workspace);
    ASSERT_TRUE(manager.add_output("right", right, right));
    EXPECT_NE(manager.focused_workspace(), scratchpad_workspace);
    EXPECT_FALSE(manager.view(scratch).visible);
    ASSERT_TRUE(manager.toggle_scratchpad());
    EXPECT_EQ(manager.focused_window(), scratch);
    EXPECT_TRUE(manager.state().window(scratch)->mapped);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, MovingAPinnedWindowFollowsAndFocusesItOverFullscreen) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto pinned = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(pinned));
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.focus_output("right"));
    const auto destination = manager.focused_workspace();
    ASSERT_TRUE(destination);
    const auto fullscreen = open_window(manager);
    ASSERT_TRUE(manager.set_fullscreen(fullscreen, true));
    ASSERT_TRUE(manager.focus_output("left"));

    ASSERT_TRUE(manager.move_to_workspace(pinned, *destination));
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_workspace(), destination);
    EXPECT_EQ(manager.focused_window(), pinned);
    EXPECT_TRUE(manager.view(pinned).visible);
    EXPECT_TRUE(manager.view(pinned).pinned);
    EXPECT_GT(manager.view(pinned).stacking, manager.view(fullscreen).stacking);

    ASSERT_TRUE(manager.click(fullscreen));
    EXPECT_FALSE(manager.view(pinned).visible);
}

TEST(WindowManager, BackgroundMapDoesNotStealFocusAndRemapRetainsIdentity) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto workspace = manager.focused_workspace();
    ASSERT_TRUE(workspace);
    const auto foreground = open_window(manager);
    const auto background = manager.create_window();
    ASSERT_TRUE(background);
    EXPECT_FALSE(manager.map_window(*background, client));
    ASSERT_TRUE(manager.initialize_window(*background));
    ASSERT_TRUE(manager.select_workspace(4));
    const auto other = open_window(manager);
    ASSERT_TRUE(manager.map_window(*background, client));
    EXPECT_EQ(manager.focused_window(), other);
    EXPECT_FALSE(manager.view(*background).visible);

    ASSERT_TRUE(manager.select_workspace(*workspace));
    EXPECT_EQ(manager.focused_window(), foreground);
    ASSERT_TRUE(manager.click(*background));
    EXPECT_EQ(manager.focused_window(), background);
    const auto opening = manager.state().window(*background)->initial_mapping_seq;
    ASSERT_TRUE(manager.unmap_window(*background));
    EXPECT_EQ(manager.focused_window(), foreground);
    EXPECT_FALSE(manager.view(*background).visible);
    ASSERT_TRUE(manager.map_window(*background, client));
    EXPECT_EQ(manager.state().window(*background)->initial_mapping_seq, opening);
    EXPECT_EQ(manager.focused_window(), background);
    ASSERT_TRUE(manager.destroy_window(*background));
    EXPECT_EQ(manager.focused_window(), foreground);
    EXPECT_FALSE(manager.state().window(*background));
    EXPECT_FALSE(manager.unmap_window(*background));
    EXPECT_FALSE(manager.click(*background));
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, KeepsHeldMruStableAndSkipsUnmappedWindows) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto first = open_window(manager);
    const auto second = open_window(manager);
    const auto third = open_window(manager);

    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), second);
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), first);
    ASSERT_TRUE(manager.unmap_window(third));
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), second);
    manager.finish_focus_cycle();
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), first);
    ASSERT_TRUE(manager.select_workspace(9));
    EXPECT_FALSE(manager.cycle_focus(false));
    EXPECT_FALSE(manager.focused_window());
}

TEST(WindowManager, CyclesSeatHistoryAcrossDisplayedWorkspacesWithoutChangingThem) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto a = open_window(manager);
    const auto b = open_window(manager);
    ASSERT_TRUE(manager.focus_output("right"));
    const auto c = open_window(manager);
    const auto d = open_window(manager);
    ASSERT_TRUE(manager.click(a));

    for (const auto &[id, output] : {std::pair{d, "right"},
                                     {c, "right"},
                                     {b, "left"},
                                     {a, "left"},
                                     {d, "right"},
                                     {c, "right"}}) {
        ASSERT_TRUE(manager.cycle_focus(true));
        EXPECT_EQ(manager.focused_window(), id);
        EXPECT_EQ(manager.focused_output(), output);
        EXPECT_EQ(manager.state().output("left")->shown, 1U);
        EXPECT_EQ(manager.state().output("right")->shown, 6U);
    }
    manager.finish_focus_cycle();
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), a);
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), c);

    ASSERT_TRUE(manager.focus_output("left"));
    EXPECT_EQ(manager.focused_window(), a);
    ASSERT_TRUE(manager.focus_output("right"));
    EXPECT_EQ(manager.focused_window(), c);
}

TEST(WindowManager, RevalidatesHeldCycleAfterMapUnmapAndOutputRemoval) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto a = open_window(manager);
    const auto b = open_window(manager);
    ASSERT_TRUE(manager.focus_output("right"));
    const auto c = open_window(manager);
    const auto d = open_window(manager);
    ASSERT_TRUE(manager.click(a));

    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), d);
    ASSERT_TRUE(manager.unmap_window(c));
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), b);
    const auto newcomer = open_window(manager);
    ASSERT_TRUE(manager.remove_output("right"));
    EXPECT_FALSE(manager.view(d).visible);
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), a);
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), b);
    manager.finish_focus_cycle();
    ASSERT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), a);
    EXPECT_TRUE(manager.view(newcomer).visible);
}

TEST(WindowManager, CyclesAFloatingPinAndFullscreenDialogsAcrossOutputs) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto pinned = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(pinned));
    ASSERT_TRUE(manager.select_workspace(3));
    const auto ordinary = open_window(manager);
    ASSERT_TRUE(manager.focus_output("right"));
    const auto fullscreen = open_window(manager);
    ASSERT_TRUE(manager.set_fullscreen(fullscreen, true));
    const auto dialog = open_window(manager, fullscreen);
    ASSERT_TRUE(manager.click(ordinary));
    for (const auto id : {dialog, fullscreen, pinned, ordinary}) {
        ASSERT_TRUE(manager.cycle_focus(true));
        EXPECT_EQ(manager.focused_window(), id);
    }
    EXPECT_TRUE(manager.view(fullscreen).fullscreen);
    EXPECT_EQ(manager.state().output("left")->shown, 3U);
    EXPECT_EQ(manager.state().output("right")->shown, 6U);
}

TEST(WindowManager, CyclesVisibleClientsWithoutHistoryInWindowIdOrder) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto first = manager.create_window();
    const auto second = manager.create_window();
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(manager.focus_output("right"));
    EXPECT_FALSE(manager.cycle_focus(false));
    ASSERT_TRUE(manager.initialize_window(*second));
    ASSERT_TRUE(manager.map_window(*second, client));
    ASSERT_TRUE(manager.initialize_window(*first));
    ASSERT_TRUE(manager.map_window(*first, client));
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), first);
    EXPECT_EQ(manager.focused_output(), "left");
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), second);
    manager.finish_focus_cycle();
    ASSERT_TRUE(manager.unmap_window(*first));
    EXPECT_FALSE(manager.cycle_focus(false));
}

TEST(WindowManager, FocusRevealsATileWithoutChangingMaximizedOrTiledGeometry) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, usable, true));
    const auto tile = open_window(manager);
    const auto browser = open_window(manager);
    const auto tile_geometry = manager.view(tile).geometry;
    ASSERT_TRUE(manager.set_maximized(browser, true));
    ASSERT_GT(manager.view(browser).stacking, manager.view(tile).stacking);

    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), tile);
    EXPECT_GT(manager.view(tile).stacking, manager.view(browser).stacking);
    EXPECT_EQ(manager.view(tile).geometry, tile_geometry);
    EXPECT_EQ(manager.view(browser).geometry, maximized);
    EXPECT_TRUE(manager.view(browser).maximized);

    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), browser);
    EXPECT_GT(manager.view(browser).stacking, manager.view(tile).stacking);
    EXPECT_EQ(manager.view(tile).geometry, tile_geometry);
    EXPECT_TRUE(manager.view(browser).maximized);
}

TEST(WindowManager, DirectionalFocusWrapsInTheCurrentWorkspace) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto a = open_window(manager);
    const auto b = open_window(manager);
    const auto c = open_window(manager);
    ASSERT_TRUE(manager.focus_direction(Direction::right));
    EXPECT_EQ(manager.focused_window(), a);
    ASSERT_TRUE(manager.focus_direction(Direction::right));
    EXPECT_EQ(manager.focused_window(), b);
    ASSERT_TRUE(manager.focus_direction(Direction::left));
    EXPECT_EQ(manager.focused_window(), a);
    ASSERT_TRUE(manager.focus_direction(Direction::left));
    EXPECT_EQ(manager.focused_window(), c);
    EXPECT_EQ(manager.focused_output(), "left");
}

TEST(WindowManager, KeyboardCycleRaisesFloatsAndMappingTilesPreservesTheirStack) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto a = open_window(manager);
    ASSERT_TRUE(manager.toggle_tiling(a));
    const auto b = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(b));
    const auto c = open_window(manager);
    EXPECT_GT(manager.view(a).stacking, manager.view(c).stacking);
    EXPECT_GT(manager.view(b).stacking, manager.view(c).stacking);
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), b);
    EXPECT_GT(manager.view(b).stacking, manager.view(a).stacking);
    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), a);
    EXPECT_GT(manager.view(a).stacking, manager.view(b).stacking);
}

TEST(WindowManager, KeyboardNavigationAndUnpinFollowVisiblePinnedWindows) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto pinned = open_window(manager);
    ASSERT_TRUE(manager.toggle_pin(pinned));
    const auto home = manager.focused_workspace().value();
    ASSERT_TRUE(manager.select_workspace(3));
    const auto tile = open_window(manager);
    EXPECT_TRUE(manager.focus_direction(Direction::left));
    EXPECT_EQ(manager.focused_window(), pinned);
    ASSERT_TRUE(manager.click(pinned));
    EXPECT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), tile);
    ASSERT_TRUE(manager.click(tile));
    EXPECT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_window(), pinned);
    ASSERT_TRUE(manager.click(pinned));
    ASSERT_TRUE(manager.toggle_pin(pinned));
    EXPECT_EQ(manager.state().window(pinned)->home_workspace, 3U);
    EXPECT_FALSE(manager.state().workspace(home)->windows.contains(pinned));
    EXPECT_TRUE(manager.view(pinned).tiled);
    EXPECT_TRUE(manager.view(pinned).visible);
    EXPECT_EQ(manager.focused_window(), pinned);
    ASSERT_TRUE(manager.focus_output("right"));
    EXPECT_TRUE(manager.cycle_focus(false));
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.focused_window(), pinned);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, FloatingRaiseAndLayeredGeometryRestore) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, usable, true));
    const auto a = open_window(manager);
    const auto b = open_window(manager);
    const auto original = manager.view(a).geometry;
    ASSERT_LT(manager.view(a).stacking, manager.view(b).stacking);
    ASSERT_TRUE(manager.click(a));
    EXPECT_GT(manager.view(a).stacking, manager.view(b).stacking);
    ASSERT_TRUE(manager.set_maximized(a, true));
    EXPECT_EQ(manager.view(a).geometry, maximized);
    ASSERT_TRUE(manager.commit_geometry(a, maximized));
    ASSERT_TRUE(manager.set_fullscreen(a, true));
    EXPECT_EQ(manager.view(a).geometry, left);
    EXPECT_TRUE(manager.view(a).maximized);
    ASSERT_TRUE(manager.commit_geometry(a, left));
    ASSERT_TRUE(manager.set_fullscreen(a, false));
    EXPECT_EQ(manager.view(a).geometry, maximized);
    ASSERT_TRUE(manager.set_maximized(a, false));
    EXPECT_EQ(manager.view(a).geometry, original);
    EXPECT_FALSE(manager.view(a).fullscreen);
}

TEST(WindowManager, InitialFullscreenDoesNotSaveItsBoundsAsOrdinaryGeometry) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, usable));
    const auto id = manager.create_window();
    ASSERT_TRUE(id);
    ASSERT_TRUE(manager.set_fullscreen(*id, true));
    EXPECT_EQ(manager.view(*id).geometry, left);
    ASSERT_TRUE(manager.initialize_window(*id));
    ASSERT_TRUE(manager.map_window(*id, left));
    ASSERT_TRUE(manager.set_fullscreen(*id, false));
    EXPECT_EQ(manager.view(*id).geometry.width, 0);
    ASSERT_TRUE(manager.commit_geometry(*id, client));
    EXPECT_EQ(manager.view(*id).geometry.width, client.width);
    EXPECT_EQ(manager.view(*id).geometry.height, client.height);
}

TEST(WindowManager, MaximizedBorderFitsWithinTheWorkAreaWithZeroOuterGap) {
    Config config;
    config.window.gaps_outer = 0;
    config.window.border_width = 4;
    WindowManager manager(config);
    ASSERT_TRUE(manager.add_output("left", left, usable));
    const auto id = open_window(manager);

    ASSERT_TRUE(manager.set_maximized(id, true));
    EXPECT_EQ(manager.view(id).geometry, (WindowRect{12, 36, 976, 652}));
    EXPECT_EQ(manager.view(id).border_width, 4);

    ASSERT_TRUE(manager.set_fullscreen(id, true));
    EXPECT_EQ(manager.view(id).geometry, left);
    EXPECT_EQ(manager.view(id).border_width, 0);

    ASSERT_TRUE(manager.set_fullscreen(id, false));
    EXPECT_EQ(manager.view(id).geometry, (WindowRect{12, 36, 976, 652}));
    EXPECT_EQ(manager.view(id).border_width, 4);
}

TEST(WindowManager, NewlyFocusedOrdinaryWindowIsVisibleAboveFullscreen) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, usable));
    const auto fullscreen = open_window(manager);
    ASSERT_TRUE(manager.set_fullscreen(fullscreen, true));
    const auto dialog = open_window(manager, fullscreen);
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_GT(manager.view(dialog).stacking, manager.view(fullscreen).stacking);
    const auto ordinary = open_window(manager);
    EXPECT_EQ(manager.focused_window(), ordinary);
    EXPECT_GT(manager.view(ordinary).stacking, manager.view(fullscreen).stacking);
    EXPECT_TRUE(manager.view(fullscreen).fullscreen);
    ASSERT_TRUE(manager.destroy_window(ordinary));
    EXPECT_EQ(manager.focused_window(), dialog);
    EXPECT_GT(manager.view(dialog).stacking, manager.view(fullscreen).stacking);
}

TEST(WindowManager, PinRetainsOneHomeAndRecoversPriorTiling) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto right_home = manager.state().output("right")->shown;
    ASSERT_TRUE(right_home);
    const auto home = manager.focused_workspace();
    ASSERT_TRUE(home);
    const auto pinned = open_window(manager);
    ASSERT_TRUE(manager.view(pinned).tiled);
    ASSERT_TRUE(manager.toggle_pin(pinned));
    EXPECT_TRUE(manager.view(pinned).pinned);
    EXPECT_FALSE(manager.view(pinned).tiled);
    ASSERT_TRUE(manager.select_workspace(4));
    EXPECT_TRUE(manager.view(pinned).visible);
    EXPECT_EQ(manager.window_output(pinned), "left");
    EXPECT_EQ(manager.state().window(pinned)->home_workspace, *home);
    EXPECT_FALSE(manager.state().workspace(4)->windows.contains(pinned));

    const auto fullscreen = open_window(manager);
    ASSERT_TRUE(manager.set_fullscreen(fullscreen, true));
    EXPECT_FALSE(manager.view(pinned).visible);
    ASSERT_TRUE(manager.set_fullscreen(fullscreen, false));
    EXPECT_TRUE(manager.view(pinned).visible);
    ASSERT_TRUE(manager.move_to_workspace(pinned, *right_home));
    EXPECT_EQ(manager.window_output(pinned), "right");
    ASSERT_TRUE(manager.toggle_pin(pinned));
    EXPECT_FALSE(manager.view(pinned).pinned);
    EXPECT_TRUE(manager.view(pinned).tiled);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, DialogsRemainRelativeAndBackgroundDialogsDoNotTakeFocus) {
    WindowManager manager(false);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto parent = open_window(manager, {}, {0, 0, 500, 400});
    const auto foreground = open_window(manager);
    const auto dialog = open_window(manager, parent, {0, 0, 100, 80});
    EXPECT_EQ(manager.focused_window(), foreground);
    const auto parent_geometry = manager.view(parent).geometry;
    const auto dialog_geometry = manager.view(dialog).geometry;
    EXPECT_EQ(dialog_geometry.x, parent_geometry.x + 200);
    EXPECT_EQ(dialog_geometry.y, parent_geometry.y + 160);
    ASSERT_TRUE(manager.move_window_by(dialog, 12, -13));
    ASSERT_TRUE(manager.move_window_by(parent, 20, 10));
    EXPECT_EQ(manager.view(dialog).geometry.x, dialog_geometry.x + 32);
    EXPECT_EQ(manager.view(dialog).geometry.y, dialog_geometry.y - 3);
    ASSERT_TRUE(manager.click(parent));
    EXPECT_GT(manager.view(parent).stacking, manager.view(dialog).stacking);
    EXPECT_FALSE(manager.set_parent(parent, dialog));
    ASSERT_TRUE(manager.move_to_workspace(parent, 7));
    EXPECT_EQ(manager.state().window(dialog)->home_workspace, 7U);
    EXPECT_TRUE(manager.view(dialog).visible);
    ASSERT_TRUE(manager.destroy_window(parent));
    EXPECT_TRUE(manager.state().window(dialog));
    EXPECT_TRUE(manager.view(dialog).visible);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, ZeroOutputsPreserveExistingAndNewClients) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, usable, true));
    const auto initial = manager.focused_workspace();
    ASSERT_TRUE(initial);
    const auto existing = open_window(manager);
    ASSERT_TRUE(manager.remove_output("left"));
    EXPECT_FALSE(manager.focused_output());
    EXPECT_FALSE(manager.focused_window());
    EXPECT_FALSE(manager.view(existing).visible);
    EXPECT_TRUE(manager.state().window(existing)->mapped);
    const auto during_loss = open_window(manager);
    EXPECT_TRUE(manager.state().window(during_loss)->mapped);
    ASSERT_TRUE(manager.add_output("left", left, usable, true));
    EXPECT_EQ(manager.focused_workspace(), initial);
    EXPECT_EQ(manager.focused_window(), existing);
    EXPECT_TRUE(manager.view(existing).visible);
    EXPECT_TRUE(manager.view(during_loss).visible);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, RestoresEachWorkspaceRangeAndFocusAfterHotplug) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.select_workspace(4));
    const auto primary_window = open_window(manager);
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.select_workspace(8));
    const auto secondary_window = open_window(manager);

    ASSERT_TRUE(manager.remove_output("right"));
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.focused_workspace(), 8U);
    EXPECT_EQ(manager.focused_window(), secondary_window);
    EXPECT_EQ(manager.workspace_output(8), "left");
    ASSERT_TRUE(manager.add_output("replacement", right, right));
    EXPECT_EQ(manager.workspace_output(8), "replacement");
    EXPECT_EQ(manager.state().output("left")->shown, 4U);
    EXPECT_EQ(manager.state().output("replacement")->shown, 8U);
    EXPECT_EQ(manager.focused_output(), "replacement");
    EXPECT_EQ(manager.focused_window(), secondary_window);
    ASSERT_TRUE(manager.select_workspace(4));
    EXPECT_EQ(manager.focused_window(), primary_window);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, PrimaryChangeSwapsTheRangesWithoutChangingWindowHomes) {
    WindowManager manager;
    ASSERT_TRUE(manager.add_output("left", left, left));
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.select_workspace(8));
    const auto secondary_window = open_window(manager);
    ASSERT_TRUE(manager.select_workspace(4));
    const auto primary_window = open_window(manager);

    ASSERT_TRUE(manager.set_primary_output("right"));
    EXPECT_EQ(manager.primary_output(), "right");
    EXPECT_EQ(manager.state().output("right")->shown, 4U);
    EXPECT_EQ(manager.state().output("left")->shown, 8U);
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), primary_window);
    EXPECT_EQ(manager.state().window(primary_window)->home_workspace, 4U);
    EXPECT_EQ(manager.state().window(secondary_window)->home_workspace, 8U);
    EXPECT_GE(manager.view(primary_window).geometry.x, right.x);
    EXPECT_LT(manager.view(secondary_window).geometry.x, right.x);
    EXPECT_FALSE(manager.set_primary_output("right"));
    EXPECT_TRUE(manager.state().valid());
}

} // namespace
} // namespace seaside

#include "window_manager.hpp"

#include <gtest/gtest.h>

namespace seaside {
namespace {

constexpr WindowRect left = {0, 0, 1000, 700};
constexpr WindowRect right = {1000, 0, 800, 600};
constexpr WindowRect top = {0, -600, 800, 600};
constexpr WindowRect usable = {8, 32, 984, 660};
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

class WorkspaceModes : public ::testing::TestWithParam<WorkspaceMode> {};

TEST_P(WorkspaceModes, SelectsAndFollowsAcrossOutputsWithoutTransferringDestination) {
    WindowManager manager(GetParam());
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

    ASSERT_TRUE(manager.select_workspace(21));
    ASSERT_TRUE(manager.focus_output("left"));
    ASSERT_TRUE(manager.select_workspace(*right_workspace));
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), moved);
    EXPECT_TRUE(manager.state().valid());
}

TEST_P(WorkspaceModes, TransferHidesDestinationAndRestoresSourceHistory) {
    WindowManager manager(GetParam());
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto first = manager.focused_workspace();
    ASSERT_TRUE(first);
    ASSERT_TRUE(manager.add_output("right", right, right));
    const auto previous = manager.state().output("right")->shown;
    ASSERT_TRUE(previous);
    ASSERT_TRUE(manager.select_workspace(31));
    const auto window = open_window(manager);

    ASSERT_TRUE(manager.transfer_workspace("right"));
    EXPECT_EQ(manager.state().output("left")->shown, first);
    EXPECT_EQ(manager.state().output("right")->shown, 31U);
    EXPECT_EQ(manager.state().workspace(*previous)->owner_or_last_output, "right");
    EXPECT_EQ(manager.focused_output(), "right");
    EXPECT_EQ(manager.focused_window(), window);
    EXPECT_TRUE(manager.state().valid());
}

TEST_P(WorkspaceModes, TransfersAnExplicitHiddenWorkspaceToTheFocusedOutput) {
    WindowManager manager(GetParam());
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.select_workspace(31));
    const auto window = open_window(manager);
    ASSERT_TRUE(manager.select_workspace(32));
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.focus_output("right"));
    const auto previous = open_window(manager);

    ASSERT_TRUE(manager.transfer_workspace("right", 31));
    EXPECT_EQ(manager.state().output("left")->shown, 32U);
    EXPECT_EQ(manager.state().output("right")->shown, 31U);
    EXPECT_EQ(manager.workspace_owner(31), "right");
    EXPECT_EQ(manager.focused_window(), window);
    EXPECT_FALSE(manager.view(previous).visible);
    EXPECT_FALSE(manager.transfer_workspace("left", 75));
    EXPECT_TRUE(manager.state().valid());
}

TEST_P(WorkspaceModes, MovingAPinnedWindowFollowsAndFocusesItOverFullscreen) {
    WindowManager manager(GetParam());
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

INSTANTIATE_TEST_SUITE_P(WindowManager, WorkspaceModes,
                         ::testing::Values(WorkspaceMode::global, WorkspaceMode::per_monitor));

TEST(WindowManager, BackgroundMapDoesNotStealFocusAndRemapRetainsIdentity) {
    WindowManager manager(WorkspaceMode::global);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    const auto workspace = manager.focused_workspace();
    ASSERT_TRUE(workspace);
    const auto foreground = open_window(manager);
    const auto background = manager.create_window();
    ASSERT_TRUE(background);
    EXPECT_FALSE(manager.map_window(*background, client));
    ASSERT_TRUE(manager.initialize_window(*background));
    ASSERT_TRUE(manager.select_workspace(14));
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
    WindowManager manager(WorkspaceMode::global);
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
    ASSERT_TRUE(manager.select_workspace(19));
    EXPECT_FALSE(manager.cycle_focus(false));
    EXPECT_FALSE(manager.focused_window());
}

TEST(WindowManager, FocusRevealsATileWithoutChangingMaximizedOrTiledGeometry) {
    WindowManager manager(WorkspaceMode::global);
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
    EXPECT_EQ(manager.view(browser).geometry, usable);
    EXPECT_TRUE(manager.view(browser).maximized);

    ASSERT_TRUE(manager.cycle_focus(true));
    EXPECT_EQ(manager.focused_window(), browser);
    EXPECT_GT(manager.view(browser).stacking, manager.view(tile).stacking);
    EXPECT_EQ(manager.view(tile).geometry, tile_geometry);
    EXPECT_TRUE(manager.view(browser).maximized);
}

TEST(WindowManager, DirectionalFocusWrapsInTheCurrentWorkspace) {
    WindowManager manager(WorkspaceMode::global, false);
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

TEST(WindowManager, FloatingRaiseAndLayeredGeometryRestore) {
    WindowManager manager(WorkspaceMode::global, false);
    ASSERT_TRUE(manager.add_output("left", left, usable, true));
    const auto a = open_window(manager);
    const auto b = open_window(manager);
    const auto original = manager.view(a).geometry;
    ASSERT_LT(manager.view(a).stacking, manager.view(b).stacking);
    ASSERT_TRUE(manager.click(a));
    EXPECT_GT(manager.view(a).stacking, manager.view(b).stacking);
    ASSERT_TRUE(manager.set_maximized(a, true));
    EXPECT_EQ(manager.view(a).geometry, usable);
    ASSERT_TRUE(manager.commit_geometry(a, usable));
    ASSERT_TRUE(manager.set_fullscreen(a, true));
    EXPECT_EQ(manager.view(a).geometry, left);
    EXPECT_TRUE(manager.view(a).maximized);
    ASSERT_TRUE(manager.commit_geometry(a, left));
    ASSERT_TRUE(manager.set_fullscreen(a, false));
    EXPECT_EQ(manager.view(a).geometry, usable);
    ASSERT_TRUE(manager.set_maximized(a, false));
    EXPECT_EQ(manager.view(a).geometry, original);
    EXPECT_FALSE(manager.view(a).fullscreen);
}

TEST(WindowManager, InitialFullscreenDoesNotSaveItsBoundsAsOrdinaryGeometry) {
    WindowManager manager(WorkspaceMode::global, false);
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

TEST(WindowManager, NewlyFocusedOrdinaryWindowIsVisibleAboveFullscreen) {
    WindowManager manager(WorkspaceMode::global);
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
    WindowManager manager(WorkspaceMode::global);
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
    ASSERT_TRUE(manager.select_workspace(44));
    EXPECT_TRUE(manager.view(pinned).visible);
    EXPECT_EQ(manager.window_output(pinned), "left");
    EXPECT_EQ(manager.state().window(pinned)->home_workspace, *home);
    EXPECT_FALSE(manager.state().workspace(44)->windows.contains(pinned));

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
    WindowManager manager(WorkspaceMode::global, false);
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
    ASSERT_TRUE(manager.move_to_workspace(parent, 17));
    EXPECT_EQ(manager.state().window(dialog)->home_workspace, 17U);
    EXPECT_TRUE(manager.view(dialog).visible);
    ASSERT_TRUE(manager.destroy_window(parent));
    EXPECT_TRUE(manager.state().window(dialog));
    EXPECT_TRUE(manager.view(dialog).visible);
    EXPECT_TRUE(manager.state().valid());
}

TEST(WindowManager, ZeroOutputsPreserveExistingAndNewClients) {
    WindowManager manager(WorkspaceMode::per_monitor);
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

TEST(WindowManager, RestoresAutomaticMigrationAndHonorsManualReassignment) {
    WindowManager manager(WorkspaceMode::per_monitor);
    ASSERT_TRUE(manager.add_output("left", left, left, true));
    ASSERT_TRUE(manager.add_output("right", right, right));
    ASSERT_TRUE(manager.add_output("top", top, top));
    ASSERT_TRUE(manager.focus_output("right"));
    const auto old_home = manager.focused_workspace();
    ASSERT_TRUE(old_home);
    const auto window = open_window(manager);
    ASSERT_TRUE(manager.select_workspace(77));

    ASSERT_TRUE(manager.remove_output("right"));
    EXPECT_EQ(manager.focused_output(), "left");
    EXPECT_EQ(manager.state().workspace(*old_home)->owner_or_last_output, "left");
    ASSERT_TRUE(manager.select_workspace(*old_home));
    EXPECT_EQ(manager.focused_window(), window);
    ASSERT_TRUE(manager.transfer_workspace("top"));
    ASSERT_TRUE(manager.add_output("right", right, right));
    EXPECT_EQ(manager.state().workspace(*old_home)->owner_or_last_output, "top");
    EXPECT_EQ(manager.state().workspace(77)->owner_or_last_output, "right");
    EXPECT_EQ(manager.state().output("right")->shown, 77U);
    EXPECT_EQ(manager.focused_output(), "top");
    EXPECT_EQ(manager.focused_window(), window);
    EXPECT_TRUE(manager.state().valid());
}

} // namespace
} // namespace seaside

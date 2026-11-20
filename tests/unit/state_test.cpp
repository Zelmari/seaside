#include "state.hpp"

#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace {
using seaside::State;
using seaside::StateError;

class StateTest : public testing::Test {
    void SetUp() override {
        RecordProperty("part", "2");
        RecordProperty("requirement", "spec-2.2-10.6-state");
    }
};

TEST_F(StateTest, ValidatesWorkspaceBoundsAndOpaqueOutputIdentity) {
    State state;
    EXPECT_EQ(state.add_workspace(0).error(), StateError::invalid_id);
    EXPECT_EQ(state.add_workspace(2147483648U).error(), StateError::invalid_id);
    ASSERT_TRUE(state.add_workspace(2147483647U));
    EXPECT_EQ(state.add_workspace(2147483647U).error(), StateError::duplicate);
    EXPECT_EQ(state.add_output("").error(), StateError::invalid_id);
    ASSERT_TRUE(state.add_output("output:opaque-1"));
    EXPECT_EQ(state.add_output("output:opaque-1").error(), StateError::duplicate);
    EXPECT_EQ(state.create_window(1).error(), StateError::missing_workspace);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, OnlyInitializedMappedWindowsAreEligibleAndDestroyedIdsStayStale) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    const auto created = state.create_window(1);
    ASSERT_TRUE(created);
    const auto id = *created;
    EXPECT_EQ(state.map_window(id).error(), StateError::not_initialized);
    ASSERT_TRUE(state.initialize_window(id));
    ASSERT_TRUE(state.map_window(id));
    EXPECT_EQ(state.map_window(id).error(), StateError::already_mapped);
    auto mapped = state.mapped_windows(1);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(*mapped, std::vector<seaside::WindowId>{id});
    ASSERT_TRUE(state.unmap_window(id));
    EXPECT_NE(state.window(id), nullptr);
    EXPECT_TRUE(state.mapped_windows(1)->empty());
    EXPECT_EQ(state.unmap_window(id).error(), StateError::not_mapped);
    ASSERT_TRUE(state.map_window(id));
    ASSERT_TRUE(state.destroy_window(id));
    EXPECT_EQ(state.window(id), nullptr);
    EXPECT_TRUE(state.workspace(1)->windows.empty());
    EXPECT_EQ(state.map_window(id).error(), StateError::missing_window);
    EXPECT_EQ(state.destroy_window(id).error(), StateError::missing_window);
    auto replacement = state.create_window(1);
    ASSERT_TRUE(replacement);
    EXPECT_GT(*replacement, id);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, MovesAreUniqueAndFailedMovesDoNotChangeMembership) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_workspace(2));
    auto created = state.create_window(1);
    ASSERT_TRUE(created);
    const auto id = *created;
    EXPECT_EQ(state.move_window(id, 3).error(), StateError::missing_workspace);
    EXPECT_EQ(state.window(id)->home_workspace, 1U);
    EXPECT_TRUE(state.workspace(1)->windows.contains(id));
    ASSERT_TRUE(state.move_window(id, 2));
    ASSERT_TRUE(state.move_window(id, 2));
    EXPECT_EQ(state.window(id)->home_workspace, 2U);
    EXPECT_FALSE(state.workspace(1)->windows.contains(id));
    EXPECT_EQ(state.workspace(2)->windows.size(), 1U);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, RejectedGraphMutationsPreserveMembershipVisibilityAndLiveObjects) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_workspace(2));
    ASSERT_TRUE(state.add_output("A"));
    ASSERT_TRUE(state.add_output("B"));
    ASSERT_TRUE(state.assign_workspace(1, "A"));
    ASSERT_TRUE(state.assign_workspace(2, "B"));
    ASSERT_TRUE(state.show_workspace("A", 1));
    ASSERT_TRUE(state.show_workspace("B", 2));
    auto first = state.create_window(1);
    auto second = state.create_window(2);
    auto retired = state.create_window(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(retired);
    ASSERT_TRUE(state.initialize_window(*first));
    ASSERT_TRUE(state.map_window(*first));
    ASSERT_TRUE(state.destroy_window(*retired));

    const auto unchanged = [&] {
        EXPECT_TRUE(state.valid());
        EXPECT_EQ(state.output_count(), 2U);
        EXPECT_EQ(state.window_count(), 2U);
        ASSERT_NE(state.output("A"), nullptr);
        ASSERT_NE(state.output("B"), nullptr);
        EXPECT_EQ(state.output("A")->shown, 1U);
        EXPECT_EQ(state.output("B")->shown, 2U);
        EXPECT_EQ(state.workspace(1)->owner_or_last_output, "A");
        EXPECT_EQ(state.workspace(2)->owner_or_last_output, "B");
        EXPECT_EQ(state.workspace(1)->windows, (std::set<seaside::WindowId>{*first}));
        EXPECT_EQ(state.workspace(2)->windows, (std::set<seaside::WindowId>{*second}));
        ASSERT_NE(state.window(*first), nullptr);
        ASSERT_NE(state.window(*second), nullptr);
        EXPECT_EQ(state.window(*first)->home_workspace, 1U);
        EXPECT_TRUE(state.window(*first)->initialized);
        EXPECT_TRUE(state.window(*first)->mapped);
        EXPECT_EQ(state.window(*second)->home_workspace, 2U);
        EXPECT_FALSE(state.window(*second)->initialized);
        EXPECT_FALSE(state.window(*second)->mapped);
        EXPECT_EQ(state.window(*retired), nullptr);
    };
    const auto rejected = [&](auto result, StateError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
        unchanged();
    };
    rejected(state.assign_workspace(3, "A"), StateError::missing_workspace);
    rejected(state.assign_workspace(1, "missing"), StateError::missing_output);
    rejected(state.show_workspace("missing", 1), StateError::missing_output);
    rejected(state.show_workspace("A", 3), StateError::missing_workspace);
    rejected(state.show_workspace("B", 1), StateError::wrong_owner);
    rejected(state.remove_output("missing"), StateError::missing_output);
    rejected(state.create_window(3), StateError::missing_workspace);
    rejected(state.move_window(*first, 3), StateError::missing_workspace);
    rejected(state.initialize_window(*retired), StateError::missing_window);
    rejected(state.map_window(*retired), StateError::missing_window);
    rejected(state.unmap_window(*retired), StateError::missing_window);
    rejected(state.move_window(*retired, 2), StateError::missing_window);
    rejected(state.set_tiling_override(*retired, false), StateError::missing_window);
    rejected(state.destroy_window(*retired), StateError::missing_window);
    rejected(state.mapped_windows(3), StateError::missing_workspace);
}

TEST_F(StateTest, InitialMappingOrderIsIndependentOfCreationAndStableAcrossRemapping) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    auto first = state.create_window(1);
    auto second = state.create_window(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(state.initialize_window(*first));
    ASSERT_TRUE(state.initialize_window(*second));
    ASSERT_TRUE(state.map_window(*second));
    ASSERT_TRUE(state.map_window(*first));
    const auto initial = state.window(*second)->initial_mapping_seq;
    EXPECT_LT(initial, state.window(*first)->initial_mapping_seq);
    ASSERT_TRUE(state.unmap_window(*second));
    ASSERT_TRUE(state.map_window(*second));
    EXPECT_EQ(state.window(*second)->initial_mapping_seq, initial);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, OutputRemovalPreservesWindowsAndLastWorkspaceOwnership) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_output("A"));
    ASSERT_TRUE(state.assign_workspace(1, "A"));
    ASSERT_TRUE(state.show_workspace("A", 1));
    auto created = state.create_window(1);
    ASSERT_TRUE(created);
    ASSERT_TRUE(state.initialize_window(*created));
    ASSERT_TRUE(state.map_window(*created));
    ASSERT_TRUE(state.remove_output("A"));
    EXPECT_EQ(state.output_count(), 0U);
    EXPECT_EQ(state.window_count(), 1U);
    EXPECT_TRUE(state.window(*created)->mapped);
    EXPECT_EQ(state.workspace(1)->owner_or_last_output, "A");
    EXPECT_TRUE(state.valid());
    ASSERT_TRUE(state.add_output("A"));
    EXPECT_FALSE(state.output("A")->shown);
    ASSERT_TRUE(state.show_workspace("A", 1));
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, AWorkspaceCannotBeMirroredAndAssignmentDoesNotInventSelectionPolicy) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_workspace(2));
    ASSERT_TRUE(state.add_output("A"));
    ASSERT_TRUE(state.add_output("B"));
    ASSERT_TRUE(state.assign_workspace(1, "A"));
    ASSERT_TRUE(state.assign_workspace(2, "B"));
    ASSERT_TRUE(state.show_workspace("A", 1));
    ASSERT_TRUE(state.show_workspace("B", 2));
    EXPECT_EQ(state.show_workspace("B", 1).error(), StateError::wrong_owner);
    ASSERT_TRUE(state.assign_workspace(1, "B"));
    EXPECT_FALSE(state.output("A")->shown);
    EXPECT_EQ(state.output("B")->shown, 2U);
    ASSERT_TRUE(state.show_workspace("B", 1));
    EXPECT_EQ(state.workspace(2)->owner_or_last_output, "B");
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, TilingOverrideResetIncludesUnmappedAndOffscreenWindows) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_workspace(2));
    auto first = state.create_window(1);
    auto second = state.create_window(2);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(state.set_tiling_override(*first, true));
    ASSERT_TRUE(state.set_tiling_override(*second, false));
    EXPECT_TRUE(state.window(*first)->tiling_override);
    EXPECT_TRUE(state.window(*second)->tiling_override);
    state.clear_tiling_overrides();
    EXPECT_FALSE(state.window(*first)->tiling_override);
    EXPECT_FALSE(state.window(*second)->tiling_override);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, GeneratedLifecycleSequencesKeepMembershipAndLifetimeInvariants) {
    State state;
    for (seaside::WorkspaceId id = 1; id <= 4; ++id) {
        ASSERT_TRUE(state.add_workspace(id));
    }
    constexpr unsigned seed = 0x5ea51deU;
    RecordProperty("seed", std::to_string(seed));
    SCOPED_TRACE(seed);
    std::mt19937 random(seed);
    std::vector<seaside::WindowId> living;
    std::vector<seaside::WindowId> destroyed;
    for (unsigned step = 0; step < 1000; ++step) {
        SCOPED_TRACE(step);
        const auto action = random() % 5;
        const auto workspace = static_cast<seaside::WorkspaceId>(random() % 4 + 1);
        if (living.empty() || action == 0) {
            auto created = state.create_window(workspace);
            ASSERT_TRUE(created);
            living.push_back(*created);
        } else {
            const auto index = static_cast<std::size_t>(random()) % living.size();
            const auto id = living[index];
            if (action == 1) {
                ASSERT_TRUE(state.move_window(id, workspace));
            } else if (action == 2) {
                ASSERT_TRUE(state.initialize_window(id));
                if (!state.window(id)->mapped) {
                    ASSERT_TRUE(state.map_window(id));
                }
            } else if (action == 3) {
                if (state.window(id)->mapped) {
                    ASSERT_TRUE(state.unmap_window(id));
                }
            } else {
                ASSERT_TRUE(state.destroy_window(id));
                destroyed.push_back(id);
                living.erase(living.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        ASSERT_TRUE(state.valid());
        EXPECT_EQ(state.window_count(), living.size());
        for (const auto id : destroyed) {
            ASSERT_EQ(state.window(id), nullptr);
        }
    }
}
} // namespace

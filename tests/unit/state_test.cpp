#include "state.hpp"

#include <gtest/gtest.h>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
template <class Value, class Error>
void expect_error(const std::expected<Value, Error> &result, Error expected) {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), expected);
}
template <class Record> const Record &checked_record(const Record *record) {
    if (!record) {
        ADD_FAILURE() << "Expected a live state record";
        throw std::logic_error("Required state record disappeared");
    }
    return *record;
}

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
    expect_error(state.add_workspace(0), StateError::invalid_id);
    expect_error(state.add_workspace(2147483648U), StateError::invalid_id);
    ASSERT_TRUE(state.add_workspace(2147483647U));
    expect_error(state.add_workspace(2147483647U), StateError::duplicate);
    expect_error(state.add_output(""), StateError::invalid_id);
    ASSERT_TRUE(state.add_output("output:opaque-1"));
    expect_error(state.add_output("output:opaque-1"), StateError::duplicate);
    expect_error(state.create_window(1), StateError::missing_workspace);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, OnlyInitializedMappedWindowsAreEligibleAndDestroyedIdsStayStale) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    const auto created = state.create_window(1);
    ASSERT_TRUE(created);
    const auto id = created.value();
    expect_error(state.map_window(id), StateError::not_initialized);
    ASSERT_TRUE(state.initialize_window(id));
    ASSERT_TRUE(state.map_window(id));
    expect_error(state.map_window(id), StateError::already_mapped);
    auto mapped = state.mapped_windows(1);
    ASSERT_TRUE(mapped);
    EXPECT_EQ(mapped.value(), std::vector<seaside::WindowId>{id});
    ASSERT_TRUE(state.unmap_window(id));
    EXPECT_NE(state.window(id), nullptr);
    EXPECT_TRUE(state.mapped_windows(1).value().empty());
    expect_error(state.unmap_window(id), StateError::not_mapped);
    ASSERT_TRUE(state.map_window(id));
    ASSERT_TRUE(state.destroy_window(id));
    EXPECT_EQ(state.window(id), nullptr);
    EXPECT_TRUE(checked_record(state.workspace(1)).windows.empty());
    expect_error(state.map_window(id), StateError::missing_window);
    expect_error(state.destroy_window(id), StateError::missing_window);
    auto replacement = state.create_window(1);
    ASSERT_TRUE(replacement);
    EXPECT_GT(replacement.value(), id);
    EXPECT_TRUE(state.valid());
}

TEST_F(StateTest, MovesAreUniqueAndFailedMovesDoNotChangeMembership) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    ASSERT_TRUE(state.add_workspace(2));
    auto created = state.create_window(1);
    ASSERT_TRUE(created);
    const auto id = created.value();
    expect_error(state.move_window(id, 3), StateError::missing_workspace);
    EXPECT_EQ(checked_record(state.window(id)).home_workspace, 1U);
    EXPECT_TRUE(checked_record(state.workspace(1)).windows.contains(id));
    ASSERT_TRUE(state.move_window(id, 2));
    ASSERT_TRUE(state.move_window(id, 2));
    EXPECT_EQ(checked_record(state.window(id)).home_workspace, 2U);
    EXPECT_FALSE(checked_record(state.workspace(1)).windows.contains(id));
    EXPECT_EQ(checked_record(state.workspace(2)).windows.size(), 1U);
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
    ASSERT_TRUE(state.initialize_window(first.value()));
    ASSERT_TRUE(state.map_window(first.value()));
    ASSERT_TRUE(state.destroy_window(retired.value()));

    const auto unchanged = [&] {
        EXPECT_TRUE(state.valid());
        EXPECT_EQ(state.output_count(), 2U);
        EXPECT_EQ(state.window_count(), 2U);
        ASSERT_NE(state.output("A"), nullptr);
        ASSERT_NE(state.output("B"), nullptr);
        EXPECT_EQ(checked_record(state.output("A")).shown, 1U);
        EXPECT_EQ(checked_record(state.output("B")).shown, 2U);
        EXPECT_EQ(checked_record(state.workspace(1)).owner_or_last_output, "A");
        EXPECT_EQ(checked_record(state.workspace(2)).owner_or_last_output, "B");
        EXPECT_EQ(checked_record(state.workspace(1)).windows,
                  (std::set<seaside::WindowId>{first.value()}));
        EXPECT_EQ(checked_record(state.workspace(2)).windows,
                  (std::set<seaside::WindowId>{second.value()}));
        ASSERT_NE(state.window(first.value()), nullptr);
        ASSERT_NE(state.window(second.value()), nullptr);
        EXPECT_EQ(checked_record(state.window(first.value())).home_workspace, 1U);
        EXPECT_TRUE(checked_record(state.window(first.value())).initialized);
        EXPECT_TRUE(checked_record(state.window(first.value())).mapped);
        EXPECT_EQ(checked_record(state.window(second.value())).home_workspace, 2U);
        EXPECT_FALSE(checked_record(state.window(second.value())).initialized);
        EXPECT_FALSE(checked_record(state.window(second.value())).mapped);
        EXPECT_EQ(state.window(retired.value()), nullptr);
    };
    const auto rejected = [&](auto result, StateError error) {
        ASSERT_FALSE(result);
        expect_error(result, error);
        unchanged();
    };
    rejected(state.assign_workspace(3, "A"), StateError::missing_workspace);
    rejected(state.assign_workspace(1, "missing"), StateError::missing_output);
    rejected(state.show_workspace("missing", 1), StateError::missing_output);
    rejected(state.show_workspace("A", 3), StateError::missing_workspace);
    rejected(state.show_workspace("B", 1), StateError::wrong_owner);
    rejected(state.remove_output("missing"), StateError::missing_output);
    rejected(state.create_window(3), StateError::missing_workspace);
    rejected(state.move_window(first.value(), 3), StateError::missing_workspace);
    rejected(state.initialize_window(retired.value()), StateError::missing_window);
    rejected(state.map_window(retired.value()), StateError::missing_window);
    rejected(state.unmap_window(retired.value()), StateError::missing_window);
    rejected(state.move_window(retired.value(), 2), StateError::missing_window);
    rejected(state.set_tiling_override(retired.value(), false), StateError::missing_window);
    rejected(state.destroy_window(retired.value()), StateError::missing_window);
    rejected(state.mapped_windows(3), StateError::missing_workspace);
}

TEST_F(StateTest, InitialMappingOrderIsIndependentOfCreationAndStableAcrossRemapping) {
    State state;
    ASSERT_TRUE(state.add_workspace(1));
    auto first = state.create_window(1);
    auto second = state.create_window(1);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(state.initialize_window(first.value()));
    ASSERT_TRUE(state.initialize_window(second.value()));
    ASSERT_TRUE(state.map_window(second.value()));
    ASSERT_TRUE(state.map_window(first.value()));
    const auto initial = checked_record(state.window(second.value())).initial_mapping_seq;
    EXPECT_LT(initial, checked_record(state.window(first.value())).initial_mapping_seq);
    ASSERT_TRUE(state.unmap_window(second.value()));
    ASSERT_TRUE(state.map_window(second.value()));
    EXPECT_EQ(checked_record(state.window(second.value())).initial_mapping_seq, initial);
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
    ASSERT_TRUE(state.initialize_window(created.value()));
    ASSERT_TRUE(state.map_window(created.value()));
    ASSERT_TRUE(state.remove_output("A"));
    EXPECT_EQ(state.output_count(), 0U);
    EXPECT_EQ(state.window_count(), 1U);
    EXPECT_TRUE(checked_record(state.window(created.value())).mapped);
    EXPECT_EQ(checked_record(state.workspace(1)).owner_or_last_output, "A");
    EXPECT_TRUE(state.valid());
    ASSERT_TRUE(state.add_output("A"));
    EXPECT_FALSE(checked_record(state.output("A")).shown);
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
    expect_error(state.show_workspace("B", 1), StateError::wrong_owner);
    ASSERT_TRUE(state.assign_workspace(1, "B"));
    EXPECT_FALSE(checked_record(state.output("A")).shown);
    EXPECT_EQ(checked_record(state.output("B")).shown, 2U);
    ASSERT_TRUE(state.show_workspace("B", 1));
    EXPECT_EQ(checked_record(state.workspace(2)).owner_or_last_output, "B");
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
    ASSERT_TRUE(state.set_tiling_override(first.value(), true));
    ASSERT_TRUE(state.set_tiling_override(second.value(), false));
    EXPECT_EQ(checked_record(state.window(first.value())).tiling_override, true);
    EXPECT_EQ(checked_record(state.window(second.value())).tiling_override, false);
    state.clear_tiling_overrides();
    EXPECT_FALSE(checked_record(state.window(first.value())).tiling_override);
    EXPECT_FALSE(checked_record(state.window(second.value())).tiling_override);
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
            living.push_back(created.value());
        } else {
            const auto index = static_cast<std::size_t>(random()) % living.size();
            const auto id = living[index];
            if (action == 1) {
                ASSERT_TRUE(state.move_window(id, workspace));
            } else if (action == 2) {
                ASSERT_TRUE(state.initialize_window(id));
                if (!checked_record(state.window(id)).mapped) {
                    ASSERT_TRUE(state.map_window(id));
                }
            } else if (action == 3) {
                if (checked_record(state.window(id)).mapped) {
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

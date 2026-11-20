#ifndef SEASIDE_STATE_HPP
#define SEASIDE_STATE_HPP

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace seaside {
using WindowId = std::uint64_t;
using WorkspaceId = std::uint32_t;
using OutputId = std::string;

enum class StateError {
    invalid_id,
    duplicate,
    missing_window,
    missing_workspace,
    missing_output,
    not_initialized,
    already_mapped,
    not_mapped,
    wrong_owner,
    id_exhausted,
    allocation
};

struct WindowState {
    WindowId id;
    WorkspaceId home_workspace;
    bool initialized = false;
    bool mapped = false;
    std::optional<bool> tiling_override;
    std::uint64_t initial_mapping_seq = 0;
};
struct WorkspaceState {
    WorkspaceId id;
    std::optional<OutputId> owner_or_last_output;
    std::set<WindowId> windows;
};
struct OutputState {
    OutputId id;
    std::optional<WorkspaceId> shown;
};

// Event-loop-owned logical registry. It supplies lifetime/membership primitives,
// not automatic migration, focus, workspace selection or layout policy.
class State final {
  public:
    std::expected<void, StateError> add_output(std::string_view id) noexcept;
    std::expected<void, StateError> remove_output(std::string_view id) noexcept;
    std::expected<void, StateError> add_workspace(WorkspaceId id) noexcept;
    std::expected<void, StateError> assign_workspace(WorkspaceId workspace,
                                                     std::string_view output) noexcept;
    std::expected<void, StateError> show_workspace(std::string_view output,
                                                   WorkspaceId workspace) noexcept;
    std::expected<WindowId, StateError> create_window(WorkspaceId workspace) noexcept;
    std::expected<void, StateError> initialize_window(WindowId window) noexcept;
    std::expected<void, StateError> map_window(WindowId window) noexcept;
    std::expected<void, StateError> unmap_window(WindowId window) noexcept;
    std::expected<void, StateError> destroy_window(WindowId window) noexcept;
    std::expected<void, StateError> move_window(WindowId window, WorkspaceId destination) noexcept;
    std::expected<void, StateError> set_tiling_override(WindowId window,
                                                        std::optional<bool> value) noexcept;
    void clear_tiling_overrides() noexcept;

    // Borrowed views expire when their corresponding record is erased. Retain
    // IDs across asynchronous work, then re-resolve rather than capturing views.
    const WindowState *window(WindowId id) const noexcept;
    const WorkspaceState *workspace(WorkspaceId id) const noexcept;
    const OutputState *output(std::string_view id) const noexcept;
    std::expected<std::vector<WindowId>, StateError> mapped_windows(WorkspaceId id) const noexcept;
    bool valid() const noexcept;
    std::size_t output_count() const noexcept { return outputs_.size(); }
    std::size_t window_count() const noexcept { return windows_.size(); }

  private:
    std::map<WindowId, WindowState> windows_;
    std::map<WorkspaceId, WorkspaceState> workspaces_;
    std::map<OutputId, OutputState, std::less<>> outputs_;
    WindowId next_window_id_ = 1;
    std::uint64_t next_mapping_seq_ = 1;
};

} // namespace seaside
#endif

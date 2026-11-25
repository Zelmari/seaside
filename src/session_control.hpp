#ifndef SEASIDE_SESSION_CONTROL_HPP
#define SEASIDE_SESSION_CONTROL_HPP

#include "ipc.hpp"
#include "live_config.hpp"
#include "output_config.hpp"

namespace seaside {

class Trace;

struct IpcAction {
    Action action;
    std::optional<WindowId> window;
};

IpcAction decode_ipc_action(std::string_view method, const IpcJson &params);
IpcResult desktop_action_result(std::expected<bool, DesktopActionError> result);
IpcJson ipc_diagnostic(const Diagnostic &diagnostic, std::string_view component = "config");
IpcJson ipc_action_result(bool changed);

class SessionControl {
  public:
    SessionControl(Scene &scene, Desktop &desktop, OutputConfig &output_config)
        : scene_(scene), desktop_(desktop), outputs_(output_config) {}
    IpcResult query(std::string_view method, const LiveConfig &config, Trace &trace,
                    std::string_view path, std::string_view phase, const IpcJson &last_error);
    void publish(IpcServer &ipc);
    void set_renderer(const char *renderer) noexcept { renderer_ = renderer; }

  private:
    struct FocusState {
        std::optional<WindowId> window;
        std::optional<WorkspaceId> workspace;
        std::optional<OutputId> output;

        bool operator==(const FocusState &) const = default;
        IpcJson json() const;
    };

    struct WorkspaceState {
        std::optional<OutputId> owner;
        std::optional<OutputId> output;
        bool visible;

        bool operator==(const WorkspaceState &) const = default;
    };

    struct PublishedWindow {
        WorkspaceId workspace;
        WindowFlags flags;
        bool mapped;

        bool operator==(const PublishedWindow &) const = default;
        IpcJson json() const;
    };

    struct OutputState {
        WindowRect geometry;
        int refresh_millihz;
        int rotation;
        float scale;
        bool connected;
        bool enabled;
        bool primary;
        bool sleeping;
        bool adaptive_sync;

        bool operator==(const OutputState &) const = default;
    };
    struct PublishedOutput {
        std::string connector;
        std::string disabled_reason;
        OutputState state{};
    };

    IpcJson focus() const;
    IpcJson workspaces() const;
    IpcJson windows() const;
    IpcJson output_rows() const;
    IpcJson night_light(const ConfigTransaction &config) const;
    IpcJson diagnostics(Trace &trace) const;
    FocusState focus_state() const;
    WorkspaceState workspace_state(WorkspaceId id) const;

    Scene &scene_;
    Desktop &desktop_;
    OutputConfig &outputs_;
    const char *renderer_ = nullptr;
    std::map<WindowId, PublishedWindow> published_windows_;
    std::map<WorkspaceId, WorkspaceState> published_workspaces_;
    std::map<OutputId, PublishedOutput> published_outputs_;
    FocusState published_focus_;
};

} // namespace seaside
#endif

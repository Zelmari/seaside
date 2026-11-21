#ifndef SEASIDE_SESSION_CONTROL_HPP
#define SEASIDE_SESSION_CONTROL_HPP

#include "ipc.hpp"
#include "live_config.hpp"
#include "output_config.hpp"

namespace seaside {

struct IpcAction {
    Action action;
    std::optional<WindowId> window;
    std::optional<WorkspaceId> workspace;
};

IpcAction decode_ipc_action(std::string_view method, const IpcJson &params);
IpcResult desktop_action_result(std::expected<bool, DesktopActionError> result);
IpcJson ipc_diagnostic(const Diagnostic &diagnostic, std::string_view component = "config");
IpcJson ipc_action_result(bool changed);

class SessionControl {
  public:
    SessionControl(Scene &scene, Desktop &desktop, OutputConfig &output_config)
        : scene_(scene), desktop_(desktop), outputs_(output_config) {}
    IpcResult query(std::string_view method, const LiveConfig &config, std::string_view path,
                    std::string_view phase, const IpcJson &last_error);
    void publish(IpcServer &ipc);
    void set_renderer(const char *renderer) noexcept { renderer_ = renderer; }

  private:
    IpcJson focus() const;
    IpcJson workspaces() const;
    IpcJson windows() const;
    IpcJson output_rows() const;
    IpcJson night_light(const ConfigTransaction &config) const;
    IpcJson diagnostics() const;

    Scene &scene_;
    Desktop &desktop_;
    OutputConfig &outputs_;
    const char *renderer_ = nullptr;
    std::map<WindowId, std::pair<WorkspaceId, bool>> published_windows_;
    std::map<WorkspaceId, IpcJson> published_workspaces_;
    std::map<std::string, IpcJson> published_outputs_;
    IpcJson published_focus_ = {
        {"window_id", nullptr}, {"workspace_id", nullptr}, {"output_id", nullptr}};
};

} // namespace seaside
#endif

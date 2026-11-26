#pragma once

#include "cursor_layout.hpp"

#include <cstdint>
#include <expected>
#include <memory>

struct timespec;
struct wl_display;
struct wlr_allocator;
struct wlr_backend;
struct wlr_output;
struct wlr_output_mode;
struct wlr_renderer;
struct wlr_surface;

namespace seaside {

inline constexpr int default_cursor_size = 16;

struct DrmOutputSettings {
    wlr_output_mode *mode;
    int width, height, refresh_millihz;
    int x, y, rotation;
    float scale;
    bool adaptive_sync;
};

struct DrmOutputDescription {
    const char *make, *model;
    int preferred_width, preferred_height, max_refresh_millihz;
    std::int64_t max_pixels;
    bool adaptive_sync_supported;
};

struct DrmOutputStatus {
    DrmOutputSettings settings;
    // A sleeping output can keep a newer accepted mode for its next wake.
    struct {
        int width, height, refresh_millihz;
    } last_mode;
    int logical_width, logical_height;
    bool sleeping;
    bool cursor;
    bool adaptive_sync;
    bool frame_pending;
    std::uint64_t committed_frames, presented_frames, discarded_frames, last_presentation_ns;
};

struct TtyDrmObservation {
    bool active;
    bool failed;
    bool cursor_presented;
    bool output_pending;
    std::uint32_t connector_id;
    std::uint32_t cursor_plane_id;
    std::uint64_t presented_frames;
    std::uint64_t cursor_moves;
    // Page flips with a cursor position different from the previous flip.
    std::uint64_t presented_moves;
};

// The output policy borrows output handles and selected modes for one loop turn.
class DrmOutputControl {
  public:
    virtual ~DrmOutputControl() = default;
    virtual DrmOutputDescription describe_output(wlr_output &output) const = 0;
    virtual bool output_status(wlr_output &output, DrmOutputStatus &status) = 0;
    virtual bool select_mode(wlr_output &output, DrmOutputSettings &settings) const = 0;
    virtual bool check_settings(wlr_output &output, const DrmOutputSettings &settings) const = 0;
    virtual bool apply_settings(wlr_output &output, const DrmOutputSettings &settings) = 0;
    virtual void set_direct_scanout(wlr_output &output, bool enabled) = 0;
    virtual void reject_unrestorable(wlr_output &output) = 0;
    virtual void reconcile_cursor() = 0;
};

class TtyDrmObserver {
  public:
    virtual ~TtyDrmObserver() = default;
    // Handles and strings are borrowed for the duration of the call.
    virtual bool output_added(wlr_output *output, int x, int y, int width, int height) noexcept = 0;
    virtual void output_removed(wlr_output *output) noexcept = 0;
    virtual void output_observed(wlr_output *output, const char *name, const char *identity,
                                 bool connected, const char *disabled_reason) noexcept = 0;
    virtual void cursor_presented(wlr_output *output, const timespec *when) noexcept = 0;
};

struct TtyDrmRendering {
    struct RendererDeleter {
        void operator()(wlr_renderer *renderer) const noexcept;
    };
    struct BackendDeleter {
        void operator()(wlr_backend *backend) const noexcept;
    };
    // The backend must tear down before the renderer.
    std::unique_ptr<wlr_renderer, RendererDeleter> renderer;
    std::unique_ptr<wlr_backend, BackendDeleter> backend;
};

enum class TtyDrmError { invalid_display, invalid_device, allocation, session, backend, renderer };

struct TtyDrmState;

DrmOutputDescription describe_drm_output(wlr_output &output) noexcept;
bool select_drm_mode(wlr_output &output, DrmOutputSettings &settings) noexcept;

// Owns the seat and the pending backend and renderer. Once rendering ownership
// is handed to Server, the seat must outlive Server's backend/display cleanup.
// All calls use the display event-loop thread.
class TtyDrm final : public DrmOutputControl {
  public:
    static bool active_console() noexcept;
    static std::expected<std::unique_ptr<TtyDrm>, TtyDrmError>
    create(wl_display *display, const char *device_path = nullptr) noexcept;
    ~TtyDrm() noexcept override;
    TtyDrm(const TtyDrm &) = delete;
    TtyDrm &operator=(const TtyDrm &) = delete;

    TtyDrmRendering take_rendering() noexcept;

    // Register before enabling outputs and clear before destroying the observer.
    void set_observer(TtyDrmObserver *observer) noexcept;
    // Drain pending output work outside backend and commit callbacks.
    bool enable_pending(wlr_allocator *allocator, wlr_renderer *renderer,
                        wlr_output *preferred) noexcept;
    // First-output qualification fixture. The result is borrowed.
    wlr_output *enable(wlr_allocator *allocator, wlr_renderer *renderer) noexcept;

    DrmOutputDescription describe_output(wlr_output &output) const noexcept override;
    bool output_status(wlr_output &output, DrmOutputStatus &status) noexcept override;
    bool select_mode(wlr_output &output, DrmOutputSettings &settings) const noexcept override;
    bool check_settings(wlr_output &output,
                        const DrmOutputSettings &settings) const noexcept override;
    bool apply_settings(wlr_output &output, const DrmOutputSettings &settings) noexcept override;
    void set_direct_scanout(wlr_output &output, bool enabled) noexcept override;
    void reject_unrestorable(wlr_output &output) noexcept override;
    void reconcile_cursor() noexcept override;

    // Power transitions preserve the logical output, mode selection and workspace.
    bool set_output_power(wlr_output *output, bool on) noexcept;
    // Reject unverified outputs when the first active presentation expires.
    void reject_unpresented_cursors() noexcept;
    // Position and relative deltas use logical output-layout coordinates.
    bool cursor_position(double &x, double &y) const noexcept;
    bool absolute_delta(double x, double y, double &dx, double &dy) const noexcept;
    bool set_cursor_visible(bool visible) noexcept;
    // A null shape restores the arrow. A null surface hides the cursor.
    bool set_cursor_shape(const char *shape) noexcept;
    bool set_cursor_surface(wlr_surface *surface, std::int32_t hotspot_x,
                            std::int32_t hotspot_y) noexcept;
    bool move(seaside_pointer_motion kind, double x, double y) noexcept;
    bool change_vt(unsigned vt) noexcept;
    TtyDrmObservation observation() const noexcept;

  private:
    explicit TtyDrm(std::unique_ptr<TtyDrmState> state) noexcept;
    std::unique_ptr<TtyDrmState> state_;
    TtyDrmRendering rendering_;
};

} // namespace seaside

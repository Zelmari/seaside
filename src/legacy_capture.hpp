#ifndef SEASIDE_LEGACY_CAPTURE_HPP
#define SEASIDE_LEGACY_CAPTURE_HPP

#include <cstdint>
#include <ctime>
#include <memory>

struct wl_display;
struct wlr_buffer;
struct wlr_output;

namespace seaside {
struct LegacyCaptureState;

// Output rendering and cursor composition belong to the observer.
class LegacyCaptureObserver {
  public:
    virtual bool supports_legacy_capture(wlr_output *output) const noexcept = 0;
    virtual void legacy_capture_active(wlr_output *output, bool active) noexcept = 0;

  protected:
    ~LegacyCaptureObserver() = default;
};

struct LegacyCaptureDeleter {
    void operator()(LegacyCaptureState *) const noexcept;
};

using LegacyCapture = std::unique_ptr<LegacyCaptureState, LegacyCaptureDeleter>;
LegacyCapture create_legacy_capture(wl_display *display, LegacyCaptureObserver &observer) noexcept;
void legacy_capture_output_lost(LegacyCaptureState *capture, wlr_output *output) noexcept;
bool legacy_capture_wants_cursor(const LegacyCaptureState *capture, wlr_output *output) noexcept;

// cursor_buffer may reuse buffer when the cursor is hidden, or be null if
// offscreen composition failed. content_revision tracks scene changes.
void legacy_capture_present(LegacyCaptureState *capture, wlr_output *output, wlr_buffer *buffer,
                            wlr_buffer *cursor_buffer, const std::timespec &when,
                            std::uint64_t content_revision) noexcept;

} // namespace seaside

#endif

#ifndef SEASIDE_OUTPUT_CAPTURE_HPP
#define SEASIDE_OUTPUT_CAPTURE_HPP

#include <expected>
#include <memory>
#include <sys/types.h>

struct wl_display;
struct wlr_ext_image_copy_capture_manager_v1;
struct wlr_output;

namespace seaside {
namespace detail {
struct OutputCaptureState;
}

enum class OutputCaptureError {
    invalid_display,
    allocation,
    layout_creation,
    source_manager_creation,
    copy_manager_creation,
    legacy_manager_creation,
};

// Installs display-wide output image-source and image-copy globals. All calls
// belong to the display event-loop thread.
class OutputCapture final {
  public:
    static std::expected<OutputCapture, OutputCaptureError> create(wl_display *display) noexcept;
    ~OutputCapture();
    OutputCapture(OutputCapture &&) noexcept;
    OutputCapture &operator=(OutputCapture &&) noexcept;
    OutputCapture(const OutputCapture &) = delete;
    OutputCapture &operator=(const OutputCapture &) = delete;

    // The shared image-copy manager is owned by the display.
    wlr_ext_image_copy_capture_manager_v1 *copy_manager() const noexcept;
    // Removing an output stops its capture sessions. Global bindings survive.
    bool add_output(wlr_output *output, int x, int y) noexcept;
    bool position(wlr_output *output, int x, int y) noexcept;
    void remove_output(wlr_output *output) noexcept;
    // A hardware cursor update may leave the underlying scene undamaged.
    void cursor_changed() noexcept;

    // Match a live client with the peer credentials and capture bindings on
    // this display, including an output binding when an output is enabled.
    bool client_ready(pid_t pid, uid_t uid) const noexcept;

  private:
    explicit OutputCapture(std::unique_ptr<detail::OutputCaptureState> state) noexcept;
    std::unique_ptr<detail::OutputCaptureState> state_;
};

} // namespace seaside

#endif

#ifndef SEASIDE_SESSION_ENVIRONMENT_HPP
#define SEASIDE_SESSION_ENVIRONMENT_HPP

namespace seaside {
// Entry point of the transient user service. Its stdin is the compositor's lifetime pipe.
int run_session_environment(const char *wayland, const char *display, const char *authority,
                            const char *cursor_size);
} // namespace seaside
#endif

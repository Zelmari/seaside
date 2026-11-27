#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static constexpr uint32_t pointer_events =
    XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_POINTER_MOTION |
    XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE;

static xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
    const xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(connection, false, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, NULL);
    const xcb_atom_t value = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    free(reply);
    return value;
}

static bool succeeded(xcb_connection_t *connection, xcb_void_cookie_t cookie) {
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (!error) {
        return xcb_connection_has_error(connection) == 0;
    }
    fprintf(stderr, "X11 request failed with error %u\n", error->error_code);
    free(error);
    return false;
}

static bool update_color(xcb_connection_t *connection, const xcb_screen_t *screen,
                         xcb_window_t window, uint32_t rgb) {
    const uint16_t red = ((rgb >> 16) & 255U) * 257U;
    const uint16_t green = ((rgb >> 8) & 255U) * 257U;
    const uint16_t blue = (rgb & 255U) * 257U;
    const xcb_alloc_color_cookie_t cookie =
        xcb_alloc_color(connection, screen->default_colormap, red, green, blue);
    xcb_alloc_color_reply_t *reply = xcb_alloc_color_reply(connection, cookie, NULL);
    if (!reply) {
        return false;
    }
    const uint32_t pixel = reply->pixel;
    free(reply);
    return succeeded(connection, xcb_change_window_attributes_checked(connection, window,
                                                                      XCB_CW_BACK_PIXEL, &pixel)) &&
           succeeded(connection, xcb_clear_area_checked(connection, false, window, 0, 0, 0, 0));
}

static void log_event(const xcb_generic_event_t *event, uint8_t type) {
    switch (type) {
    case XCB_MAP_NOTIFY:
        puts("map");
        break;
    case XCB_UNMAP_NOTIFY:
        puts("unmap");
        break;
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT:
        puts(type == XCB_FOCUS_IN ? "focus-in" : "focus-out");
        break;
    case XCB_ENTER_NOTIFY:
    case XCB_LEAVE_NOTIFY: {
        const auto *crossing = reinterpret_cast<const xcb_enter_notify_event_t *>(event);
        printf("pointer-%s %u %d %d root=%d,%d mode=%u\n",
               type == XCB_ENTER_NOTIFY ? "enter" : "leave", crossing->time, crossing->event_x,
               crossing->event_y, crossing->root_x, crossing->root_y, crossing->mode);
        break;
    }
    case XCB_MOTION_NOTIFY: {
        const auto *motion = reinterpret_cast<const xcb_motion_notify_event_t *>(event);
        printf("pointer-motion %u %d %d root=%d,%d\n", motion->time, motion->event_x,
               motion->event_y, motion->root_x, motion->root_y);
        break;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
        const auto *button = reinterpret_cast<const xcb_button_press_event_t *>(event);
        printf("pointer-button %u %u %s %d %d\n", button->time, button->detail,
               type == XCB_BUTTON_PRESS ? "press" : "release", button->event_x, button->event_y);
        break;
    }
    case XCB_CONFIGURE_NOTIFY: {
        const auto *configure = reinterpret_cast<const xcb_configure_notify_event_t *>(event);
        printf("configure %d %d %u %u\n", configure->x, configure->y, configure->width,
               configure->height);
        break;
    }
    default:
        break;
    }
}

static bool done(xcb_connection_t *connection, xcb_atom_t protocols, xcb_atom_t close_atom,
                 bool pointer_probe) {
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(connection))) {
        const uint8_t type = event->response_type & 0x7f;
        const xcb_client_message_event_t *message = (const xcb_client_message_event_t *)event;
        const bool close_requested = type == XCB_CLIENT_MESSAGE && message->type == protocols &&
                                     message->format == 32 && message->data.data32[0] == close_atom;
        if (pointer_probe) {
            log_event(event, type);
        }
        free(event);
        if (close_requested) {
            return true;
        }
    }
    return false;
}

static bool set_fullscreen(xcb_connection_t *connection, const xcb_screen_t *screen,
                           xcb_window_t window, bool fullscreen) {
    xcb_client_message_event_t message{};
    message.response_type = XCB_CLIENT_MESSAGE;
    message.format = 32;
    message.window = window;
    message.type = atom(connection, "_NET_WM_STATE");
    message.data.data32[0] = fullscreen ? 1 : 0;
    message.data.data32[1] = atom(connection, "_NET_WM_STATE_FULLSCREEN");
    message.data.data32[3] = 1;
    printf("ewmh-fullscreen %u\n", fullscreen);
    return succeeded(connection, xcb_send_event_checked(connection, false, screen->root,
                                                        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                                                            XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                                                        reinterpret_cast<const char *>(&message)));
}

static bool hide_cursor(xcb_connection_t *connection, xcb_window_t window) {
    auto *version = xcb_xfixes_query_version_reply(
        connection, xcb_xfixes_query_version(connection, 5, 0), nullptr);
    if (!version || version->major_version < 4) {
        free(version);
        puts("xfixes-hide unsupported");
        return false;
    }
    free(version);
    const bool hidden = succeeded(connection, xcb_xfixes_hide_cursor_checked(connection, window));
    printf("xfixes-hide %s\n", hidden ? "ok" : "failed");
    return hidden;
}

static void grab_pointer(xcb_connection_t *connection, xcb_window_t window, bool relative) {
    xcb_generic_error_t *error = nullptr;
    auto *reply = xcb_grab_pointer_reply(connection,
                                         xcb_grab_pointer(connection, true, window, pointer_events,
                                                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                                                          window, XCB_NONE, XCB_CURRENT_TIME),
                                         &error);
    printf("grab %s status=%d error=%u\n", relative ? "relative" : "confine",
           reply ? reply->status : -1, error ? error->error_code : 0);
    const bool acquired = reply && reply->status == XCB_GRAB_STATUS_SUCCESS;
    free(reply);
    free(error);
    if (relative && acquired) {
        auto *geometry =
            xcb_get_geometry_reply(connection, xcb_get_geometry(connection, window), nullptr);
        if (geometry) {
            const int16_t x = geometry->width / 2;
            const int16_t y = geometry->height / 2;
            const bool warped =
                succeeded(connection,
                          xcb_warp_pointer_checked(connection, XCB_NONE, window, 0, 0, 0, 0, x, y));
            printf("warp %s %d %d\n", warped ? "ok" : "failed", x, y);
        }
        free(geometry);
    }
}

static bool handle_probe_command(xcb_connection_t *connection, const xcb_screen_t *screen,
                                 xcb_window_t window, const char *line, bool &hidden) {
    if (strcmp(line, "grab\n") == 0 || strcmp(line, "relative\n") == 0) {
        const bool relative = strcmp(line, "relative\n") == 0;
        if (!relative || hidden || (hidden = hide_cursor(connection, window))) {
            grab_pointer(connection, window, relative);
        }
    } else if (strcmp(line, "ungrab\n") == 0) {
        const bool ok =
            succeeded(connection, xcb_ungrab_pointer_checked(connection, XCB_CURRENT_TIME));
        printf("ungrab %s\n", ok ? "ok" : "failed");
        if (hidden && succeeded(connection, xcb_xfixes_show_cursor_checked(connection, window))) {
            hidden = false;
            puts("xfixes-show ok");
        }
    } else if (strcmp(line, "fullscreen\n") == 0 || strcmp(line, "windowed\n") == 0) {
        set_fullscreen(connection, screen, window, strcmp(line, "fullscreen\n") == 0);
    } else {
        return false;
    }
    return true;
}

static bool serve(xcb_connection_t *connection, const xcb_screen_t *screen, xcb_window_t window,
                  xcb_atom_t protocols, xcb_atom_t close_atom, bool pointer_probe) {
    struct pollfd watched[] = {
        {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0},
        {.fd = xcb_get_file_descriptor(connection), .events = POLLIN, .revents = 0}};
    bool hidden = false;
    while (!xcb_connection_has_error(connection)) {
        if (done(connection, protocols, close_atom, pointer_probe)) {
            return true;
        }
        if (poll(watched, sizeof(watched) / sizeof(watched[0]), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (watched[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return false;
        }
        if (!(watched[0].revents & (POLLIN | POLLHUP))) {
            continue;
        }
        char line[32];
        if (!fgets(line, sizeof(line), stdin) || strcmp(line, "quit\n") == 0) {
            return true;
        }
        if (pointer_probe && handle_probe_command(connection, screen, window, line, hidden)) {
            continue;
        }
        if (strlen(line) == 13 && line[12] == '\n' && strncmp(line, "color ", 6) == 0 &&
            strspn(line + 6, "0123456789abcdefABCDEF") == 6) {
            const uint32_t rgb = (uint32_t)strtoul(line + 6, NULL, 16);
            if (update_color(connection, screen, window, rgb)) {
                printf("COLOR %06" PRIx32 "\n", rgb);
                fflush(stdout);
                continue;
            }
        }
        fputs("Expected color RRGGBB, grab, relative, ungrab, fullscreen, windowed or quit\n",
              stderr);
        return false;
    }
    return false;
}

int main(int argc, char **argv) {
    const char *title = "Seaside Part 7 X11 capture";
    bool pointer_probe = false;
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pointer-probe") == 0) {
            pointer_probe = true;
        } else if (strcmp(argv[i], "--fullscreen") == 0) {
            fullscreen = true;
        } else if (strncmp(argv[i], "--title=", sizeof("--title=") - 1) == 0) {
            title = argv[i] + sizeof("--title=") - 1;
        } else {
            fputs("Usage: seaside-part7-x11-client [--pointer-probe] [--fullscreen] "
                  "[--title=TITLE]\n",
                  stderr);
            return EXIT_FAILURE;
        }
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char window_class[] = "seaside-part7-x11\0SeasidePart7X11";
    int screen_index = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &screen_index);
    if (!connection || xcb_connection_has_error(connection)) {
        fputs("Could not open the session X11 display\n", stderr);
        if (connection) {
            xcb_disconnect(connection);
        }
        return EXIT_FAILURE;
    }

    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int index = 0; index < screen_index && screens.rem; ++index) {
        xcb_screen_next(&screens);
    }
    const xcb_screen_t *screen = screens.data;
    if (!screens.rem) {
        xcb_disconnect(connection);
        return EXIT_FAILURE;
    }
    const xcb_window_t window = xcb_generate_id(connection);
    const xcb_atom_t name = atom(connection, "_NET_WM_NAME");
    const xcb_atom_t encoding = atom(connection, "UTF8_STRING");
    const xcb_atom_t protocols = atom(connection, "WM_PROTOCOLS");
    const xcb_atom_t close_atom = atom(connection, "WM_DELETE_WINDOW");
    uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    if (pointer_probe) {
        events |= pointer_events | XCB_EVENT_MASK_FOCUS_CHANGE;
    }
    const uint32_t attributes[] = {screen->black_pixel, events};
    bool ok =
        name && encoding && protocols && close_atom &&
        succeeded(connection, xcb_create_window_checked(
                                  connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 256,
                                  144, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attributes));
    if (ok) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 8, strlen(title), title);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, name, encoding, 8,
                            strlen(title), title);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING, 8, sizeof(window_class), window_class);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, protocols, XCB_ATOM_ATOM, 32,
                            1, &close_atom);
        ok = update_color(connection, screen, window, 0x2496D0) &&
             succeeded(connection, xcb_map_window_checked(connection, window));
        if (ok && fullscreen) {
            ok = set_fullscreen(connection, screen, window, true);
        }
    }
    if (ok) {
        // The compositor's mapped-window event is the Wayland-side readiness check.
        printf("READY %" PRIu32 "\n", window);
        fflush(stdout);
        // Keep poll in step with the command stream when several lines arrive together.
        setvbuf(stdin, NULL, _IONBF, 0);
        ok = serve(connection, screen, window, protocols, close_atom, pointer_probe);
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
    }
    xcb_disconnect(connection);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

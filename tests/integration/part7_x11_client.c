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

static xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
    const xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(connection, false, (uint16_t)strlen(name), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection, cookie, NULL);
    const xcb_atom_t value = reply ? reply->atom : XCB_ATOM_NONE;
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

static bool done(xcb_connection_t *connection, xcb_atom_t protocols, xcb_atom_t close_atom) {
    xcb_generic_event_t *event;
    while ((event = xcb_poll_for_event(connection))) {
        const uint8_t type = event->response_type & 0x7f;
        const xcb_client_message_event_t *message = (const xcb_client_message_event_t *)event;
        const bool close_requested = type == XCB_CLIENT_MESSAGE && message->type == protocols &&
                                     message->format == 32 && message->data.data32[0] == close_atom;
        free(event);
        if (close_requested) {
            return true;
        }
    }
    return false;
}

static bool serve(xcb_connection_t *connection, const xcb_screen_t *screen, xcb_window_t window,
                  xcb_atom_t protocols, xcb_atom_t close_atom) {
    struct pollfd watched[] = {{.fd = STDIN_FILENO, .events = POLLIN},
                               {.fd = xcb_get_file_descriptor(connection), .events = POLLIN}};
    while (!xcb_connection_has_error(connection)) {
        if (done(connection, protocols, close_atom)) {
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
        if (strlen(line) == 13 && line[12] == '\n' && strncmp(line, "color ", 6) == 0 &&
            strspn(line + 6, "0123456789abcdefABCDEF") == 6) {
            const uint32_t rgb = (uint32_t)strtoul(line + 6, NULL, 16);
            if (update_color(connection, screen, window, rgb)) {
                printf("COLOR %06" PRIx32 "\n", rgb);
                fflush(stdout);
                continue;
            }
        }
        fputs("Expected color RRGGBB or quit\n", stderr);
        return false;
    }
    return false;
}

int main(void) {
    const char title[] = "Seaside Part 7 X11 capture";
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
    const uint32_t attributes[] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    bool ok =
        name && encoding && protocols && close_atom &&
        succeeded(connection, xcb_create_window_checked(
                                  connection, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 256,
                                  144, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attributes));
    if (ok) {
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NAME,
                            XCB_ATOM_STRING, 8, sizeof(title) - 1, title);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, name, encoding, 8,
                            sizeof(title) - 1, title);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_CLASS,
                            XCB_ATOM_STRING, 8, sizeof(window_class), window_class);
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, protocols, XCB_ATOM_ATOM, 32,
                            1, &close_atom);
        ok = update_color(connection, screen, window, 0x2496D0) &&
             succeeded(connection, xcb_map_window_checked(connection, window));
    }
    if (ok) {
        // The compositor's mapped-window event is the Wayland-side readiness check.
        printf("READY %" PRIu32 "\n", window);
        fflush(stdout);
        // Keep poll in step with the command stream when several lines arrive together.
        setvbuf(stdin, NULL, _IONBF, 0);
        ok = serve(connection, screen, window, protocols, close_atom);
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
    }
    xcb_disconnect(connection);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

/* Simulated metadata only. Reuse the report/classification functions without
 * replacing libdrm or treating this fixture as a successful hardware probe. */
#define main seaside_probe_application_main
#include "../../tools/probe-drm.cpp"
#undef main

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "report-fixture: %s\n", message);
        exit(1);
    }
}

int main(void) {
    require(query_failure(EACCES) == OBS_BLOCKED, "denied access is blocked");
    require(query_failure(ENODEV) == OBS_BLOCKED, "removed device is blocked");
    require(query_failure(ENOTTY) == OBS_BLOCKED, "unavailable KMS query is blocked");
    require(query_failure(EIO) == OBS_ERROR, "I/O failure is a probe error");
    require(query_failure(ENOMEM) == OBS_ERROR, "allocation failure is a probe error");
    require(card_basename("/dev/dri/card17"), "numeric primary node accepted");
    require(!card_basename("/dev/dri/card0-DP-1"), "connector name excluded");
    require(!card_basename("/dev/dri/renderD128"), "render node excluded");

    const std::unique_ptr<device_info> storage{new (std::nothrow) device_info{}};
    auto *device = storage.get();
    require(device != NULL, "allocate snapshot");
    int pipe_fds[2];
    require(pipe(pipe_fds) == 0, "create non-device inherited fd");
    int original_flags = fcntl(pipe_fds[0], F_GETFD);
    inspect_borrowed_fd(device, pipe_fds[0], false);
    require(device->observation == OBS_ERROR, "non-DRM fd rejected");
    require(fcntl(pipe_fds[0], F_GETFD) == original_flags, "borrowed fd retained unchanged");
    close(pipe_fds[0]);
    close(pipe_fds[1]);

    memset(device, 0, sizeof(*device));
    snprintf(device->path, sizeof(device->path), "simulated:\"driver\\path\n");
    snprintf(device->driver, sizeof(device->driver), "fixture\t\"driver");
    device->universal_prepared = true;
    device->enumerated = true;
    device->observation = OBS_COMPLETE;
    device->crtc_count = 32;
    for (size_t i = 0; i < device->crtc_count; ++i)
        device->crtcs[i].id = 100 + i * 7;
    device->encoder_count = 1;
    device->encoders[0] = encoder_info{
        .id = 51,
        .crtc_id = 107,
        .possible_crtcs = UINT32_C(0x80000002),
    };
    device->connector_count = 1;
    device->connectors[0] = connector_info{
        .id = 61,
        .type = DRM_MODE_CONNECTOR_DisplayPort,
        .type_id = 1,
        .encoder_id = 51,
        .current_crtc_id = 107,
        .possible_crtcs = UINT32_C(0x80000002),
        .connection = DRM_MODE_CONNECTED,
    };
    device->plane_count = 2;
    device->planes[0] = plane_info{
        .id = 71,
        .crtc_id = 107,
        .fb_id = 81,
        .possible_crtcs = 2,
        .format_count = 1,
        .type_known = true,
        .type = DRM_PLANE_TYPE_CURSOR,
        .property_count = 1,
        .properties = {},
    };
    device->planes[0].properties[0] = property_info{
        .name = {},
        .id = 91,
        .flags = DRM_MODE_PROP_ENUM,
        .value = DRM_PLANE_TYPE_CURSOR,
    };
    snprintf(device->planes[0].properties[0].name, sizeof(device->planes[0].properties[0].name),
             "type");
    device->planes[1] = plane_info{
        .id = 72,
        .crtc_id = 0,
        .fb_id = 0,
        .possible_crtcs = UINT32_C(0x80000000),
        .format_count = 0,
        .type_known = true,
        .type = DRM_PLANE_TYPE_PRIMARY,
        .property_count = 0,
        .properties = {},
    };
    device->cursor_width_known = true;
    device->cursor_width = 64;
    fputs("{\"simulated\":true,\"hardware_cursor_qualified\":false,"
          "\"actual_cursor_use_tested\":false,\"devices\":[",
          stdout);
    report_device(stdout, device);

    /* A complete universal-plane observation with no cursor is distinct from
     * an unprepared fd or failed query that merely did not expose one. */
    device->plane_count = 0;
    fputc(',', stdout);
    report_device(stdout, device);
    device->universal_prepared = false;
    fail_device(device, OBS_BLOCKED, "simulated: client caps unknown", 0);
    fputc(',', stdout);
    report_device(stdout, device);
    device->universal_prepared = true;
    device->enumerated = false;
    fail_device(device, OBS_BLOCKED, "simulated: no device access", EACCES);
    fputc(',', stdout);
    report_device(stdout, device);
    fail_device(device, OBS_ERROR, "simulated: query failed", EIO);
    fputc(',', stdout);
    report_device(stdout, device);
    fputs("]}\n", stdout);
    return fflush(stdout) == 0 && !ferror(stdout) ? 0 : 1;
}

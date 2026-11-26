#include "../src/storage_fd.hpp"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <limits.h>
#include <memory>
#include <new>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

/* Development evidence only: a plane and its bindings cannot qualify a cursor.
 * In particular, never open a primary node here: even O_RDONLY can implicitly
 * acquire DRM master. An authorized supervisor may lend an existing nonmaster
 * descriptor, whose client capabilities we also leave untouched. */
enum {
    MAX_DEVICES = 64,
    MAX_CRTCS = 32,
    MAX_CONNECTORS = 128,
    MAX_ENCODERS = 128,
    MAX_PLANES = 256,
    MAX_PROPERTIES = 64,
    EXIT_UNAVAILABLE = 77,
};

enum observation { OBS_COMPLETE, OBS_BLOCKED, OBS_ERROR };

struct crtc_info {
    uint32_t id, width, height;
    bool mode_valid;
};

struct encoder_info {
    uint32_t id, crtc_id, possible_crtcs;
};

struct connector_info {
    uint32_t id, type, type_id, encoder_id, current_crtc_id, possible_crtcs;
    drmModeConnection connection;
};

struct property_info {
    char name[DRM_PROP_NAME_LEN + 1];
    uint32_t id, flags;
    uint64_t value;
};

struct plane_info {
    uint32_t id, crtc_id, fb_id, possible_crtcs, format_count;
    bool type_known;
    uint64_t type;
    size_t property_count;
    struct property_info properties[MAX_PROPERTIES];
};

struct device_info {
    char path[PATH_MAX];
    char sysfs_device[PATH_MAX];
    char sysfs_driver[PATH_MAX];
    char driver[256];
    unsigned int device_major, device_minor;
    bool identity_known, universal_prepared, enumerated;
    enum observation observation;
    char reason[256];
    int error_number;
    bool cursor_width_known, cursor_height_known;
    uint64_t cursor_width, cursor_height;
    size_t crtc_count, encoder_count, connector_count, plane_count;
    struct crtc_info crtcs[MAX_CRTCS];
    struct encoder_info encoders[MAX_ENCODERS];
    struct connector_info connectors[MAX_CONNECTORS];
    struct plane_info planes[MAX_PLANES];
};

static const char *observation_name(enum observation value) {
    switch (value) {
    case OBS_COMPLETE:
        return "complete";
    case OBS_BLOCKED:
        return "blocked";
    case OBS_ERROR:
        return "error";
    }
    return "error";
}

static void fail_device(struct device_info *device, enum observation observation,
                        const char *reason, int error_number) {
    device->observation = observation;
    device->error_number = error_number;
    snprintf(device->reason, sizeof(device->reason), "%s", reason);
}

static enum observation query_failure(int error_number) {
    switch (error_number) {
    case EACCES:
    case EPERM:
    case EBUSY:
    case ENODEV:
    case ENOENT:
    case ENXIO:
    case ENOTSUP:
    case ENOTTY:
        return OBS_BLOCKED;
    default:
        return OBS_ERROR;
    }
}

static void fail_query(struct device_info *device, const char *operation) {
    int saved = errno ? errno : EIO;
    fail_device(device, query_failure(saved), operation, saved);
}

static void json_string(FILE *out, const char *value) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 0x20 || *p >= 0x7f) {
            /* Device/driver names are metadata, not arbitrary UTF-8 input. */
            fprintf(out, "\\u%04x", (unsigned int)*p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void json_optional_string(FILE *out, const char *value) {
    if (*value)
        json_string(out, value);
    else
        fputs("null", out);
}

static void json_crtc_ids(FILE *out, const struct device_info *device, uint32_t mask) {
    bool separator = false;
    fputc('[', out);
    /* DRM masks use resource-array indices, not the numeric CRTC object IDs. */
    for (size_t i = 0; i < device->crtc_count; ++i) {
        if (!(mask & (UINT32_C(1) << i)))
            continue;
        fprintf(out, "%s%" PRIu32, separator ? "," : "", device->crtcs[i].id);
        separator = true;
    }
    fputc(']', out);
}

static size_t cursor_plane_count(const struct device_info *device) {
    size_t count = 0;
    for (size_t i = 0; i < device->plane_count; ++i) {
        if (device->planes[i].type_known && device->planes[i].type == DRM_PLANE_TYPE_CURSOR)
            ++count;
    }
    return count;
}

static uint32_t cursor_crtcs(const struct device_info *device) {
    uint32_t mask = 0;
    for (size_t i = 0; i < device->plane_count; ++i) {
        if (device->planes[i].type_known && device->planes[i].type == DRM_PLANE_TYPE_CURSOR)
            mask |= device->planes[i].possible_crtcs;
    }
    return mask;
}

static const char *cursor_capability(const struct device_info *device) {
    if (!device->enumerated)
        return "unknown";
    if (cursor_plane_count(device))
        return "cursor_planes_observed";
    if (device->observation == OBS_COMPLETE && device->universal_prepared)
        return "no_cursor_planes_observed";
    return "unknown";
}

static const char *connection_name(drmModeConnection connection) {
    switch (connection) {
    case DRM_MODE_CONNECTED:
        return "connected";
    case DRM_MODE_DISCONNECTED:
        return "disconnected";
    default:
        return "unknown";
    }
}

static const char *plane_type(const struct plane_info *plane) {
    if (!plane->type_known)
        return "unknown";
    switch (plane->type) {
    case DRM_PLANE_TYPE_CURSOR:
        return "cursor";
    case DRM_PLANE_TYPE_PRIMARY:
        return "primary";
    case DRM_PLANE_TYPE_OVERLAY:
        return "overlay";
    default:
        return "unknown";
    }
}

static void report_device(FILE *out, const struct device_info *device) {
    fputs("{\"path\":", out);
    json_string(out, device->path);
    fprintf(out, ",\"status\":\"%s\",\"reason\":", observation_name(device->observation));
    json_optional_string(out, device->reason);
    fprintf(out, ",\"errno\":%d,\"device\":", device->error_number);
    if (device->identity_known)
        fprintf(out, "{\"major\":%u,\"minor\":%u}", device->device_major, device->device_minor);
    else
        fputs("null", out);
    fputs(",\"sysfs_device\":", out);
    json_optional_string(out, device->sysfs_device);
    fputs(",\"sysfs_driver\":", out);
    json_optional_string(out, device->sysfs_driver);
    fputs(",\"drm_driver\":", out);
    json_optional_string(out, device->driver);
    fprintf(out,
            ",\"universal_planes\":\"%s\",\"enumerated\":%s,"
            "\"cursor_capability\":\"%s\",\"cursor_plane_count\":",
            device->universal_prepared ? "caller_asserted_prepared" : "unknown",
            device->enumerated ? "true" : "false", cursor_capability(device));
    if (device->enumerated)
        fprintf(out, "%zu", cursor_plane_count(device));
    else
        fputs("null", out);
    fputs(",\"cursor_size_caps\":{\"width\":", out);
    if (device->cursor_width_known)
        fprintf(out, "%" PRIu64, device->cursor_width);
    else
        fputs("null", out);
    fputs(",\"height\":", out);
    if (device->cursor_height_known)
        fprintf(out, "%" PRIu64, device->cursor_height);
    else
        fputs("null", out);
    fputs("},\"crtcs\":[", out);
    for (size_t i = 0; i < device->crtc_count; ++i) {
        const struct crtc_info *crtc = &device->crtcs[i];
        fprintf(out,
                "%s{\"id\":%" PRIu32 ",\"index\":%zu,\"mode_valid\":%s,"
                "\"width\":%" PRIu32 ",\"height\":%" PRIu32 "}",
                i ? "," : "", crtc->id, i, crtc->mode_valid ? "true" : "false", crtc->width,
                crtc->height);
    }
    fputs("],\"encoders\":[", out);
    for (size_t i = 0; i < device->encoder_count; ++i) {
        const struct encoder_info *encoder = &device->encoders[i];
        fprintf(out, "%s{\"id\":%" PRIu32 ",\"current_crtc_id\":%" PRIu32 ",\"possible_crtc_ids\":",
                i ? "," : "", encoder->id, encoder->crtc_id);
        json_crtc_ids(out, device, encoder->possible_crtcs);
        fputc('}', out);
    }
    fputs("],\"connectors\":[", out);
    for (size_t i = 0; i < device->connector_count; ++i) {
        const struct connector_info *connector = &device->connectors[i];
        fprintf(out,
                "%s{\"id\":%" PRIu32 ",\"type\":%" PRIu32 ",\"type_id\":%" PRIu32
                ",\"connection\":\"%s\","
                "\"current_encoder_id\":%" PRIu32 ",\"current_crtc_id\":%" PRIu32
                ",\"possible_crtc_ids\":",
                i ? "," : "", connector->id, connector->type, connector->type_id,
                connection_name(connector->connection), connector->encoder_id,
                connector->current_crtc_id);
        json_crtc_ids(out, device, connector->possible_crtcs);
        fputs(",\"candidate_cursor_crtc_ids\":", out);
        json_crtc_ids(out, device, connector->possible_crtcs & cursor_crtcs(device));
        fputc('}', out);
    }
    fputs("],\"planes\":[", out);
    for (size_t i = 0; i < device->plane_count; ++i) {
        const struct plane_info *plane = &device->planes[i];
        fprintf(out,
                "%s{\"id\":%" PRIu32 ",\"type\":\"%s\","
                "\"current_crtc_id\":%" PRIu32 ",\"current_fb_id\":%" PRIu32
                ",\"format_count\":%" PRIu32 ",\"possible_crtc_ids\":",
                i ? "," : "", plane->id, plane_type(plane), plane->crtc_id, plane->fb_id,
                plane->format_count);
        json_crtc_ids(out, device, plane->possible_crtcs);
        fputs(",\"properties\":[", out);
        for (size_t j = 0; j < plane->property_count; ++j) {
            const struct property_info *property = &plane->properties[j];
            fprintf(out, "%s{\"id\":%" PRIu32 ",\"name\":", j ? "," : "", property->id);
            json_string(out, property->name);
            fprintf(out, ",\"flags\":%" PRIu32 ",\"value\":%" PRIu64 "}", property->flags,
                    property->value);
        }
        fputs("]}", out);
    }
    fputs("]}", out);
}

static void read_identity(struct device_info *device, const struct stat *st) {
    char sysfs_path[128], resolved[PATH_MAX];
    device->identity_known = true;
    device->device_major = major(st->st_rdev);
    device->device_minor = minor(st->st_rdev);
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/char/%u:%u/device", device->device_major,
             device->device_minor);
    if (realpath(sysfs_path, resolved))
        snprintf(device->sysfs_device, sizeof(device->sysfs_device), "%s", resolved);
    snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/char/%u:%u/device/driver",
             device->device_major, device->device_minor);
    if (realpath(sysfs_path, resolved)) {
        const char *name = strrchr(resolved, '/');
        snprintf(device->sysfs_driver, sizeof(device->sysfs_driver), "%s",
                 name ? name + 1 : resolved);
    }
}

static bool primary_identity(const struct stat *st) {
    return S_ISCHR(st->st_mode) && drmGetNodeTypeFromDevId(st->st_rdev) == DRM_NODE_PRIMARY;
}

static void discover_device(struct device_info *device, const char *path) {
    struct stat st;
    snprintf(device->path, sizeof(device->path), "%s", path);
    if (stat(path, &st) < 0) {
        fail_query(device, "cannot stat device node");
        return;
    }
    if (!primary_identity(&st)) {
        fail_device(device, OBS_ERROR, "not a Linux DRM primary device node", EINVAL);
        return;
    }
    read_identity(device, &st);
    fail_device(device, OBS_BLOCKED,
                "safe enumeration requires an inherited nonmaster primary fd; "
                "opening a primary node can implicitly acquire DRM master",
                0);
}

static const struct encoder_info *find_encoder(const struct device_info *device, uint32_t id) {
    for (size_t i = 0; i < device->encoder_count; ++i)
        if (device->encoders[i].id == id)
            return &device->encoders[i];
    return NULL;
}

static bool collect_planes(int fd, struct device_info *device) {
    const std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)> resources(
        drmModeGetPlaneResources(fd), drmModeFreePlaneResources);
    if (!resources) {
        fail_query(device, "cannot enumerate DRM planes");
        return false;
    }
    if (resources->count_planes > MAX_PLANES) {
        fail_device(device, OBS_ERROR, "plane count exceeds probe bound", EOVERFLOW);
        return false;
    }
    for (uint32_t i = 0; i < resources->count_planes; ++i) {
        drmModePlanePtr plane = drmModeGetPlane(fd, resources->planes[i]);
        if (!plane) {
            fail_query(device, "cannot read DRM plane");
            return false;
        }
        struct plane_info *info = &device->planes[device->plane_count];
        info->id = plane->plane_id;
        info->crtc_id = plane->crtc_id;
        info->fb_id = plane->fb_id;
        info->possible_crtcs = plane->possible_crtcs;
        info->format_count = plane->count_formats;
        drmModeFreePlane(plane);
        drmModeObjectPropertiesPtr properties =
            drmModeObjectGetProperties(fd, info->id, DRM_MODE_OBJECT_PLANE);
        if (!properties) {
            fail_query(device, "cannot read DRM plane properties");
            return false;
        }
        if (properties->count_props > MAX_PROPERTIES) {
            drmModeFreeObjectProperties(properties);
            fail_device(device, OBS_ERROR, "plane property count exceeds probe bound", EOVERFLOW);
            return false;
        }
        for (uint32_t j = 0; j < properties->count_props; ++j) {
            drmModePropertyPtr property = drmModeGetProperty(fd, properties->props[j]);
            if (!property) {
                fail_query(device, "cannot read DRM property metadata");
                drmModeFreeObjectProperties(properties);
                return false;
            }
            struct property_info *item = &info->properties[info->property_count++];
            memcpy(item->name, property->name, DRM_PROP_NAME_LEN);
            item->name[DRM_PROP_NAME_LEN] = '\0';
            item->id = property->prop_id;
            item->flags = property->flags;
            item->value = properties->prop_values[j];
            if (strcmp(item->name, "type") == 0) {
                info->type_known = true;
                info->type = item->value;
            }
            drmModeFreeProperty(property);
        }
        drmModeFreeObjectProperties(properties);
        if (!info->type_known) {
            fail_device(device, OBS_ERROR, "DRM plane has no type property", EPROTO);
            return false;
        }
        ++device->plane_count;
    }
    return true;
}

static bool collect_resources(int fd, struct device_info *device) {
    const std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources(
        drmModeGetResources(fd), drmModeFreeResources);
    if (!resources) {
        fail_query(device, "cannot enumerate DRM resources");
        return false;
    }
    if (resources->count_crtcs < 0 || resources->count_crtcs > MAX_CRTCS ||
        resources->count_encoders < 0 || resources->count_encoders > MAX_ENCODERS ||
        resources->count_connectors < 0 || resources->count_connectors > MAX_CONNECTORS) {
        fail_device(device, OBS_ERROR, "DRM resource count exceeds probe bound", EOVERFLOW);
        return false;
    }
    for (int i = 0; i < resources->count_crtcs; ++i) {
        drmModeCrtcPtr crtc = drmModeGetCrtc(fd, resources->crtcs[i]);
        if (!crtc) {
            fail_query(device, "cannot read DRM CRTC");
            return false;
        }
        struct crtc_info *info = &device->crtcs[device->crtc_count++];
        info->id = crtc->crtc_id;
        info->mode_valid = crtc->mode_valid;
        info->width = crtc->width;
        info->height = crtc->height;
        drmModeFreeCrtc(crtc);
    }
    for (int i = 0; i < resources->count_encoders; ++i) {
        drmModeEncoderPtr encoder = drmModeGetEncoder(fd, resources->encoders[i]);
        if (!encoder) {
            fail_query(device, "cannot read DRM encoder");
            return false;
        }
        struct encoder_info *info = &device->encoders[device->encoder_count++];
        info->id = encoder->encoder_id;
        info->crtc_id = encoder->crtc_id;
        info->possible_crtcs = encoder->possible_crtcs;
        drmModeFreeEncoder(encoder);
    }
    for (int i = 0; i < resources->count_connectors; ++i) {
        /* GetConnector would force a display/EDID reprobe; Current only reads
         * the kernel's cached connector state. Hotplug may still race us. */
        drmModeConnectorPtr connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
        if (!connector) {
            fail_query(device, "cannot read cached DRM connector");
            return false;
        }
        struct connector_info *info = &device->connectors[device->connector_count];
        info->id = connector->connector_id;
        info->type = connector->connector_type;
        info->type_id = connector->connector_type_id;
        info->connection = connector->connection;
        info->encoder_id = connector->encoder_id;
        if (connector->count_encoders < 0 || connector->count_encoders > MAX_ENCODERS) {
            drmModeFreeConnector(connector);
            fail_device(device, OBS_ERROR, "connector encoder count exceeds probe bound",
                        EOVERFLOW);
            return false;
        }
        for (int j = 0; j < connector->count_encoders; ++j) {
            const struct encoder_info *encoder = find_encoder(device, connector->encoders[j]);
            if (!encoder) {
                drmModeFreeConnector(connector);
                fail_device(
                    device, OBS_BLOCKED,
                    "connector references an unavailable encoder; topology may have changed", 0);
                return false;
            }
            info->possible_crtcs |= encoder->possible_crtcs;
        }
        const struct encoder_info *current = find_encoder(device, info->encoder_id);
        if (current)
            info->current_crtc_id = current->crtc_id;
        else if (info->encoder_id) {
            drmModeFreeConnector(connector);
            fail_device(device, OBS_BLOCKED, "current connector encoder is unavailable", 0);
            return false;
        }
        drmModeFreeConnector(connector);
        ++device->connector_count;
    }
    return collect_planes(fd, device);
}

static void inspect_borrowed_fd(struct device_info *device, int borrowed_fd,
                                bool universal_prepared) {
    struct stat st;
    snprintf(device->path, sizeof(device->path), "fd:%d", borrowed_fd);
    device->universal_prepared = universal_prepared;
    /* dup preserves the DRM file and ownership; reopening /proc/self/fd/N does
     * not provide this guarantee. Never close or alter the caller's copy. */
    const seaside::StorageFd inherited{fcntl(borrowed_fd, F_DUPFD_CLOEXEC, 3)};
    const int fd = inherited.get();
    if (fd < 0) {
        fail_query(device, "cannot duplicate inherited fd");
        return;
    }
    if (fstat(fd, &st) < 0) {
        fail_query(device, "cannot stat inherited fd");
        return;
    }
    if (!primary_identity(&st) || drmGetNodeTypeFromFd(fd) != DRM_NODE_PRIMARY) {
        fail_device(device, OBS_ERROR, "inherited fd is not a DRM primary device", EINVAL);
        return;
    }
    read_identity(device, &st);
    /* libdrm tests master with AUTHMAGIC(0), an invalid authentication token;
     * it does not use SET_MASTER. Fail conservatively on an ambiguous result. */
    if (drmIsMaster(fd)) {
        fail_device(device, OBS_BLOCKED, "inherited fd is master or master status is uncertain", 0);
        return;
    }
    drmVersionPtr version = drmGetVersion(fd);
    if (!version) {
        fail_query(device, "cannot read DRM driver version");
        return;
    }
    size_t length = version->name_len;
    if (length >= sizeof(device->driver))
        length = sizeof(device->driver) - 1;
    if (version->name)
        memcpy(device->driver, version->name, length);
    drmFreeVersion(version);
    device->cursor_width_known = drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &device->cursor_width) == 0;
    device->cursor_height_known = drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &device->cursor_height) == 0;
    if (!collect_resources(fd, device))
        return;
    device->enumerated = true;
    if (universal_prepared) {
        device->observation = OBS_COMPLETE;
    } else {
        fail_device(device, OBS_BLOCKED,
                    "universal-plane client capability is unverified; "
                    "missing cursor planes cannot establish absence",
                    0);
    }
}

static bool card_basename(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (strncmp(name, "card", 4) != 0 || !name[4])
        return false;
    for (const char *p = name + 4; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    return true;
}

static void usage(FILE *out) {
    fputs("Usage: seaside-probe-drm [--device /dev/dri/cardN]\n"
          "       seaside-probe-drm --fd N [--universal-planes-prepared]\n"
          "Default: discover primary nodes without opening them; return 77.\n"
          "--fd: borrow an inherited nonmaster primary fd from an authorized supervisor.\n"
          "--universal-planes-prepared: caller asserts DRM_CLIENT_CAP_UNIVERSAL_PLANES\n"
          "was enabled on that same DRM file before lending it. Not independently verified.\n"
          "No client caps, DRM ownership, modes or display resources are changed.\n"
          "JSON stdout; diagnostics stderr. Exit 0: complete capability observation,\n"
          "77: unavailable/blocked, 1: probe failure, 2: invalid arguments.\n"
          "No result qualifies working hardware cursors or actual Seaside plane use.\n",
          out);
}

int main(int argc, char **argv) {
    const auto bad_args = [] {
        usage(stderr);
        return 2;
    };
    const char *device_path = nullptr;
    int borrowed_fd = -1;
    bool universal_prepared = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(argv[i], "--device") == 0 && !device_path && i + 1 < argc) {
            device_path = argv[++i];
            if (strlen(device_path) >= PATH_MAX)
                return bad_args();
        } else if (strcmp(argv[i], "--fd") == 0 && borrowed_fd < 0 && i + 1 < argc) {
            char *end = NULL;
            const char *argument = argv[++i];
            errno = 0;
            long value = strtol(argument, &end, 10);
            if (!*argument || *argument == '-' || *end || errno || value < 0 || value > INT_MAX)
                return bad_args();
            borrowed_fd = (int)value;
        } else if (strcmp(argv[i], "--universal-planes-prepared") == 0 && !universal_prepared) {
            universal_prepared = true;
        } else
            return bad_args();
    }
    if ((device_path && borrowed_fd >= 0) || (universal_prepared && borrowed_fd < 0))
        return bad_args();

    const std::unique_ptr<device_info> storage{new (std::nothrow) device_info{}};
    auto *device = storage.get();
    if (!device) {
        perror("seaside-probe-drm: allocate snapshot");
        return 1;
    }
    glob_t matches{};
    size_t device_count = 0;
    bool blocked = false, failed = false;
    const char *summary_reason = NULL;
    if (!device_path && borrowed_fd < 0) {
        int result = glob("/dev/dri/card*", GLOB_NOSORT, NULL, &matches);
        if (result != 0 && result != GLOB_NOMATCH) {
            failed = true;
            summary_reason = "cannot enumerate /dev/dri/card*";
        }
    }
    fputs("{\"schema\":1,\"probe\":\"drm-cursor-capability\",\"read_only\":true,"
          "\"qualification\":\"capability_only\",\"actual_cursor_use_tested\":false,"
          "\"hardware_cursor_qualified\":false,\"devices\":[",
          stdout);
    size_t total = (device_path || borrowed_fd >= 0) ? 1 : matches.gl_pathc;
    for (size_t i = 0; i < total; ++i) {
        const char *path =
            device_path ? device_path : (borrowed_fd >= 0 ? NULL : matches.gl_pathv[i]);
        if (borrowed_fd < 0 && !device_path && !card_basename(path))
            continue;
        if (device_count >= MAX_DEVICES) {
            failed = true;
            summary_reason = "device count exceeds probe bound";
            break;
        }
        memset(device, 0, sizeof(*device));
        if (borrowed_fd >= 0)
            inspect_borrowed_fd(device, borrowed_fd, universal_prepared);
        else
            discover_device(device, path);
        if (device_count++)
            fputc(',', stdout);
        report_device(stdout, device);
        blocked |= device->observation == OBS_BLOCKED;
        failed |= device->observation == OBS_ERROR;
        if (device->observation != OBS_COMPLETE) {
            fprintf(stderr, "seaside-probe-drm: %s: %s%s%s\n", device->path, device->reason,
                    device->error_number ? ": " : "",
                    device->error_number ? strerror(device->error_number) : "");
        }
    }
    if (!device_count && !failed) {
        blocked = true;
        summary_reason = "no DRM primary nodes discovered";
    }
    const char *status = failed ? "error" : blocked ? "blocked" : "complete";
    fprintf(stdout, "],\"status\":\"%s\",\"reason\":", status);
    if (summary_reason) {
        json_string(stdout, summary_reason);
        fprintf(stderr, "seaside-probe-drm: %s\n", summary_reason);
    } else
        fputs("null", stdout);
    fputs("}\n", stdout);
    globfree(&matches);
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fputs("seaside-probe-drm: cannot write JSON report\n", stderr);
        return 1;
    }
    return failed ? 1 : blocked ? EXIT_UNAVAILABLE : 0;
}

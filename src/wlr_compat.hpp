#pragma once

#include <pixman.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <wayland-server-core.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <xkbcommon/xkbcommon.h>

// wlroots 0.20 uses C array parameter bounds in these declarations.
extern "C" {
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define static
#include <wlr/render/color.h>
#undef static
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

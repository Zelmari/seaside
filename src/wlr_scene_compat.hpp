#pragma once

#include "wlr_compat.hpp"

extern "C" {
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

// Keep the macro off the headers that the scene header includes.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define static
#include <wlr/types/wlr_scene.h>
#undef static
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

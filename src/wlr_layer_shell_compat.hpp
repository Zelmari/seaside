#pragma once

#include "wlr_compat.hpp"

extern "C" {
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/edges.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define namespace wlr_namespace
#include <wlr/types/wlr_layer_shell_v1.h>
#undef namespace
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

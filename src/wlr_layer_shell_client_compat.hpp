#pragma once

#include <wayland-client.h>

// The generated request uses namespace as an argument name.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define namespace wlr_namespace
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#ifdef __clang__
#pragma clang diagnostic pop
#endif

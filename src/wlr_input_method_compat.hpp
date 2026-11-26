#pragma once

#include "wlr_compat.hpp"

extern "C" {
#include <wlr/types/wlr_seat.h>
#include <wlr/util/box.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define delete wlr_delete
#include <wlr/types/wlr_input_method_v2.h>
#undef delete
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

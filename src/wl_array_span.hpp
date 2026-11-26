#pragma once

#include <span>
#include <wayland-util.h>

template <typename T> std::span<T> wl_array_span(wl_array *array) noexcept {
    if (array == nullptr || array->data == nullptr) {
        return {};
    }
    return {static_cast<T *>(array->data), array->size / sizeof(T)};
}

template <typename T> std::span<const T> wl_array_span(const wl_array *array) noexcept {
    if (array == nullptr || array->data == nullptr) {
        return {};
    }
    return {static_cast<const T *>(array->data), array->size / sizeof(T)};
}

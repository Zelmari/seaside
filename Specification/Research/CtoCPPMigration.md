# Seaside C to C++23 Migration Plan

## 1. Goal & Locked Architectural Decisions

Migrate all **31 handwritten `.c` files** and **17 handwritten `.h` files** (~18,355 lines) in Seaside to **C++23**, matching the repository's existing C++23 core (66 `.cpp` / 34 `.hpp` files) under GCC 16 / Clang 23, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`, and `.clang-tidy` (`WarningsAsErrors: '*'`).

### Locked Decisions

| # | Decision | Choice | Rationale |
|---|---|---|---|
| **1** | `wlroots-0.20` & `wlr-layer-shell` C++23 header compatibility | **Option A: Centralized compat header (`src/wlr_compat.hpp`)** | Pre-include all standard/POSIX/Wayland/DRM/Pixman/XKB headers *first* (outside `extern "C"`), then wrap `<wlr/...>` includes in `extern "C"` with tightly scoped `#define`/`#undef` around only the 4 C99-only headers (`static`, `namespace`, `delete`). Empirically verified **0 warnings** across all 67 used `wlr/*` headers on GCC 16 and Clang 23 (`0` `static inline` functions exist in `/usr/include/wlroots-0.20/`), matches existing precedent in `tests/integration/part4.cpp:12-15`, keeps IDE navigation pointed at system headers, and avoids custom Meson header-patching scripts. |
| **2** | Refactor depth | **Option A: Two-pass (port first, de-bridge second)** | Execute Phases 0–6 as clean, behavior-preserving C++23 ports (with trivial single-caller cleanups like folding `idle_inhibit` into `idle_power` and deleting `src/server.h` + `src/server_c.cpp`), then perform structural de-bridging and idiomatic C++23 redesign in Phase 7. Isolates C→C++23 semantic fixes from architectural refactors and keeps mixed C/C++ test targets green throughout. |
| **3a** | `wayland-scanner` generated `*-protocol.c` files (23 build artifacts) | **Option A: Keep `'c'` in `meson.build` strictly for generated protocol glue** | `wayland-scanner private-code` emits `WL_PRIVATE const struct wl_interface` symbols without `extern "C"`. Keeping `'c'` in `project('seaside', 'c', 'cpp', ...)` solely for generated protocol `.c` files follows standard C++ Wayland compositor practice, avoids fragile scanner wrappers, and preserves `tools/record-baseline:183`. |
| **3b** | Dynamic 28-line `LD_PRELOAD` `lstat` snippet in `tests/session/repair_services_vm_guest.py:251-281` | **Option A: Keep as inline C (`cc -O2 -shared -fPIC`)** | Not a tracked C source file; keeping it C avoids injecting `libstdc++` into arbitrary processes preloaded under `LD_PRELOAD` (and avoids ASan preload-order interactions). |

---

## 2. Repository C Inventory

| Area | `.c` Files | `.h` Files | Approx. Lines | Key External Dependencies |
|---|---:|---:|---:|---|
| `src/` (bridge, TTY/DRM/input, idle, text-input, popup) | 7 | 8 | ~4,774 | `wlroots-0.20`, `libdrm`, `libinput`, `xkbcommon`, `glib-2.0` |
| `src/` (scene, layer-shell, blur, legacy/output/window capture) | 6 | 6 | ~6,767 | `wlroots-0.20`, `wayland-server`, `libdrm`, `libpng`, `glib-2.0`, `pixman` |
| `tests/` (unit, integration, probe, hardware) | 17 | 3 | ~6,212 | `wlroots-0.20`, `wayland-client/server`, `xcb`, `libdrm`, `-Wl,--wrap=` |
| `tools/` (`probe-drm.c`) | 1 | 0 | 602 | `libdrm` (textually `#include`d by `tests/probe/report-fixture.c`) |
| **Total handwritten C** | **31** | **17** | **~18,355** | |

### Packaging & Tooling Notes
- **`packaging/`**: Contains **0** `.c`/`.h` files (`PKGBUILD` already lists `libstdc++`, `clang`, `meson`, `gtest`, etc.). Preserve the 2-space `  version: '...',` line in `meson.build:5` parsed by `packaging/prepare-source:21`.
- **`tools/check-code`**:
  - Line 57 explicitly excludes `tests/integration/headless.c` from `clang-format` — remove this exclusion once `headless.c` migrates to `headless.cpp` and format it.
  - Line 77 keeps `.c` in the `compile_commands.json` filter (`tests/runner/test_runner.py:958-967` tests a synthetic `legacy/probe.c` entry).
  - Runs `.clang-tidy` with `WarningsAsErrors: '*'` (`clang-analyzer-*`, `bugprone-narrowing-conversions`, `bugprone-exception-escape`, `bugprone-unused-raii`, `bugprone-use-after-move`, `bugprone-dangling-handle`, `performance-move-const-arg`) on all repo `.cpp`/`.hpp` files under `src/` and `tests/` (generated build-dir `.c` files are automatically skipped).
- **`tools/record-baseline`**: Line 183 reads both `'c'` and `'cpp'` compilers from `intro-compilers.json`, which remains intact under Decision 3a.
- **`tests/hardware/vmwgfx_layout.c`**: Not built by `meson.build`; compiled manually on Arch VM guests (referenced in `Specification/Parts/Part-10/HANDOFF.md:136`).

---

## 3. Technical Invariants & Compatibility Layer Design

### 3.1 `src/wlr_compat.hpp` (Decision 1, Option A)
All 67 `<wlr/...>` headers used by Seaside and all 26 generated protocol headers were dry-run compiled against `g++ 16.2.1` and `clang++ 23.1.5` (`-std=c++23 -Wall -Wextra -Wpedantic -Werror`). Exactly **4 upstream/generated headers** fail to parse in C++23:

1. `/usr/include/wlroots-0.20/wlr/render/color.h:148,172,185` — C99 `[static 9]` / `[static 3]` array parameter syntax (transitively pulled in by `wlr_output.h`, `wlr_scene.h`, `wlr_renderer.h`, `pass.h`, `wlr_output_layout.h`, `backend*.h`, `gles2.h`, `pixman.h`).
2. `/usr/include/wlroots-0.20/wlr/types/wlr_scene.h:458,470` — `const float color[static 4]`.
3. `/usr/include/wlroots-0.20/wlr/types/wlr_layer_shell_v1.h:89` (`char *namespace;`) and generated `wlr-layer-shell-unstable-v1-{,client-}protocol.h` (`const char *namespace`).
4. `/usr/include/wlroots-0.20/wlr/types/wlr_input_method_v2.h:30` (`struct wlr_input_method_v2_delete_surrounding_text delete;`) — actively read at `src/text_input.c:319` (`state->delete.before_length`, `state->delete.after_length`).

**Strict rules for `src/wlr_compat.hpp` (and optional narrow wrappers `src/wlr_layer_shell_compat.hpp`, `src/wlr_input_method_compat.hpp`):**
- **Never** include any C++ standard library, POSIX, GLib, Wayland, DRM, Pixman, or XKB header inside a keyword-macro block. Pre-include `<cstddef>`, `<cstdint>`, `<ctime>`, `<wayland-server-core.h>`, `<pixman.h>`, `<xf86drm.h>`, `<xf86drmMode.h>`, `<xkbcommon/xkbcommon.h>` (and `<wayland-client.h>` for client protocol wrappers) *before* entering `extern "C"` or defining any macro. (Avoid deprecated `<cstdbool>` under `-Werror`.)
- Keep keyword macros strictly localized and immediately `#undef`'d:
  - Around `wlr/render/color.h` and `wlr/types/wlr_scene.h`: `#define static` … `#undef static`.
  - Around `wlr-layer-shell-unstable-v1-*-protocol.h` and `wlr/types/wlr_layer_shell_v1.h`: `#define namespace wlr_namespace` … `#undef namespace`.
  - Around `wlr/types/wlr_input_method_v2.h`: `#define delete wlr_delete` … `#undef delete` (access fields as `state->wlr_delete.before_length` / `state->wlr_delete.after_length`).
- Wrap every `<wlr/...>` include in `extern "C" { ... }` (zero `wlr/*` headers contain `extern "C"` guards).
- Add `-DWLR_USE_UNSTABLE` for `language: ['c', 'cpp']` in `meson.build:39`.

### 3.2 Wayland Macro Rules
- **`wl_container_of`, `wl_list_for_each`, `wl_list_for_each_safe`, `wl_list_for_each_reverse`**: Compile cleanly in GCC 16 & Clang 23 C++23 (`WL_TYPEOF` expands to `__typeof__`) **only on standard-layout structs**. Enforce `static_assert(std::is_standard_layout_v<...>)` on structs holding embedded `wl_listener` / `wl_list` nodes. For non-standard-layout C++ classes, embed a standard-layout listener shim (matching `src/scene.cpp:154-157`).
- **`wl_array_for_each` FAILS in C++23** (`invalid conversion from 'void*' to 'const uint32_t*'`; used in `src/tty_input.c:1063,1074` and `tests/unit/tty_input_test.c:257`). Provide a tiny header `src/wl_array_span.hpp`:
  ```cpp
  template <typename T>
  inline std::span<T> wl_array_span(wl_array *array) noexcept {
      if (array == nullptr || array->data == nullptr) {
          return {};
      }
      return {static_cast<T *>(array->data), array->size / sizeof(T)};
  }
  template <typename T>
  inline std::span<const T> wl_array_span(const wl_array *array) noexcept {
      if (array == nullptr || array->data == nullptr) {
          return {};
      }
      return {static_cast<const T *>(array->data), array->size / sizeof(T)};
  }
  ```

### 3.3 Linker `--wrap` & ELF Symbol Interposition (`extern "C"` Invariants)
Every wrapped target, `__wrap_*` definition, `__real_*` declaration, and ELF-interposed `libdrm` function **must** retain `extern "C"` linkage:

| Target | File(s) | `extern "C"` Symbols Required |
|---|---|---|
| `seaside-server-tests` | `src/wlroots_bridge.cpp`, `tests/unit/server_test.cpp` | `wl_display_create`, `wl_display_destroy`, and 7 `seaside_wlr_*` functions (`allocator_create`, `renderer_init_display`, `compositor_create`, `backend_start`, `backend_destroy`, `allocator_destroy`, `renderer_destroy`) |
| `seaside-output-capture-test` | `tests/integration/capture_fence_fixture.cpp`, `tests/integration/output_capture.cpp` | 12 `__wrap_*` / `__real_*` hooks (`wlr_linux_drm_syncobj_*`, `wlr_drm_syncobj_*`, `wlr_renderer_begin_buffer_pass`, `wlr_render_pass_add_texture`, `wlr_output_commit_state`, `wlr_output_is_drm`, `drmGetCap`) |
| `seaside-part3-fixture` | `tests/integration/part8_frame_fault.cpp` | `__wrap_wlr_output_commit_state`, `__real_wlr_output_commit_state` |
| `seaside-part8-night-light` | `tests/integration/part8_night_light.cpp` | `__wrap_wlr_output_commit_state`, `__real_wlr_output_commit_state`, `__wrap_wlr_output_get_gamma_size` |
| `seaside-scanout-release` | `tests/integration/scanout_release.cpp` | `__wrap`/`__real_wlr_output_commit_state`, `__wrap`/`__real_wlr_scene_output_build_state`, **plus direct ELF interposition of** `drmSyncobjCreate`, `drmSyncobjDestroy`, `drmSyncobjTimelineSignal` |
| `seaside-part6-output-tests` | `tests/integration/part6_outputs.cpp` | 8 `extern "C"` `seaside_tty_drm_*` link-time mocks (until Phase 7 interface refactor) |

### 3.4 Recurring C → C++23 Compilation & `.clang-tidy` Checklist
Apply these patterns systematically across all migrated files (matching `src/*.cpp` style):
1. **Implicit `void*` conversions (~220+ sites)**: Replace with `static_cast<T *>(...)` for `calloc`/`malloc`/`mmap`, `wl_registry_bind`, callback `void *data`, `node->data`, `wl_resource_get_user_data`, and `g_hash_table_lookup`.
2. **Compound literals & address-of rvalue (~60 sites)**: Replace `(struct foo){...}` with `foo{...}` or named locals value-initialized via `foo opts{}; opts.field = ...;` (mandatory wherever `&(struct ...){...}` is passed to wlroots functions).
3. **`{0}` & partial struct initializers (`-Wmissing-field-initializers -Werror`)**: Replace `= {0}` with `= {}` (also fixes hard enum-first-field errors in `tty_input_test.c`) and initialize omitted trailing fields explicitly or via field assignment.
4. **`goto` crossing variable initializations**: Replace with RAII cleanup structs / `std::unique_ptr` + early returns (`tty_drm.c`, `probe-drm.c`, `headless.c`, and 81 `CHECK(...)` sites in `output_capture.c`).
5. **Exceptions across C callbacks (`bugprone-exception-escape`)**: Mark every Wayland/wlroots callback `noexcept` and guard throwing C++ calls with `try { ... } catch (...)` (matching `src/scene.cpp:264`, `src/desktop.cpp:120`).
6. **Feature-test macro redefinitions**: Remove bare `#define _GNU_SOURCE` (6 test files) and `#define _POSIX_C_SOURCE 200809L` (`scene_bridge.c`, `window_capture.c`) — GCC C++ predefines them and warns under `-Werror`.
7. **Other C-only idioms**: Replace flexible array members (`tty_drm.c`, `scene_blur.c`) with `std::vector<std::uint32_t>`, C99 array designators (`tty_drm.c:298-301`) with `switch`/`std::array`, tentative `static const` definitions (`output_capture.c`, `window_capture.c`) with forward-declared callbacks + single definition, out-of-order designators (`report-fixture.c`, `tty_input_test.c`, `output_capture.c`) with declaration order, and `virtual` identifiers (`text_input.c`, 12 sites) with `virtual_kbd`/`virtual_mgr`.

---

## 4. Phased Migration Roadmap

Commit each phase (or logical sub-step) as focused Conventional Commits per local Git rules. Every phase must leave `meson compile -C build`, `tools/run-tests`, and `tools/check-code --build-dir build` green.

### Phase 0: Build & Compatibility Foundation
- [ ] Update `meson.build:39` to `add_project_arguments('-DWLR_USE_UNSTABLE', language: ['c', 'cpp'])`.
- [ ] Switch `meson.get_compiler('c').find_library('m')` (`meson.build:253,282`) to `cpp.find_library('m')` as targets move to C++.
- [ ] Create `src/wlr_compat.hpp` (plus narrow `wlr_layer_shell_compat.hpp` / `wlr_input_method_compat.hpp` wrappers as needed) and `src/wl_array_span.hpp`.
- [ ] Add temporary `#ifdef __cplusplus extern "C"` guards to the 4 currently unguarded headers (`src/popup_grab.h`, `src/scene_blur.h`, `src/text_input.h`, `tests/integration/capture_fence_fixture.h`) so incremental mixed builds succeed across Phases 1–5.
- [ ] Verify all 67 used `<wlr/...>` headers compile with 0 warnings under both GCC 16 and Clang 23 `-std=c++23 -Wall -Wextra -Wpedantic -Werror`.
- **Gate**: `meson compile -C build && tools/check-code --build-dir build && tools/run-tests`.

### Phase 1: Zero-wlroots Leaf Utilities, Probes & Test Clients (8 `.c` files)
- [ ] `src/cursor_layout.{c,h}` → `src/cursor_layout.{cpp,hpp}` (111 LOC; fix 2 compound literals + `{0}`; keep `extern "C"` declarations until Phase 6 C tests migrate).
- [ ] `tests/hardware/vmwgfx_layout.c` → `.cpp` (40 LOC; fix `.pad64` init + `reinterpret_cast<uintptr_t>`).
- [ ] `tests/integration/part7_x11_client.c` → `.cpp` (161 LOC; cast `static_cast<xcb_atom_t>(XCB_ATOM_NONE)` on line 16, init `pollfd` `.revents`).
- [ ] `tests/integration/part7_native_client.c` → `.cpp` (331 LOC; drop `_GNU_SOURCE`, fix 10 `void*` casts & 3 partial inits).
- [ ] `tests/integration/part9_panel.c` → `.cpp` (150 LOC; include `<wayland-client.h>` before `wlr-layer-shell-unstable-v1-client-protocol.h` using `namespace` compat guard, drop `_GNU_SOURCE`, fix 6 `void*` casts).
- [ ] `tests/integration/wayland_input_client.c` → `.cpp` (560 LOC; drop `_GNU_SOURCE`, fix 21 `void*` casts & 4 partial inits).
- [ ] Coupled pair: `tools/probe-drm.c` + `tests/probe/report-fixture.c` → `.cpp` (602 + 94 LOC; extract shared internal header/unit or update `#include`, replace `goto` over variable initializations with RAII, fix out-of-order designators and compound literals in `report-fixture`).
- **Gate**: `meson compile -C build && tools/check-code --build-dir build && tools/run-tests` (specifically verify `probe` tests and all client executables).

### Phase 2: Small `src/` Modules, Headless Fixture & Delete Legacy Server Facade (4 `.c` files)
- [ ] `src/idle_inhibit.{c,h}` (125 LOC) → fold directly into `src/idle_power.{hpp,cpp}` (single caller, clean wlroots headers; removes C callback trampolines immediately).
- [ ] `src/popup_grab.{c,h}` → `src/popup_grab.{hpp,cpp}` (95 LOC; 0 body errors; keep `extern "C"` until Phase 4).
- [ ] `src/wlroots_bridge.{c,h}` → `src/wlroots_bridge.{hpp,cpp}` (174 LOC; 3 `void*` casts; **keep `extern "C"`** on the 7 `seaside_wlr_*` symbols wrapped by `seaside-server-tests`).
- [ ] `tests/integration/headless.c` → `tests/integration/headless.cpp` (520 LOC; call `seaside::Server` directly, drop `_GNU_SOURCE`, fix 9 `void*` casts and 3 `goto done` crossings, remove `tools/check-code:57` exclusion and format with `clang-format`).
- [ ] Delete `src/server.h`, `src/server_c.cpp`, and the legacy C-facade test block in `tests/unit/server_test.cpp:167-186` (zero remaining callers).
- **Gate**: `meson compile -C build && tools/check-code --build-dir build && tools/run-tests` (`headless-lifecycle`, `server-ownership`, `runner`, `part9-idle`).

### Phase 3: Capture Foundation Cluster (`seaside-capture-foundation`, 2 `.c` files)
- [ ] `src/legacy_capture.{c,h}` → `src/legacy_capture.{cpp,hpp}` (395 LOC; 8 `void*` casts, 4 compound literals, 1 narrowing `size_t` → `uint32_t`, 4 partial inits; internal to `output_capture`).
- [ ] `src/output_capture.{c,h}` → `src/output_capture.{cpp,hpp}` (652 LOC; resolve tentative `static const capture_impl` at L54/227, 11 `void*` casts, 2 address-of compound literals at L170/186, 4 partial inits; keep `extern "C"` until Phase 6).
- **Gate**: `meson compile -C build && tools/check-code --build-dir build && tools/run-tests` (`part3-output-capture`, `part7-selected-capture`, `portal-smoke`).

### Phase 4: Scene, Blur, Layer & Window Capture Cluster (`seaside-core`, 4 `.c` files)
- [ ] `src/scene_blur.{c,h}` → `src/scene_blur.{cpp,hpp}` (637 LOC; replace `blur_image::pixels[]` FAM + `GArray` with `std::vector`, fix 4 `void*` casts, 7 compound literals incl. 2 address-of rvalues, 3 narrowing casts; keep `extern "C"` until Phase 6).
- [ ] `src/layer_scene.{c,h}` → `src/layer_scene.{cpp,hpp}` (785 LOC; use `wlr_layer_shell_v1.h` compat wrapper, 10 `void*` casts, 1 compound literal, replace `enum { LAYER_COUNT }` with `constexpr` to fix `-Wenum-compare`; internal to `scene_bridge`).
- [ ] `src/window_capture.{c,h}` → `src/window_capture.{cpp,hpp}` (895 LOC; drop `_POSIX_C_SOURCE`, fix tentative `static const window_source_impl` at L461/619, 12 `void*` casts, 4 compound literals incl. 3 address-of rvalues, 8 partial inits; internal to `scene_bridge`).
- [ ] `src/scene_bridge.{c,h}` → `src/scene_bridge.{cpp,hpp}` (2,979 LOC; drop `_POSIX_C_SOURCE`, hoist `LUT_CLEAN/PENDING/RETRY` into `enum class LutRequest`, fix 26 `void*` casts, ~28 compound literals incl. 2 address-of rvalues, 9 narrowing casts, ~20 partial inits; keep `extern "C"` until Phase 6).
- **Gate**: `meson compile -C build && tools/check-code --build-dir build && tools/run-tests` (all scene/blur/appearance/night-light/scanout/part3/part4/output-capture/portal suites) + Arch VM smoke check.

### Phase 5: TTY DRM, Input & Text Input Cluster (`seaside-tty-foundation`, 3 `.c` files)
- [ ] `src/text_input.{c,h}` → `src/text_input.{cpp,hpp}` (628 LOC; use `wlr_input_method_v2.h` compat wrapper and access `state->wlr_delete.*` at L319, rename 12 uses of C++ keyword `virtual`, fix 11 `void*` casts & 3 `{0}` compound literals; internal to `tty_input`).
- [ ] `src/tty_input.{c,h}` → `src/tty_input.{cpp,hpp}` (1,416 LOC; replace 2 `wl_array_for_each` with `wl_array_span`, fix 19 `void*` casts, 4 compound literals, partial inits; keep `extern "C"` until Phase 6).
- [ ] `src/tty_drm.{c,h}` → `src/tty_drm.{cpp,hpp}` (1,735 LOC; fix `goto error:` over `device` init at L716-735, replace `cursor_buffer::pixels[]` FAM with `std::vector`, replace `[WLR_*]` array designators at L298-301, fix address-of `wlr_texture_read_pixels_options` + narrowing `.stride` at L211-215, `static_cast<wl_output_transform>` at L1268, disambiguate `seaside_tty_drm_observation` struct vs function, migrate `<stdatomic.h>` → `<atomic>`; update `seaside-tty-compile-o2` and `seaside-tty-compile-o3` in `meson.build:289-299`).
- **Gate**: `meson compile -C build` (including `-O2`/`-O3` targets), `tools/check-code --build-dir build`, `tools/run-tests` (`part8-drm-modes`, `tty-input`, `tty-fixture`, `part6-outputs`), + Arch VM smoke check.

### Phase 6: Remaining wlroots Test Fixtures & Suites (10 `.c` files)
- [ ] Small fixtures:
  - `tests/unit/wlroots_fixture.{c,h}` → `.{cpp,hpp}` (14 LOC).
  - `tests/unit/part8_drm_modes.c` → `.cpp` (90 LOC; 8 partial inits).
  - `tests/integration/part3_bridge.{c,h}` → `.{cpp,hpp}` (241 LOC; 3 `void*` casts, replace anonymous `enum` with `constexpr` for `-Wdeprecated-enum-float-conversion`, 3 compound literals).
  - `tests/integration/part8_frame_fault.c` → `.cpp` (22 LOC; keep `extern "C"` on `__wrap`/`__real_wlr_output_commit_state`).
  - `tests/integration/scene_blur.c` → `.cpp` (174 LOC; 1 `void*` cast, extract `wlr_texture_read_pixels_options` local).
  - `tests/integration/part8_night_light.c` → `.cpp` (204 LOC; keep `extern "C"` on `--wrap` hooks, 1 `void*` cast, replace `++failure` enum loop with array iteration).
  - `tests/integration/scanout_release.c` → `.cpp` (165 LOC; keep `extern "C"` on **both** `--wrap` hooks and the 3 ELF-interposed `libdrm` functions, 2 `void*` casts).
- [ ] Large suites:
  - `tests/unit/tty_input_test.c` → `.cpp` (1,693 LOC; drop `_GNU_SOURCE`, 11 `{0}` → `{}` enum inits, 2 out-of-order designators, `wl_array_for_each` → `wl_array_span`, 39 `void*` casts, 43 partial inits).
  - `tests/integration/capture_fence_fixture.{c,h}` + `tests/integration/output_capture.c` → `.{cpp,hpp}` (1,670 LOC; keep `extern "C"` on all 12 `--wrap` hooks, replace `CHECK` `goto done` macro with RAII + early returns fixing 81 errors, 44 `void*` casts, 2 out-of-order designators, 2 address-of compound literals).
- **Gate**: Full `meson compile -C build`, `tools/check-code --build-dir build`, `tools/run-tests`, and confirm `git ls-files '*.c'` returns **0** files.

### Phase 7: Post-Port C++23 De-Bridging & Idiomatic Modernization
Once all 31 handwritten `.c` files are migrated and green:
- [ ] Remove now-unnecessary `extern "C"` wrappers from internal `.hpp` headers (`cursor_layout`, `output_capture`, `scene_blur`, `scene_bridge`, `tty_drm`, `tty_input`, `part3_bridge`, `wlroots_fixture`, `capture_fence_fixture`), keeping `extern "C"` strictly on `--wrap` and ELF-interposed symbols.
- [ ] Replace C-style opaque structs + function-pointer callback tables + static `void*` trampolines in `src/session_entry.cpp`, `src/desktop.cpp`, and `src/scene.cpp` with `namespace seaside` RAII classes (`std::expected` factories, `std::unique_ptr`, typed observer interfaces) — starting with `tty_input` (removes 15-entry callback table), `tty_drm` (replaces `part6_outputs.cpp`'s C link-time mocks with a clean mock interface), and `output_capture`.
- [ ] Evaluate folding `scene_bridge` directly into `seaside::Scene` to eliminate double-wrapping.
- [ ] Replace remaining GLib containers (`GHashTable` in `scene_bridge` → `std::unordered_map`, `g_strdup`/`g_free` → `std::string`); evaluate whether `glib-2.0` is still worth keeping solely for UTF-8 validation and base64 encoding.
- [ ] Audit active docs (`Specification/CONTRIBUTING.md`, `Specification/Research/*`) without altering historical dated handoff logs (`Specification/PUBLIC_DOCUMENTATION.md:60`).

---

## 5. Verification Gates

1. **Local build & static analysis (every phase)**:
   - `meson compile -C build` (GCC 16, `-std=c++23 -Wall -Wextra -Wpedantic -Werror`, including `seaside-tty-compile-o2` and `seaside-tty-compile-o3`).
   - `tools/check-code --build-dir build` (`clang-format` + `clang-tidy` `WarningsAsErrors: '*'`).
   - `tools/run-tests` (unit, integration, probe, packaging, and runner suites).
   - Periodic Clang 23 verification build (`CXX=clang++ meson setup ...`) to ensure zero GCC-only extensions creep in.
2. **Arch VirtualBox guest qualification (after Phases 4, 5, and 7)**:
   - Deploy to `Arch Seaside Testing` without stealing host focus; run `WLR_RENDERER=pixman seaside` on TTY, verify `seasidectl status`, `foot` via Super+Enter, focus/workspace navigation, VT round-trip (`Ctrl+Alt+F2` / `Alt+F1`), screenshot-to-clipboard shutdown (`wl-paste --type image/png`, zero new `coredumpctl` crashes), and Arch package install (`pacman -U` + `pacman -Qk seaside`).
3. **High-priority regression checks**:
   - `static_assert(std::is_standard_layout_v<...>)` on all `wl_container_of` structs.
   - Zero exceptions escaping C callbacks (`noexcept` + `try`/`catch` + `clang-tidy` `bugprone-exception-escape`).
   - Explicit verification that all 5 `-Wl,--wrap=` fault-injection tests still trigger their wrapped hooks after C++ conversion.

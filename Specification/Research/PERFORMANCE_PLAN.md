# Performance plan after the C++23 migration

The first runtime task is to measure where `query.windows` spends CPU time. The
next task is to measure and reduce work on ordinary focus changes. The scene
bridge rebuild has a confirmed development cost and can proceed independently.
Collect a native display baseline when hardware is available so renderer and
capture changes can be judged on the hardware they affect.

## Baseline and measurement contract

The migration comparison used pre-migration source `1b43467` and migrated source
`bb31523`. The matching release packages carried revisions `6a481ea` and
`c85c851` respectively. The release binaries used GCC 16.2.1 with `-O2` and
`-flto=auto`. The live trials used Arch Linux, VirtualBox VMSVGA with 3D disabled,
Pixman, and one 1280×800 output configured at 60 Hz. The headless and compile
benchmarks used `-O2 -g` builds.

| Benchmark | Before migration | After migration | Follow-up |
|---|---:|---:|---|
| Headless full scene projection at 128 windows | 22.39 µs | 20.77 µs | Keep the gain and measure focus-specific work separately |
| Headless workspace dispatch at 128 windows | 737 µs | 792 µs | Compare stages since the live result moved in the opposite direction |
| Live workspace switch at 128 windows | 1.53 ms | 1.47 ms | Keep the large-desktop trial as a regression check |
| Isolated IPC at 64 windows, median and p95 | 213 µs and 262 µs | 220 µs and 263 µs | Attribute the small median difference with finer CPU timings |
| Virtual commit to presentation under H.264 recording | 2.94 ms | 2.94 ms | Collect comparable native capture and presentation data |
| Clean optimized compositor and CLI build | 42.75 s | 43.88 s | Check the clean-build cost of extracting source files |
| Incremental scene bridge rebuild and relink | 1.28 s | 1.83 s | Reduce the amount of code rebuilt by a typical edit |

A separate host replay isolated scene bridge compilation at `-O2 -g`. It took
0.72 s before and 1.21 s after the migration. GCC's diagnostic run attributed
0.05 s and 0.35 s respectively to parsing. The 64-window IPC pairs varied in
both directions, and the guest process CPU counter advanced in 10 ms ticks.
Use a thread CPU clock for stage timings before attributing that difference.
The original benchmark scripts, raw data and CSV are local under
`test-artifacts/migration-benchmarks/`.

1. Make the workload reproducible from a checkout. Reuse the headless desk and
   real Wayland fixture clients where they fit, and record the harness revision,
   workload, source and package hashes, compiler and flags, CPU affinity,
   dependencies, renderer, output mode, and relevant configuration. Run the
   same fixture against the baseline and candidate.
2. Warm up each workload, alternate paired baseline and candidate runs, and
   time one workload at a time. Record per-run sample counts, medians, p95,
   p99, and the spread of paired changes. Keep raw samples and record hiccups
   and failed correctness checks. Repeat when the observed effect is small
   relative to run-to-run variation. Compare each experiment against its
   parent commit and rerun the consolidated candidate against `bb31523`.
3. Measure end-to-end wall time and the CPU cost of the affected stage. Measure
   instrumentation overhead with a control run and separate sanitizer
   correctness runs from performance runs. Keep the output, event stream,
   pixels, or package payload checks appropriate to each workload.
4. Use headless fixtures for CPU attribution and Arch Pixman for real TTY,
   session, and package checks. Use background VirtualBox controls and verify
   the tested binary, installed package, and final TTY state. Use physical
   hardware for native display conclusions. [Headless desktop fixture](../../tests/integration/part4.cpp) · [Test harness](../CONTRIBUTING.md#build-and-test) · [Measurement method](PERFORMANCE.md#measurements-needed-first)

## 1. Reduce repeated work in window IPC

`SessionControl::windows()` walks the ordered window state, looks up each
scene view and window flags, bounds and sanitizes strings with `bounded_utf8`,
and constructs a JSON row. `IpcServer::Impl::Peer::request()` dispatches the
query. `Peer::respond()` serializes it through `ipc::serialize()` and writes it
through the existing bounded peer queue. The request handler, serialization,
and window policy files are identical between the source baselines. Stage
measurements will show where the time goes. [Window query](../../src/session_control.cpp) · [IPC peer](../../src/ipc.cpp) · [Wire protocol](../../src/ipc_protocol.cpp)

1. Measure 0, 8, 32, 64, and 128 windows with short, multibyte UTF-8, and long
   metadata. Run a direct query fixture and a persistent Unix socket trial.
   Measure scene lookup and flags, UTF-8 processing, JSON construction,
   serialization, and queueing separately. Count allocations and bytes in
   the attributed runs. Include steady queries and queries interleaved with
   window and metadata changes. Check result sizes and content in both arms.
2. Benchmark reserving the JSON array to the current window count and
   constructing each final row once as independent candidates. Keep the
   smaller change if it reduces allocation or query cost with a repeatable
   end-to-end gain.
3. If string processing dominates, prototype caching the owned, bounded
   app ID, title, and icon name for each live view. Invalidate on title and
   app-ID changes and when pending icon data is applied at surface commit,
   including icon removal. Exercise remap and remove cached state on
   destruction. The window-capture source already listens for title and
   app-ID changes. Test when updates become visible to queries and measure
   mutation cost and added memory alongside steady-query gains.
   [Scene metadata and icon lifecycle](../../src/scene_bridge.cpp) · [Metadata listeners](../../src/window_capture.cpp)
4. If profiles instead show publication on the hot path, measure
   `SessionControl::publish()` separately with zero, one, and several
   subscribers, including a slow reader. Compare updating only affected
   records against today's full diff. Keep the sequence watermark even
   with zero subscribers, event order including unmap before destroy, and
   the current queue limits and overflow behavior. [Snapshot publication](../../src/session_control.cpp) · [Publication and readiness hooks](../../src/session_entry.cpp)

**Acceptance.** Preserve window ordering by ID, mapped and hidden rows, the
11 fields and their null values, UTF-8 replacement, and the current UTF-8
byte limits of 1,024 for app IDs, 4,096 for titles, and 255 for icon names.
Preserve the separate `query.window_icon` behavior, request IDs, and JSON
Lines framing. Exercise metadata changes, remap, destruction, and slow-reader
behavior with a real `SessionControl` request if a new cache or fast path
needs coverage. Run
`part6-ipc`, `part6-cli`, `part8-output-policy`, `part4-headless-desktop`, and
the Part 6 TTY trial. Compare p95 and CPU cost on the small and busy desktops
before accepting an optimization. [IPC unit tests](../../tests/unit/ipc_test.cpp) · [Real session queries](../../tests/integration/part6_outputs.cpp) · [Part 6 VM trial](../../tests/session/part6_vm.py)

## 2. Reduce work on ordinary focus changes

`Desktop::dispatch()` calls directional or MRU focus policy and may call
`Desktop::project()`. The full projection reconciles drags, pending
openings, and output work areas, then walks the window set to update geometry,
visibility, stacking and decoration. It also updates seat focus, grabs and
pointer targets. The projection callback updates idle visibility and publishes
session state. `WindowManager::cycle_focus()` can also spend time building its
candidate list before scene projection, especially on repeated one-shot IPC
requests. [Desktop dispatch and projection](../../src/desktop.cpp) · [Focus policy](../../src/window_manager.cpp) · [Projection callback](../../src/session_entry.cpp)

1. At 1, 8, 32, 64, and 128 windows, benchmark directional focus, a held MRU
   cycle, one-shot IPC MRU cycling, and pointer focus. Keep workspace switches
   as a comparison workload. Separate candidate selection, scene placement,
   seat update, and publication. Count windows visited and placed, and record
   wall time as well as thread CPU. Include Dwindle, scrolling, and ordinary
   floating desktop cases.
2. If candidate construction dominates, test order-preserving membership
   tracking in `WindowManager::cycle_focus()`. Compare small and large
   desktops and keep the held cycle's existing ordering and reuse behavior.
3. If projection dominates, prototype updating the previous and newly
   focused ordinary floating windows on the same stable output and
   workspace. Verify which windows need raising, activated decoration, or
   pointer hit updates before narrowing the work. Reuse `Scene::place_window()`
   for the affected windows in existing order and include the eligibility
   check in the timing. Use full projection when focus reveals scrolling
   columns, alters visibility or clipping, affects dialogs or special
   window states, or intersects pending openings, resize or drag, grabs,
   layer focus, or a work-area change.
4. Compare full and narrowed projection under identical actions. Verify
   final geometry, stacked pixels, pointer and keyboard targets, grabbed
   popup dismissal, focus events and their order, idle notifications, and
   configure/commit ordering. Check first open, workspace round trips,
   delayed resize, pinned windows, nested dialogs, fullscreen, and multiple
   or temporarily absent outputs.

**Acceptance.** Fewer visits or placements and a repeatable end-to-end gain
on the targeted focus workload, with equivalent visible behavior and event
ordering. Run `part4-window-policy`, `part5-layout-policy`,
`part4-headless-desktop`, `part5-headless-dwindle`,
`part5-headless-scrolling`, `part8-fullscreen-aspect`, `part9-layer-shell`,
`part6-binding-reload-during-drag`, and `native-wayland-input`. Repeat real
resize, keyboard and pointer focus, and a VT round trip in the guest.
[Window policy tests](../../tests/unit/window_manager_test.cpp) · [Headless desktop tests](../../tests/integration/part4.cpp) · [Live layout trial](../../tests/session/part5_vm.py) · [Delayed resize trial](../../tests/session/resize_vm.py)

## 3. Shorten scene bridge rebuilds

`src/scene_bridge.cpp` is a roughly 3,000-line translation unit in
`seaside-core`, which is linked into the compositor and integration fixtures.
It contains icon encoding, scene images, view lifecycle and projection, and
per-output frame handling. Keep the current scene interface and library
target while comparing extraction boundaries. [Build graph](../../meson.build) · [Scene bridge](../../src/scene_bridge.cpp) · [Scene interface](../../src/scene_bridge.hpp)

1. Measure the current compiler, archive, and relink steps separately.
   Benchmark a clean `seaside` and `seasidectl` build, a no-op build, and edits
   to icon code, view or render code, and a common header. Use the same
   compiler cache state, parallelism, and flags on both sides. Use Ninja's
   explanations and log to record recompiled and relinked targets. Compare
   both development `-O2 -g` builds and the Arch package build with LTO.
2. Extract `copy_icon_pixels()` and `encode_icon()` behind a narrow private
   boundary so the bridge can stop including PNG and GLib for that code.
   Leave icon ownership, application and listener ordering with the view.
   If useful, separately extract the self-contained image buffer helpers
   around `seaside_scene_create_image()` and `seaside_scene_drop_image()`.
   Measure each extraction before extending it.
3. If render edits still cause expensive rebuilds, assess moving the
   per-output frame state and handlers together. Keep `commit_frame()`,
   `output_present()`, retry and fence lifetime, LUT state, and output
   cleanup together. Keep the private boundary narrow and verify that a
   shared header edit leaves a small dependency fan-out. Revisit a
   window-lifecycle or projection split only if the first extractions
   leave a measured cost.

**Acceptance.** Faster rebuilds for the measured edit cases with the clean
build and header-edit cost reported. Preserve the scene API, wlroots listener
layout and teardown, icon application, capture ordering, output lifetime, and
presentation/fence behavior. Build with GCC and Clang and use ASan/UBSan for
lifetime changes. Exercise `part3-headless-session`, `part6-scene-appearance`,
`part4-application-titlebars`, `native-wayland-input`,
`part8-display-protocols`, `part8-night-light`, `part8-scanout-release`,
`part11-backdrop-blur`, `part3-output-capture`, and
`part7-selected-capture`. Verify the installed Arch package after changes to
the link graph. [Arch recipe](../../packaging/PKGBUILD) · [Package trial](../../tests/session/part10_vm.py)

## 4. Measure native rendering and capture

Collect baseline data on native Arch with physical AMD graphics when
available. Reuse `seasidectl outputs`, `seasidectl debug render`, the bounded
`seasidectl trace start`, `stop`, and `export` workflow, and the
`wp_presentation` client fixture. Correlate scene and DRM observations by
output and commit sequence. Match clock domains before subtracting
timestamps. Measure trace overhead and count failed commits, missing or
discarded feedback, and dropped trace records separately.
[Trace workflow](../USER_GUIDE.md#troubleshoot) · [Frame observer](../../src/session_entry.cpp) · [DRM output events](../../src/tty_drm.cpp) · [Presentation fixture](../../tests/integration/part3.cpp)

| Workload | Measurements and behavior to verify |
|---|---|
| Idle desktop and small versus full damage | Per-output frame intervals and missed deadlines, compositor CPU/GPU work, and idle wakeups |
| Fullscreen composition and direct scanout | Eligible and successful scanout, fallback reasons, commit retries, and explicit fence release when overlays, blur, capture, or transforms require composition |
| Fixed refresh and VRR, single and mixed-refresh outputs | Independent pacing and mode reporting, including rotation and fractional scale, followed by VT, hotplug and suspend recovery where supported |
| Output and selected-window capture | Render, copy and fence cost, frame age and cadence through portal/PipeWire, decoded pixels, cursor inclusion, and live updates on hidden workspaces |
| Native and Xwayland apps, an external recorder, and a representative game | Input dispatch under load, client frame timing, compositor and encoder resource cost, and recording or gameplay stability with matched settings |

If a bottleneck is repeatable, isolate it before changing runtime policy.
Compare rendering/commit in `scene_bridge.cpp`, monitor copy in
`output_capture.cpp`, and selected-window delivery in `window_capture.cpp`
against the portal or encoder cost. Preserve hardware cursor, KMS night light,
idle policy, capture contents and source identity, and safe buffer release.
Use external measurement for physical input-to-photon results. Record
unavailable hardware cases explicitly. [Monitor capture](../../src/output_capture.cpp) · [Window capture](../../src/window_capture.cpp) · [Native Arch checklist](NATIVE_ARCH_CHECKLIST.md)

**Acceptance.** For an identified bottleneck, report per-output p50/p95/p99,
missed presentations and capture correctness with the actual hardware,
renderer, mode and workload. Keep correctness coverage through the display
protocol, night light, scanout release, output policy, capture, idle and trace
suites, plus relevant Part 7–9 guest trials. [Streaming fixture](../../tests/integration/test_real_portals.py) · [Display trial](../../tests/session/part8_vm.py) · [Idle and trace trial](../../tests/session/part9_vm.py)

## Delivery

Land the benchmark setup before its first optimization, then keep each
candidate in a focused commit with the before-and-after evidence. Record which
candidates improved the intended workload, which effects were inconclusive,
and any trade-offs in small desktops, tails, memory, or build times. Run the
named fixtures with `meson test -C build --print-errorlogs` during each phase.
Run `tools/check-code --build-dir build`, `tools/run-tests`, and the relevant
installed-package checks before calling the follow-up complete.
[Build and test](../CONTRIBUTING.md#build-and-test)

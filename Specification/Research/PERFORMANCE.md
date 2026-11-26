# Response time and performance investigations

Updated 2026-11-26

These notes catalogue performance investigations. The [performance plan](PERFORMANCE_PLAN.md)
records the migration benchmark baseline and schedules the IPC, scene, build,
and native display follow-up work.

For the day-to-day desktop trial on the native installation, see the
[native Arch desktop checklist](NATIVE_ARCH_CHECKLIST.md).

## Selected baseline

- C++23 with GCC, wlroots, its existing scene graph and renderers,
  libwayland's main event loop, and Meson/Ninja.
- Automatic production renderer selection through `wlr_renderer_autocreate`.
  Record the renderer actually selected on each tested machine.
- Standard C++ `std::jthread` workers with a small bounded task queue for slow
  file/configuration/image work; results return to the owning main event loop.
  Queue submission must not block dispatch, and stale results are discarded.
- Existing window-management behavior, hardware-cursor enforcement, capture
  behavior, night-light policy and configurable presentation settings.
- Bounded, opt-in diagnostics under §12.1; ordinary operation does not acquire
  continuous performance tracing or an overlay.

Seaside's handwritten sources use C++23. Wayland protocol generation still
emits C build artifacts. The migration comparison includes headless CPU
timings and live Arch VirtualBox results. Native AMD presentation and
physical input-to-screen measurements require native hardware.

## Measurements needed first

Use native Arch on physical AMD graphics hardware for graphics and
latency conclusions. Headless/VM cases remain useful for functional regression
tests and controlled failure cases.

For each comparison, preserve the source revision, compiler and flags, kernel,
Mesa, wlroots/protocol revisions, actual renderer, GPU, display mode/refresh,
scale/rotation, game/client versions and relevant configuration. Keep workload,
power/thermal conditions and presentation settings comparable; change one
variable at a time and repeat trials.

Measure the relevant stages separately: event dispatch, correlated test-client
response, buffer readiness, rendering/commit and actual presentation feedback.
Reconcile clock domains before subtracting timestamps. Rendering duration,
client FPS, input-event-to-presentation timing and physical input-to-photon
latency are different measurements. Physical end-to-end claims need external
measurement; a software timestamp alone cannot establish them. Presentation
feedback describes a submitted update's realized presentation, rather than
its predicted target time; the installed primary protocol definition is
`stable/presentation-time/presentation-time.xml` in `wayland-protocols`.

Report sample counts, distributions and tail behavior (for example median,
p95 and p99), missed presentations, relevant CPU/GPU work and correctness
results. Describe measurement uncertainty and instrumentation overhead. Use
comparable optimized builds for performance comparisons, keeping sanitizer
runs as separate correctness evidence. Define no arbitrary millisecond target.

## Investigation list

Use the [performance plan](PERFORMANCE_PLAN.md) for the active investigations.
Use the other entries as profiles identify work worth measuring.

| ID | Investigation | Evidence to collect and behavior to preserve |
|---|---|---|
| P01 | **Automatic selection versus OpenGL ES/Vulkan** | Record the automatic result, then compare supported renderer paths using development-only controls available in the pinned wlroots build. Include composition workloads; direct scanout can bypass compositor drawing. Compare latency distributions, GPU time, buffer import/synchronization, capture and stability. Automatic selection remains the agreed production policy; no renderer TOML option is proposed. |
| P02 | **Frame scheduling and queued work** | Locate avoidable waits between input, buffer readiness and presentation. Measure queue depth where observable, deadline misses and tail latency under desktop and fullscreen loads. Exercise the existing VRR/tearing policies and fixed-refresh operation separately; do not change their approved defaults or synchronization semantics to improve a result. |
| P03 | **Main event-loop stalls** | Measure long callbacks and delays caused by config reload, image decoding, IPC, logging, external command startup and service communication. Use the approved bounded C++ workers for slow file/configuration/image work and asynchronous service integration, preserving ordering and recovery. Measure queue saturation, result-handoff costs and shutdown behavior before tuning internal limits. |
| P04 | **Direct scanout eligibility and transitions** | Count eligible fullscreen presentations that actually scan out and record observable fallback reasons. Compare scale/rotation, capture, overlays and hardware night light. Verify correct composition fallback and prompt recovery when eligibility changes. Required drawing or capture must not be omitted merely to improve scanout statistics. |
| P05 | **Copies, buffer imports and synchronization** | Trace avoidable buffer copies, CPU readbacks and blocking fence waits. Check supported formats/modifiers and buffer reuse, retaining correct acquire/release synchronization. Measure native and satellite-backed clients; a path is not zero-copy just because it uses DMA-BUF. |
| P06 | **Damage, visibility and idle rendering** | Compare partial/full damage, unchanged desktops and covered wallpaper. Check frame scheduling and CPU/GPU wakeups when no redraw is needed. Actively captured hidden windows must continue producing live frames; visibility optimizations must preserve that requirement. |
| P07 | **Input dispatch and cursor updates** | Measure input delivery and cursor presentation during render, IPC and reload load. Verify actual hardware-cursor use and recovery after VT/hotplug/resume. Preserve pointer acceleration, event ordering, focus, bindings and gestures; this is not a proposal to discard/coalesce input or introduce new device settings. |
| P08 | **Multiple outputs with different refresh rates** | Compare each output's pacing with mixed refresh, scale and rotation. Investigate unnecessary coordination that delays a faster output. Preserve independent output state, workspace ownership and recovery; do not infer physical timing from a nested display. |
| P09 | **Screen-sharing overhead** | Compare inactive capture, whole-output capture and chosen-window capture, including live updates on a hidden workspace. Attribute copying, rendering, scheduling and external portal/PipeWire costs separately. Verify no capture work when unused and no substitution of another window/output. Isolated application audio remains deferred. |
| P10 | **Layout, scene updates and allocations** | Profile layout/focus/workspace operations with representative window counts. Consider fewer redundant scene updates, bounded caching or allocation reduction only where measured. Verify existing geometry, focus/lifetime invariants, config reload and client-observed events; do not introduce different layout policies. |
| P11 | **IPC and diagnostic overhead** | Test slow subscribers, output floods and temporary tracing under load. Measure event-loop delay and allocation/serialization costs. Preserve bounded queues, accurate overflow reporting and opt-in traces; performance work must not add continuous monitoring. |
| P12 | **Compiler optimization and dependency revisions** | Compare supported release optimization levels, link-time optimization or representative profile-guided optimization after identifying CPU costs. Record flags and keep the supported CPU baseline. Separately compare kernel/Mesa/wlroots revisions with reproducible before/after results and regression tests. No compiler or dependency change is selected by this entry. |
| P13 | **More specialized rendering infrastructure** | Revisit a targeted upstream fix or custom rendering component only if profiles identify a limitation that the selected wlroots APIs cannot adequately address. Estimate maintenance and compatibility costs alongside any demonstrated gain. Replacing the scene graph, renderer or backend requires a separate architecture decision. |

## Recording a result

For an investigated ID, add a dated result with the hypothesis, exact scenario,
baseline/candidate revisions, reproduction command or fixture, artifacts,
measurements and uncertainty, correctness checks, and remaining native/manual
observations. Record regressions and negative results too. State whether the
evidence supports a change, rules it out or is inconclusive.

Correctness and approved behavior are prerequisites for accepting a performance
change. A measured gain does not itself approve a new setting, changed default,
weakened synchronization/cursor rule or replacement architecture. Routine
internal fixes that preserve the contract follow the normal development rules.

### VirtualBox terminal stalls

During the Part 3 VM trial at `ae05938`, a tester observed intermittent
pauses of about half a second while typing in `foot`. The entered characters
then appeared together. The Arch VirtualBox guest used `vmwgfx`, Pixman and
a 1280 × 839 mode near 60 Hz. The duration is a user estimate. The cursor and
terminal worked after the VT round trip.

The keyboard handler forwards accepted events to the focused Wayland client
as they are dispatched. The scene sends frame callbacks after output commits.
The first Arch VM harness run also reported guest watchdog stalls. Correlation
with the terminal pauses remains open. Track under P02 and P03. Compare the
same typing workload in `foot` and the plain VM TTY.
If the stall recurs, record whether the cursor freezes at the same time, then
correlate guest scheduling, keyboard delivery, client commits and DRM
presentation. The cause and the native timing remain unmeasured.

## Primary API references

These identify mechanisms to investigate, not measured advantages. Match them
to the actual installed/pinned versions before implementing an experiment.

- [wlroots renderer selection and GPU render timers](https://wlroots.pages.freedesktop.org/wlroots/wlr/render/wlr_renderer.h.html).
- [wlroots scene graph and output helpers](https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_scene.h.html).
- [wlroots output, cursor, synchronization and presentation APIs](https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_output.h.html).
- [Linux DRM/KMS](https://docs.kernel.org/gpu/drm-kms.html).
- [GCC optimization options](https://gcc.gnu.org/onlinedocs/gcc/Optimize-Options.html).

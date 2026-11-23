# Part 8 plan for display and GPU behavior

Part 8 completes the output and presentation paths and qualifies them on
a native Arch system with AMD DRM/KMS. Start with the existing output
transactions, workspace migration, wlroots scene, hardware cursor checks,
and `seasidectl` status. Use the Part 7 capture clients and receiver to test
what happens when recording or sharing starts and stops during fullscreen use.
[Part 4 handoff](../Part-04/HANDOFF.md) · [Part 6 handoff](../Part-06/HANDOFF.md) · [Part 7 handoff](../Part-07/HANDOFF.md)

The contract is in §§5–6, 8.2, 10.6, and 11–14 of the design. Cover monitor
identity and physical rounding (G3 and G8), mandatory hardware cursors (G7),
hardware night light (G9), and the gaming portion of G12. Part 9 owns idle
and layer-shell integration, the bounded trace recorder and exporter, and
broader session recovery. Part 8 checks display recovery when a VT transition
or resume returns the seat and outputs to the running session. [Seaside design](https://github.com/Zelmari/seaside/blob/77c0607ea900c6ccbd2da510c9ec141a34a2aba8/Specification/SEASIDE_SPEC.md)

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 8A. Establish the display baseline | Record wlroots and protocol inputs and the actual renderer, backend, modes and DRM capabilities. Check the pinned headers and behavior for scene commits, presentation feedback, DMA-BUF, fractional scale, syncobj, async flips and hardware LUTs before choosing the implementation path. Keep automatic renderer selection. | Reproduce the Part 6 output transaction and Part 7 capture checks. Record guest capabilities and the first native baseline when the reference system is available. |
| 8B. Finish output configuration and recovery | Extend `output_config`, `tty_drm` and the desktop state paths for the remaining mode, identity, hotplug and cursor cases. Reapply per-output settings on reconnect and revalidate cursor planes after output or seat changes. | Verify applied geometry and mode, rollback on rejected changes, workspace restoration, client survival with zero outputs, and isolation of an output without a usable cursor plane. |
| 8C. Complete buffer and presentation support | Wire functional presentation-time, fractional-scale/viewport and DMA-BUF integration and capability-gated DRM syncobj into the existing wlroots scene and renderer. Track per-output frame state and results, and verify damage-aware composition and direct scanout with a safe commit fallback. | Observe real client events, buffer release, frame scheduling and correctly presented pixels. Exercise rejected commits, loss of an output with work pending and repeated scanout/composition transitions. |
| 8D. Apply fullscreen policies | Implement preserve/stretch using the presented content geometry and keep the configured physical mode. Complete VRR policy and support client-requested asynchronous presentation only for eligible fullscreen frames on a capable output. | Check preserved ratios and bars, stretch, return to the prior window state, relative pointer and pointer lock, native and satellite clients, and actual fixed-refresh/VRR and synchronized/async outcomes. |
| 8E. Apply hardware night light | Connect the existing global config, bindings and IPC to the qualified hardware LUT path. Report the effective setting and the per-output result through existing queries. | Verify the default off state, 4500 K when enabled by default, restoration on disable, reapplication through display changes, and unchanged color and output availability on unsupported hardware. Measure the effect on scanout. |
| 8F. Run the harness and qualify the milestone | Register the `part8` suite in Meson, add the Arch TTY scenario, and prepare the native display trials alongside the code. Record the evidence in the Part 8 handoff. | Run the software, guest and native cases below. Record the exact tested revision. |

## Output and presentation acceptance

- Test preferred resolution at the highest supported refresh for that
  resolution and independent resolution and refresh overrides. Cover explicit
  primary selection and automatic ranking by maximum advertised refresh,
  resolution, then connector order. Check that the automatic primary stays
  selected when a faster display appears or the active mode changes. Keep the
  remembered output ID distinct from its connector. Cover known EDIDs moving
  connectors, missing or duplicate serials, and two similar displays remaining
  distinct. Exercise manual placement, 1.0/1.25/1.5 scales,
  all four rotations, transformed logical dimensions and rounding, and the
  scale requested by the actual client protocol.
- Extend the output transaction fixture for disconnected overrides, unsupported
  modes, partial commit failure and rollback failure. Exercise disconnect,
  cursor-based output disable, automatic workspace migration/restoration,
  manual reassignment during the outage, and zero-output survival in both
  workspace modes. Verify focus, input and window identity when outputs return.
  Check the actual cursor plane and motion after modesets, transforms, hotplug
  and VT return, including a resume with changed connectors on native Arch.
- With controllable Wayland clients, cover DMA-BUF import and basic SHM
  compatibility, explicit acquire/release ordering and implicit sync, and
  presentation or discard feedback. Verify that use waits for acquire and
  release follows the last use across rendering, scanout and capture. Exercise
  buffer replacement and client/output destruction with work pending. Assert
  that teardown releases owned resources and other clients and outputs keep
  making progress. Run real fence and GPU import cases on a capable backend.
- Attempt scanout where eligible, including a suitable ordinary surface.
  Exercise composition with overlap and the config-error banner, incompatible
  buffers, scale and rotation, cursor constraints and capture that requires
  composition. Check the Part 7 monitor and chosen-window content during the
  transitions and observe scanout recovery when eligible again. Exercise a
  failed KMS test/commit and verify bounded retry, correct fallback, and no
  busy rendering on an unchanged desktop. Correlate `seasidectl debug render`
  and `seasidectl outputs` with client feedback and observable backend evidence
  for actual mode, scanout, capability and frame counters. Keep unknown fields
  honest when the backend cannot establish a result.
- Exercise VRR `off`, `fullscreen` and `always`, including fullscreen entry and
  exit on one of two outputs. Check tearing defaults to allowed for eligible
  fullscreen clients that request it on a supported backend. Test the
  tearing opt-out, rejection of async presentation and synchronized
  fallback. With a mismatched client surface, sample bars versus stretch and
  verify output mode, pointer mapping, popup placement and window restoration.
  Check an output-sized client surface, fullscreen shortcuts and uncapped
  client pacing.
- Test night light through both bindings and IPC, then reload unrelated
  settings, temperature, and the configured enable value. Exercise rejection
  and restart, output hotplug, mode changes and VT return. Verify restoration
  of the prior hardware color state, independent outcomes on mixed-capability
  displays, and no GPU color pass. Record scanout with night light off and on
  on the same mode and workload.

## Test harness and Arch VM

- Add GoogleTest cases around the existing production policy and extend the
  headless Wayland and capture fixtures with predictable buffer patterns and
  event assertions. Set `SEASIDE_TEST_PART=8` and tag cases with the existing
  requirement metadata. Use the Part 6 DRM-boundary fixture for injected failures
  and multi-output sequences and the Part 7 clients for capture regressions.
  Wait for protocol and output events with deadlines. Keep each trial's config,
  state, runtime directory and owned processes isolated. [Harness contract](https://github.com/Zelmari/seaside/blob/77c0607ea900c6ccbd2da510c9ec141a34a2aba8/Specification/SEASIDE_SPEC.md#122b-incremental-test-harness-and-minimum-human-involvement)
- From a clean build, use `tools/run-tests --jobs 2 --suite part8 --suite part6
  --suite part7` for focused runs, then `tools/run-tests --jobs 2` for the full
  regression in Arch. Run the affected suites in a separate Meson ASan/UBSan
  build using `-Db_sanitize=address,undefined` and the runner's `--build-dir`.
  Inspect `summary.json`, `summary.txt`, `baseline.json` and relevant logs
  under `test-results/`. Use `tools/check-code --build-dir build` for the
  standalone formatting and analysis check.
- Add `tests/session/part8_vm.py` and its guest scenario following the Part 7
  runner. Build and test the exact revision in an isolated Arch guest checkout.
  Run the guest software suite and the production TTY entrypoint, with real
  preflight, `foot`, a working virtual DRM cursor plane, and the supported mode,
  scale and rotation transitions. Check invalid-mode rollback, fullscreen and
  capture, VT return to the same running clients, clean exit and fail-to-TTY
  on an unavailable required service. Require pass evidence for each applicable
  case. Report cases the virtual GPU or guest connectors cannot exercise with
  the observed capability and reason.
- Run the trial from an idle logged-in TTY in the test guest. Retain the
  harness summaries, actual guest renderer and backend, logs, and relevant
  pixel and DRM evidence.
  Record whether a failure came from a test assertion, an unavailable
  capability or guest infrastructure.

## Native Arch qualification and handoff

- Add opt-in checks under `tests/hardware/` for native Arch with AMD graphics.
  Start with a real TTY and the existing passive DRM probe. Collect actual KMS and
  client presentation evidence for cursor use and movement, scale/rotation,
  reconnect and workspace restore, scanout/fallback, explicit sync, VRR,
  tearing and hardware LUT. Use mixed-refresh monitors and suitable VRR/async
  hardware where available. Include a physical cable and VT/resume round trip
  with live clients. Require visual checks where software cannot establish the
  result, such as cursor alignment or visible night-light warmth.
- Run predictable native and X11 clients and representative Steam/Proton titles.
  Check lazy satellite startup, focus restoration, pointer lock, fullscreen
  shortcuts and capture start/stop. Compare fixed-refresh and supported VRR or
  tearing runs in optimized builds. Collect bounded, opt-in samples of idle
  work, frame timing and misses, and the fraction of eligible presentations
  reaching scanout with observable fallback reasons. Record the clock and
  instrumentation limits. Physical input-to-photon timing requires external
  measurements. [Performance investigations](../../Research/PERFORMANCE.md)
- Keep the open Part 7 OBS/Discord, desktop-share audio, microphone, playback
  device and Fn-key checks in the native checklist. Use the same native test
  window when practical and record each app and device result against that
  checklist. [Part 7 checks still open](../../Research/NATIVE_ARCH_CHECKLIST.md#part-7-checks-still-open)
- Record the date, exact revision, kernel, Mesa, wlroots, protocol hashes,
  renderer/backend, GPU, monitor models and modes, app versions, commands,
  and bounded artifacts. Mark results by requirement and environment as pass,
  fail, skip or blocked. Record implementation, automated verification and
  pending qualification separately. Guest results qualify the virtual DRM
  path. Physical gates require native evidence. If AMD access, a display
  capability or a manual interaction is unavailable, name the exact missing
  prerequisite and keep that gate pending while independent work proceeds.

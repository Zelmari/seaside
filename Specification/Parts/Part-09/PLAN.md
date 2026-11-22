# Part 9 plan for idle, layer-shell, diagnostics, and recovery

Part 9 adds display sleep and wake, support for optional external Wayland
shell clients, temporary frame tracing, and recovery of a running session.
Reuse the Part 3 service probes, the Part 6 config, IPC and worker paths,
the Part 7 capture receivers, and the Part 8 output and presentation paths.
An empty autostart list still starts a complete Seaside session.
[Seaside design](../../SEASIDE_SPEC.md) · [Part 8 handoff](../Part-08/HANDOFF.md)

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 9A. Support external layer surfaces | Integrate `wlr-layer-shell` with the existing scene and seat. Follow the pinned protocol for output selection, layer ordering, anchors, margins, exclusive zones, keyboard interactivity, and popups. Project per-output work areas into the desktop. | Real Wayland fixtures verify configure and map, reservations and geometry, pointer and keyboard routing, fullscreen suppression and restoration, and output or client removal. |
| 9B. Add idle power and inhibition | Use an event-loop deadline with the existing live `idle.display_off_after_minutes` setting. Resolve eligible `idle-inhibit` surfaces and visible fullscreen windows per output. Put an output to sleep and wake it through its DRM path while retaining its logical assignment. | A fixture clock covers the 15-minute default, `0`, live reload, inhibitor transitions, independent outputs, input wake, and rejected power commits. The guest verifies actual virtual display power transitions where supported. |
| 9C. Record and export bounded traces | Implement the existing `trace.start`, `trace.stop`, and `trace.export` commands. Collect opt-in output and frame observations from production event hooks. Export a completed trace through the bounded worker queue and return its path after a successful write. | Check typed status and errors, every cap, dropped samples, JSONL and file permissions, slow readers, write failures and retry, and continued frame and input dispatch during export. |
| 9D. Recover the running session | Complete post-ready monitoring and requalification of required external integrations. Reconcile seat, input, cursor and output state across VT changes and resume with the existing DRM and workspace owners. | Observe failure and recovery events and receive a new portal stream after recovery. Verify client, focus and workspace continuity through applicable VT, suspend and output changes, plus clean exit and strict startup failure. |
| 9E. Qualify and hand off | Register a `part9` suite and extend the exact-revision Arch VM trial. Record the tested commands, revisions, artifacts and deferred native checks in the handoff. | Run the software and guest checks below. Publish the verified implementation and verify the usual VM checkout and rebuilt `build/seaside` at that revision. |

## Layer-shell and idle acceptance

- Start with pinned layer-shell and idle-inhibit definitions. Use small clients
  that exercise real configure/ack/commit and input events. Cover explicit and
  automatic output assignment, all four layers, positive, zero and negative
  exclusive-zone behavior, margins, scale and rotation. Update the affected
  work area when a panel maps, changes or closes. Check tiling and maximize
  with the resulting work area and fullscreen with the full output.
- Apply the existing fullscreen policy per output. Verify that nonessential
  layers stop drawing and receiving input on that output while fullscreen is
  visible, and restore them on exit. Handle protocol-required keyboard and
  popup behavior and preserve the config-error banner. Exercise supported
  layer keyboard modes, keep compositor shortcuts effective, and restore the
  previous eligible focus when a layer releases it or disappears. Check that
  a hidden layer leaves no hit target or active grab.
- Base idle eligibility on the output's visible content and supported
  inhibitors. Recompute it on visibility, focus/output routing, layer, app,
  and inhibitor lifetime changes. Test hidden workspaces, unmap/destroy,
  repeated inhibitor changes, and fullscreen moving between two outputs.
  Shortening, extending or disabling the timeout must reconcile with the
  current idle state through the accepted config transaction.
- Preserve clients, workspace ownership, mode, scale and transform through
  idle power-off. Keep sleep distinct from a disconnected or cursor-rejected
  output so the hotplug path does not migrate sleeping workspaces. Route
  eligible keyboard and pointer activity to wake, reverify hardware cursor
  presentation, and check pointer and keyboard continuity. Exercise idle
  around VT loss, reconnect, capture in flight and shutdown. A visible
  fullscreen client keeps only its own output awake. [Idle and shell requirements](../../SEASIDE_SPEC.md#7-shell-interoperability-capture-clipboard-and-idle-power)

## Trace and recovery acceptance

- Match the existing IPC contract. Stop at the first of 60 seconds, 20,000
  entries, or 16 MiB. Keep at most one in-memory trace and report entries,
  bytes, dropped samples and the stop reason. Record actual output and
  presentation observations with documented clock domains. Exercise idle
  time expiry, start while running, stop when idle, export before start or
  while running, and replacement of a completed trace. [Trace contract](../../SEASIDE_SPEC.md#105-error-codes-cli-and-tracing-behavior)
- Write complete JSONL under `$XDG_STATE_HOME/seaside/traces/` with owner-only
  permissions (`0700` for the directory and `0600` for files). Use the
  existing state-directory fallback and safe file-writing patterns. Keep the
  trace available after a failed export. Extend IPC to finish a deferred
  response within the existing request-ID, watermark and backpressure rules.
  Test queue saturation, slow writes, connection loss, concurrent requests
  and teardown while export is pending. Verify trace collection runs only
  during an active trace and other clients stay responsive during export.
- Treat strict startup readiness and post-ready integration health as distinct
  states. After readiness, observe portal frontend/backend and PipeWire loss,
  use bounded asynchronous rechecks with external activation and recovery,
  and emit `integration.failed` and `integration.recovered` on actual changes.
  Cancel stale work on service replacement and shutdown. Keep existing
  Wayland clients and IPC responsive. Demonstrate a new live share through
  the real chooser and receiver after recovery, including repeat failures,
  timeouts and stale probe responses. Scope fault injection to test-owned
  service instances. [Session contract](../../SEASIDE_SPEC.md#3-startup-session-recovery-and-shutdown)
- Verify seat pause and reactivation, focus and grab cleanup, preservation
  of clients and workspaces, and reapplication of display settings. Use the
  Part 8 cursor-plane evidence to verify recovered presentation. Exercise
  suspend/resume and connector changes on the available backend, and leave
  physical sleep, wake and resume observations in the native checklist.

## Harness, Arch VM, and handoff

- Register requirement-tagged `part9` unit and integration cases in Meson.
  Run `tools/run-tests --jobs 2 --suite part9 --suite part8 --suite part7
  --suite part6 --suite part3`, the full regression, and ASan/UBSan on affected
  lifetime paths. Use fixture clocks, deterministic clients and injected
  failures for fast coverage. Retain real protocol events and pixel samples
  where they can establish the result. Record test and code-quality outcomes
  and any deadline or environment blocker.
- Extend the Part 8 bundle-based scenario on `Arch Seaside Testing`. Build the
  exact revision in an isolated guest checkout with private config and state.
  Verify the real TTY, virtual hardware cursor, shell-less startup, a test
  panel and fullscreen restoration, idle/wake, trace export, and external
  service loss and fresh sharing. Use the shortest supported live idle timeout
  for the real-DRM trial. Exercise VT return and both clean and forced-failure
  exit. Try guest suspend/resume only where the runner has a reliable targeted
  wake path. Record actual renderer and backend, using Pixman on the current
  guest configuration. [Part 8 VM commands](../Part-08/HANDOFF.md#automated-checks)
- Check VM status and inspect a console screenshot for an idle logged-in
  foreground shell on TTY 1 before sending input. Use targeted VirtualBox
  controls so host window focus stays intact. Save guest results, logs, trace
  artifacts and display evidence. Mark each check as pass, fail, skip or
  blocked with the specific reason. Keep physical AMD display sleep/wake,
  resume and device changes in the native checklist when still pending, along
  with the existing Parts 7 and 8 checks. [Native Arch checklist](../../Research/NATIVE_ARCH_CHECKLIST.md)
- When the implementation is ready for owner testing, push the verified
  revision to `origin/main`, fast-forward the usual `~/Projects/seaside`
  guest checkout, rebuild `build/seaside`, and verify the revision and TTY
  launch through the Council runner. Preserve the owner's config and local
  changes. Record the tested revision and any blockers in the handoff.

# Part 12 plan for desktop workflow and fullscreen input

Develop on `desktop-workflow`, based on the merged `main` at `f2f34e4`.
This plan records code observations and proposed behavior. The Arch VM will
be supplied during development. Record the initial input and frame-pacing
results from that baseline before changing either path.

## 1. Cycle through the workspaces shown on both monitors

`WindowManager::cycle_focus` currently builds and advances a snapshot using
the focused workspace. `set_focus` can raise or reveal a window, while
monitor focus is tracked separately. Changing the snapshot alone would
leave subsequent commands targeting the wrong output.
[Window policy](../../../src/window_manager.cpp) ·
[Desktop action and keyboard focus](../../../src/desktop.cpp)

- Keep the existing Alt+Tab action and held-Alt behavior. Build one
  most-recently-used (MRU) list from eligible windows on the workspace shown
  by each enabled output, including a visible scratchpad. With one output,
  cycle its displayed windows. With two outputs, allow the next window to
  be on either output. Exclude hidden workspaces and disconnected or disabled
  outputs. Preserve the logical displayed set through normal input wake
  from DPMS sleep.
- Track actual seat focus changes across outputs and retain separate local
  history for restoring each workspace. Keep a stable snapshot through a
  held Alt sequence and promote the final selection on release. Revalidate
  candidates before applying them. Use a deterministic fallback for
  eligible windows with no focus history, and include each pinned window
  once on the output where it is visible. Retain existing fullscreen,
  transient dialog, floating stacking and scrolling reveal rules.
- Set the target output and window together through the desktop focus and
  projection path. Apply the normal Wayland keyboard focus and grab
  handling, and preserve the pointer position for ordinary Alt+Tab.
  The displayed workspace IDs stay in place. An empty output contributes
  zero candidates, so focus can still move to a populated output.
- Extend the window-manager and Part 4 real-client fixtures. Cover
  cross-output MRU, repeated Tab with Alt held, release and restart of the
  cycle, wrap, one or zero eligible windows, an empty second output, pins,
  dialogs, fullscreen and popup focus, and candidate unmap or output removal
  mid-cycle. Update the current expectation that cycling from an empty
  secondary is a no-op. In the VM, leave the pointer on A, use real Alt+Tab
  to select a client on B, then type. Check the received events, focused
  output/window, unchanged pointer position and both displayed workspace
  IDs. Repeat the opposite direction and with one output.
  [Window policy tests](../../../tests/unit/window_manager_test.cpp) ·
  [Part 4 fixture](../../../tests/integration/part4.cpp)

## 2. Diagnose cursor capture and add opt-in centring

The current seat code activates a constraint when the pointer has entered
the requesting surface, lies in its effective region and shares a root
surface with keyboard focus. It also checks for seat grabs and compositor
gestures. After a lock activates, the input path already suppresses
absolute cursor movement and delivers relative motion. The session's lock
callback changes cursor visibility and leaves its coordinates in place.
These observations explain why Seaside supplies no automatic centring.
Distinguish an inactive or cancelled constraint from an active constraint
that lets events escape. Wayland permits a compositor to position the
pointer for activation and requires pointer focus and an eligible position
before it sends the activation event.
[Seat input](../../../src/tty_input.cpp) ·
[Session cursor callback](../../../src/session_entry.cpp) ·
[Wayland pointer-constraints protocol](https://raw.githubusercontent.com/wayland-mirror/wayland-protocols/main/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml)

### Capture the failure on the baseline

1. Extend the native Wayland test client or add a small sibling fixture.
   Give it fullscreen and windowed modes, an unconstrained control,
   explicit lock and confine modes, configurable input and constraint
   regions, and persistent and one-shot lifetimes. Log keyboard and pointer
   enter/leave, constraint requests and activation, relative motion,
   ordinary pointer motion, buttons and configure/commit transitions.
   A passive client on the other monitor logs any misrouted events.
2. Add a simulated X11 fullscreen client using the existing XCB fixture.
   Request fullscreen through EWMH. Exercise a checked pointer grab with a
   confine target and cursor hide/warp behavior that requests relative
   capture through the installed bridge where supported. Record the X11
   grab result, events and observed Wayland constraint lifecycle. Trace
   which requests the Xwayland/satellite versions actually forward before
   changing the bridge.
3. For each fixture, start off-centre inside the fullscreen content, in an
   aspect-ratio bar on the same output, and on the other output. Focus the
   test client with the keyboard, then drive motion towards the seam
   without a click. Repeat with a click as a control. Log whether a request
   was issued, when it activated, when it ended, which surface received each
   input event and where the compositor placed its cursor. Exercise
   accelerated and raw relative deltas and an absolute tablet path
   separately. Compare a source buffer smaller than the viewport with a
   larger source that must be downscaled. Cover both fullscreen aspect
   policies and unequal scale or rotation as the guest supports them.
   [Native fixture](../../../tests/integration/part7_native_client.cpp) ·
   [X11 fixture](../../../tests/integration/part7_x11_client.cpp) ·
   [Existing seat constraint tests](../../../tests/unit/tty_input_test.cpp) ·
   [Xwayland input source](https://raw.githubusercontent.com/mirror/xserver/master/hw/xwayland/xwayland-input.c)

### Fix the captured failure and verify the placement policy

- Make an eligible pending constraint activate as soon as keyboard focus,
  hit testing and the committed region agree. Verify focus-first, request-first,
  hover-entry and scene changes. Entering a valid target with keyboard
  focus should require zero clicks. If capture is already active, relative
  and absolute input must keep the lock at its activation position or the
  confinement inside the effective region on that output. The second
  client must receive zero foreign motion or clicks during active capture.
- Investigate the current confinement conversion as a specific hypothesis.
  The scene supplies surface-local coordinates while the input path adds
  layout deltas and forwards the result as a layout delta. Test
  downscaled, stretched and letterboxed surfaces to see whether the point
  escapes on conversion or whether reprojection cancels the constraint.
  If confirmed, use paired layout/surface transforms for the presented
  content, intersect the effective region with its visible content and
  output, and clamp before applying movement. Verify null and explicitly
  empty regions, commit timing and handling after a transform changes.
  [DRM cursor placement](../../../src/tty_drm.cpp) ·
  [Scene projection and hit testing](../../../src/scene_bridge.cpp)
- Add a live `[input.mouse]` setting spelled
  `auto_center_on_lock = false`. With it enabled, place the cursor once at
  the centre of the rendered interactive content when a visible,
  keyboard-focused toplevel has explicitly requested a pointer **lock**
  and that point passes the committed region and scene hit test. Apply the
  policy to a translated X11 lock only when the equivalent request is
  observable. A focused fullscreen lock can acquire from the other output
  with the setting enabled and a valid target. For a windowed lock,
  require that the pointer is already over the requesting content. Leave
  focus-only changes, ordinary fullscreen, confinement-only requests,
  background windows, popups and active grabs on their normal pointer
  paths. Deliver pointer enter before lock activation and keep a synthetic
  placement out of the client's relative motion stream.
- With the setting off, keep an eligible lock at the current position.
  If the pointer starts outside the eligible region, the request waits
  until it enters. Validate the independently fixed capture behavior with
  this default. Treat any committed cursor-position hint as an unlock
  concern and diagnose it separately if the baseline shows a relevant
  problem. Reload changes apply to the next lock acquisition, so toggling
  the setting causes no mid-lock jump.
- Repeat each failing sequence after every relevant change. Check motion
  suppression and real raw deltas for lock, in-region motion for confine,
  and ordinary cross-monitor movement for the unconstrained control.
  Cover focus loss and reacquisition via Alt+Tab, region changes, popup and
  drag grabs, unmap and client exit, fullscreen exit, output loss, and VT
  switch. Verify prompt release, visible free cursor, one-shot cleanup,
  persistent reacquisition and coherent state after each transition.
  Run the real protocol cases in both the synthetic two-output harness
  and installed VM TTY session.

## 3. Investigate persistent lower frame rates by workspace

Use the baseline revision with a small, deterministic Wayland animation.
Extend the native client or add a sibling fixture that continuously submits
identical timed damage from preallocated buffers. Log numbered client
commits, frame callbacks, and `wp_presentation` feedback using the
advertised presentation clock. Frame callbacks indicate client pacing.
Presentation feedback and output events measure what reached the display.
[Presentation fixture](../../../tests/integration/part3.cpp) ·
[Existing performance measurement plan](../../Research/PERFORMANCE_PLAN.md)

- Match the monitor mode and refresh, buffer size, scale, transform,
  window state, damage and renderer. Control idle/DPMS, adaptive sync,
  tearing, blur, capture, and background load. Start with composition at a
  fixed refresh and check other paths individually if the result points to
  them. Save the effective settings with each run.
- Compare numbered workspaces on the same connector in single-output mode
  first. Test repeated workspace changes and revisits in one session to
  check whether a slowdown persists. With two outputs, compare within each
  numbered workspace group, then vary primary assignment, connector,
  keyboard focus and client identity independently where supported. Run
  counterbalanced A/B and B/A samples with 5 seconds of warm-up and 20
  seconds of steady-state data per sample, at least three pairs, then
  restart and repeat any apparent difference.
- Pair `seasidectl outputs`, `workspaces`, `focus` and `debug render`
  with short `trace start`, `trace stop` and `trace export` captures. Join
  commit and presentation events by output and commit sequence. Compare
  median and tail inter-presentation times, missed refresh intervals,
  commit failures, presentation discards and compositor render path
  alongside client submission cadence. Count dropped trace records and
  check whether tracing changes the result.
  [Render diagnostics](../../../src/session_control.cpp) ·
  [Frame trace](../../../src/session_entry.cpp) ·
  [Output frame and presentation handling](../../../src/scene_bridge.cpp)
- Patch only after a repeatable difference follows the workspace under
  controlled conditions and a cause is identified. Use the timings to
  isolate client submission, scene visibility or damage, frame
  scheduling, render/scanout path, or output commits. Preserve a focused
  failing regression and rerun the paired measurements against the fix.
  If the symptom stays inconclusive, record the tested conditions and
  results and leave runtime behavior alone. Report virtual and physical
  GPU timing evidence separately.

## 4. Add a scratchpad workspace on the primary monitor

- Add one named scratchpad with regular window layout and lifetime, hidden
  initially. Keep the ten numbered workspaces and their selection/move
  range of 1–10. Reuse the configured primary-output selection and
  existing primary fallback. Keep the scratchpad out of numbered workspace
  ownership, last-shown and reconnect selection.
- Bind `Super+grave` (Super plus backtick on the default layout) to a
  named toggle. Opening from either monitor saves the primary's current
  numbered workspace, shows the scratchpad and sends keyboard focus to
  its last eligible window if one exists. Newly opened windows can map
  there. Closing restores the primary's saved numbered workspace and its
  local focus history. If the secondary has keyboard focus at close, keep
  that focus. Selecting a numbered workspace for the primary hides the
  scratchpad. Selecting one on the secondary leaves the primary slot in
  place.
- Retain the scratchpad's clients, layout and focus when hidden. Provide
  a bindable move-to-scratchpad action for an existing window and its
  transient children. Keep the scratchpad's current visibility and
  restore another eligible local focus if the moved window becomes
  hidden. Reuse numbered move to take a window out. Keep pins scoped to
  numbered workspaces so hidden scratchpad clients cannot appear
  elsewhere. On primary reassignment or disconnection, hide the
  scratchpad, restore a valid normal workspace on each surviving output,
  and make it available on the new primary at the next toggle. With zero
  outputs, retain its contents and allow an ordinary numbered fallback
  for new unrelated windows.
- Publish additive named workspace state and actions through the binding
  and IPC paths so the scratchpad is distinguishable from numeric IDs.
  Update fresh and previously generated stock bindings, preserve custom
  or empty binding sets, and honour explicit removal or reassignment
  across reload and restart. Verify the old stock binding migration still
  applies. Test the real grave chord, empty and populated toggles, repeated
  toggling from the secondary, focus and workspace restoration, window
  movement and stacking, hidden-window input isolation, Alt+Tab while
  shown and hidden, primary reassignment, disconnect and reconnect.
  [Workspace state](../../../src/state.hpp) ·
  [Window policy](../../../src/window_manager.cpp) ·
  [Bindings](../../../src/bindings.cpp) ·
  [Config and stock binding upgrades](../../../src/config.cpp) ·
  [IPC commands and queries](../../../src/session_control.cpp)

## 5. Apply a default browser from Seaside config

- Add `[applications]` with `default_browser = "firefox"` to the
  canonical example, generated template and parser defaults. Resolve
  `firefox` to the installed `firefox.desktop` and accept installed desktop
  entry IDs for alternatives. Validate the value as an application ID and
  let desktop entry activation handle URL arguments. Use the browser for
  HTTP and HTTPS, and for local HTML/XHTML where the entry advertises those
  types. Keep other schemes and file defaults with the desktop.
- Use the standard Seaside-specific MIME defaults under
  `${XDG_CONFIG_HOME:-$HOME/.config}/seaside-mimeapps.list`. Seaside already
  sets `XDG_CURRENT_DESKTOP=Seaside` for launched apps and its portals.
  Update the managed entries atomically, preserving unrelated entries and
  any newer external edits. Resolve and check the selected desktop entry
  before applying it. Preserve the prior association as a fallback, and
  when the requested app is unavailable, release the previous Seaside
  override if still owned and report the effective fallback. Apply valid
  config changes live and keep the last-good state on an invalid save.
  Treat Firefox as an optional application.
- An isolated planning probe with temporary XDG directories resolved a
  stub browser via `xdg-mime` and GIO. Both `xdg-open` and `gio open`
  delivered the test HTTPS URL to the stub. During development, use two
  stubs that log the exact argument received. Test HTTP/HTTPS and local
  HTML on the guest, including a launched app and the real OpenURI portal
  with its existing user choice behavior. Exercise a live change, missing
  app, malformed config and correction, and a pre-existing association
  file. Verify the original file defaults in a different desktop context.
  [Session activation](../../../src/session_environment.cpp) ·
  [Portal activation](../../../src/session_activation.cpp) ·
  [MIME Applications specification](https://specifications.freedesktop.org/mime-apps/latest-single/) ·
  [XDG OpenURI portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.OpenURI.html)

## Development order and VM sign-off

1. When the VM is available, record the baseline SHA, package, kernel,
   VirtualBox, wlroots, Xwayland/bridge, renderer, input devices and output
   IDs, modes, scales and arrangement. Start from the logged-in TTY and
   use the repository's `seaside-vm` skill for headless host control and
   installation. With VMSVGA and 3D disabled, use `WLR_RENDERER=pixman`.
   Verify `seasidectl outputs` reports two distinct connected, enabled,
   awake outputs before beginning the real cross-monitor cases.
2. Build the no-game fixtures and capture existing input and frame pacing
   before implementation. Use the Part 8 guest runner as a starting point.
   Its launcher depends on the adjacent Council checkout, which was absent
   during planning. Reuse the guest scenario or fall back to the VM skill's
   background control when that transport is unavailable. The older
   mouse helper relies on Windows COM absolute input. Use a guest-targeted
   Linux VirtualBox or guest evdev fixture for independent relative bursts
   and absolute tablet events. Confirm both paths reach their intended
   devices and leave host focus alone.
3. Part 10's VM accepted a layout request while only one display became
   available. Try the existing vmwgfx layout probe on a suitable guest and
   verify the effective topology. If the supplied VM exposes one DRM
   output, run deterministic two-output headless protocol tests inside it
   and run one-output production TTY tests. Record dual-output TTY as
   pending until a two-output VM setup can exercise it.
   [Earlier VM results](../Part-10/HANDOFF.md) ·
   [vmwgfx layout probe](../../../tests/hardware/vmwgfx_layout.cpp) ·
   [Dual-output tests](../../../tests/integration/part6_outputs.cpp)
4. Land the diagnosed pointer fix and centring toggle in focused commits,
   with a baseline/candidate comparison for each. Implement Alt+Tab, then
   the scratchpad and its integration with the cycle. Handle the browser
   separately. Implement a frame-pacing change only if the investigation
   establishes its cause. Update the user guide and both config examples
   alongside the behavior changes. Test omitted keys, invalid reload,
   last-good recovery and installed payload.
5. Run `tools/run-tests --suite unit --suite integration` and the
   relevant packaging checks, plus an ASan/UBSan pass for pointer
   lifecycle changes. Boot the final installed Arch build from the
   original TTY and run the native/X11 capture matrix, keyboard focus and
   scratchpad checks, browser routing, and any reproduced frame-pacing
   case. Verify the installed revision and package with `pacman -Qk seaside`,
   repeat the VT round trip and exit to the original TTY.
6. Keep revision-tagged logs, screenshots, event summaries and traces in
   the ignored test directories. Record each case as pass, fail, skipped
   or blocked, including separate native Wayland, Xwayland, headless and
   real TTY coverage. Finish when the fullscreen capture regression
   passes in the two-monitor VM and the remaining work has the recorded
   acceptance evidence or a documented external dependency.
   [Build and test workflow](../../CONTRIBUTING.md#build-and-test) ·
   [Part 8 guest runner](../../../tests/session/part8_vm.py)

# Part 9 idle, layer-shell, diagnostics, and recovery

An external Wayland panel or launcher can use `wlr-layer-shell` to reserve
work area, receive input, and open popups. Panels and other ordinary layers
are suppressed on the output showing fullscreen content. Top and overlay
layers with exclusive keyboard interactivity keep their protocol focus.
Seaside starts with an empty autostart list by default.

`idle.display_off_after_minutes` applies live, defaults to 15, and accepts
`0` to disable display sleep. A visible fullscreen window or an eligible
`idle-inhibit` surface keeps its output awake. Relevant local input wakes a
sleeping output with its workspace, mode, and clients intact. Seaside
rechecks the hardware cursor on a new presentation after wake.

Use `seasidectl trace start`, `seasidectl debug render`, `seasidectl trace
stop`, and `seasidectl trace export`. The trace records compositor commit and
backend present observations while armed. It stops at 60 seconds, 20,000
entries, or 16 MiB. `observed_ns` is a decimal string in the compositor's
steady-clock domain. `presentation_ns` is the backend presentation timestamp
when available. Successful export returns the completed JSONL path under
`$XDG_STATE_HOME/seaside/traces/` or the usual state-directory fallback.

Required services still gate startup. A service loss during a running session
produces `integration.failed`. Seaside rechecks PipeWire and the external
portal route asynchronously and emits `integration.recovered` after the
service passes its check. Portal recovery verifies unit ownership before it
reuses or restarts services. [Part 9 plan](PLAN.md) · [Seaside design](../../SEASIDE_SPEC.md)

## Automated checks

```sh
tools/run-tests --jobs 2 --suite part9 --suite part8 --suite part7 --suite part6 --suite part3
tools/run-tests --jobs 2
python3 tests/session/part9_vm.py --console-ready
```

The VM runner requires an idle logged-in foreground shell on TTY 1. It takes
the compositor and guest scenario from the same Git revision, builds in an
isolated checkout, and keeps guest config and state temporary. It accepts
`--pkg-config-path` and `--runtime-prefix` for staged guest dependencies.
The default renderer is Pixman on the
dedicated virtual graphics configuration. Results include the guest report,
logs, captured frames and the exported trace in the Council VM evidence
directory and guest `test-results/`.

## Arch VM evidence from 2026-11-22

- The full trial at `18f2d2f` passed 44 Meson checks and 268 GoogleTest
  cases, including the installed portal checks. The passive inventory skipped
  because it had no borrowed DRM descriptor. The real TTY trial retained the
  same `foot` shell through the VT round trip and returned to TTY 1 after a
  clean exit and an induced PipeWire startup failure.
- The panel pixels appeared in the top layer, disappeared during fullscreen,
  and returned after fullscreen. The runner retained PNGs for each state.
  The trace exported real `commit` and `present` rows from the virtual
  output with directory mode `0700` and file mode `0600`.
- A private, trial-owned PipeWire daemon was stopped and restarted to verify
  the running compositor's reconnect. The external portal backend and
  frontend were then stopped in turn. The corresponding failure and recovery
  events were observed, with application identity, workspace, and focus
  retained. A new share via the real chooser sent a 1301×810 monitor frame
  to a receiver over the guest's usual PipeWire instance.
- The live one-minute idle timeout powered off `Virtual-1`. A guest key
  woke it. DRM readback again found cursor plane 40, and a new capture
  showed the original client with its workspace and output assignment.

The guest ran kernel `7.3.5-arch1-1`, wlroots `0.20.2`, Wayland protocols
`1.50`, VirtualBox Guest Additions `7.2.22`, and `vmwgfx` DRM with Pixman.
The full reports are in Council VM evidence run `20261122T003049Z-e2d005ae`
and guest `test-results/vm-e2d005aeab4b`. Guest system suspend skipped
because the runner has no verified targeted wake path.

## WSL2 software evidence

The full harness passed 43 Meson checks, including clang-format and clang-tidy,
and 268 GoogleTest cases. The installed portal checks and passive DRM
inventory skipped in WSL2. Report `20261122T003048Z-595a5ecc`. The old
240-second static-analysis deadline expired on this build. The harness has
a 480-second aggregate static-analysis budget and retains the per-file cap.

ASan/UBSan with leak detection passed 32 Meson checks and 157 GoogleTest
cases across Parts 3, 4, 6, 7, 8, and 9. Both installed portal checks
skipped. Report `20261122T003037Z-2e6654bb`.

## Native Arch qualification still open

The Framework Desktop checklist covers physical AMD display sleep and wake,
per-output inhibition with mixed displays, suspend and resume, connector
and input-device changes, and the panel or launcher used on the actual
desktop. [Native Arch desktop checklist](../../Research/NATIVE_ARCH_CHECKLIST.md#part-9-checks-on-the-framework-desktop)

# Part 4 plan for windows and workspaces

Part 4 turns the Part 2 state registry and Part 3 XDG, scene, and seat
foundations into working window management. The scope is basic window geometry,
focus, and workspace behavior under [the Seaside design](https://github.com/Zelmari/seaside/blob/77c0607ea900c6ccbd2da510c9ec141a34a2aba8/Specification/SEASIDE_SPEC.md),
especially §§2.2, 4.1, 4.4–4.5, 5, 6.2, 12.2b, and 14. The
[Part 3 VM trial](../Part-03/HANDOFF.md#observed-vm-results) provides the starting TTY and
session baseline.

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 4A. Own real XDG windows | Connect toplevel lifecycle events to the existing C++ state registry through the scene bridge. Track XDG configure, ack, and commit, keeping committed client geometry and requested geometry distinct. Put ordinary clients on their logical workspace and project visibility into the scene. Replace production PID-based terminal focus with the normal window lifecycle. | Two independent Wayland clients can map, unmap, remap the same object, and destroy in different orders. Unmap removes focus and visibility, destroy invalidates the ID, and delayed protocol events cannot resurrect stale state. |
| 4B. Select and move between workspaces | Add focused output, workspace selection and history, and move-to-workspace with follow. Honor global and per-monitor modes, globally unique IDs, last connected assignment, and explicit transfer. Wire usable-output add/remove events into the policy so the running session can survive with zero outputs and resume on return. | Exercise new, hidden, and already-visible destinations in both modes. Assert exactly one window home, move-and-follow focus, transfer fallback on the source, no cursor warp, and intact clients and workspace state across loss of all outputs. |
| 4C. Route seat input and focus | Extend the existing seat bridge with surface hit testing and pointer enter, leave, motion, button, and axis routing. Keep hover and keyboard focus independent, honor active grabs or pointer constraints, and implement click-to-focus, floating raise, spatial wrap, and workspace-local MRU. Dispatch the Part 4 actions through the existing startup binding map. | Real clients report click focus and background scroll delivery. Tests cover hidden/background maps, stable Alt+Tab order while the modifier is held, removal during cycling, directional wrap, visible-workspace focus restoration, and shortcut precedence in fullscreen. |
| 4D. Restore window geometry and state | Add basic logical placement and separate ordinary geometry from effective geometry. Implement maximize against the work area, fullscreen against the output, and monitor-local pin/unpin. Handle parented-dialog placement, stacking, focus eligibility, and movement with the parent. Integrate XDG requests and popup handling with the same window and seat policy. | Real clients acknowledge and commit state/size changes. Assert ordinary → maximized → fullscreen → maximized → ordinary restoration, pin visibility across switches with a single window record, and parent/dialog behavior across moves, unmaps, and destruction. |
| 4E. Verify the production path | Register Part 4 suites with Meson and `tools/run-tests`. Run a clean build, the Part 4 suite, earlier regressions, and sanitizer checks for lifecycle and input handling. Add Part 4 scenarios to the automated Arch VM trial. | Collect commands, versions, results, and artifacts from the headless and guest runs. Preserve the existing TTY startup, VT return, and clean-shutdown checks. |

## Scope and acceptance details

- Keep `State` authoritative for identity and membership. The scene bridge
  applies geometry, visibility, and stacking on the event-loop thread. Use
  stable IDs for references that survive a callback, and keep focus and
  visibility coherent on unmap, output loss, and callbacks during teardown.
- Start from simple deterministic placement. Preserve ordinary state and
  layout eligibility across maximize, fullscreen, and pin so Part 5 can attach
  its tiling engines. Use the output rectangle as the initial work area and
  exercise distinct work-area geometry in tests for later layer-shell
  reservations. Part 5 owns the split tree, column viewport, tile operations,
  and global layout rebuilds. Part 6 owns full live reload and public IPC.
- In the multi-output fixture, cover selection in place, transfer, monitor-local
  pinning, and restoration after output loss. Exercise zero-output survival
  with a real Wayland client that stays connected, then assert restored
  presentation and input on output return. Use usable-output events for these
  tests. Native connector identity, display mode changes, and DRM hotplug
  qualification stay with Part 8.
- Reuse the real XDG client fixture and add bounded protocol-driven checks for
  configure/ack/commit, keyboard enter/leave, and pointer delivery. Exercise two
  visible client windows in the VM for click focus, scroll without focus change,
  MRU, workspace select and move-with-follow, and maximize/fullscreen/pin
  restoration. Use an isolated Arch guest at the exact tested
  revision and retain its guest report, logs, and relevant artifacts. Record
  each unavailable case with the exact missing guest or hardware capability.

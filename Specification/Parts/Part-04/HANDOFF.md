# Part 4 windows and workspaces

The TTY session now manages ordinary XDG windows through the same event-loop
policy used by the headless fixtures. See the [Part 4 plan](PLAN.md) and
[design](../../SEASIDE_SPEC.md) §§4–5, 6.2, and 14.

- The scene bridge applies XDG configure state, visibility, geometry, and
  stacking. Unmap preserves the window ID until destroy. Restore configures
  guard the saved ordinary size against commits from an older special state.
  XDG popups use their parent's scene and output bounds.
- The state policy restores workspace-local focus, handles both workspace
  modes, and follows explicit window or workspace moves. It tracks automatic
  migration so explicitly reassigned workspaces stay at their new destination.
  When the last usable output disappears, the running session keeps client
  connections and window ownership.
- Pointer hit testing and keyboard focus use the same scene as presentation.
  Hover scroll delivers to the pointer surface, while click sets keyboard
  focus. The seat handles implicit and popup grabs, relative motion,
  constraints, and consumed compositor shortcuts.
- Basic placement centers windows with deterministic offsets. Maximize uses
  the logical work area, fullscreen uses the output, and pin applies to one
  monitor. Part 4 uses the output rectangle as the runtime work area. A
  separate work area is exercised by the policy suite for layer-shell
  reservations. Tiling eligibility and ordinary geometry are available for
  the Part 5 layout engines.

The startup binding map operates on the Part 4 actions. The example
[configuration](../../SEASIDE_CONFIG_EXAMPLE.toml) includes optional chords for
workspace selection, move-and-follow, transfer, close, pin, maximize, and
fullscreen. The seat tries the exact effective XKB symbol, then the active
layout's base symbol and the effective symbol's alternate case. Each lookup
uses the same modifier chord. Full live reload and public IPC are Part 6 work.

## Automated coverage

| Test | Evidence |
|---|---|
| `part4-window-policy` | Fourteen GoogleTest cases cover focus history and directional wrap, both workspace modes, transfer, automatic migration, dialog geometry, pinning, zero outputs, and restore state. |
| `part4-headless-desktop` | Two real Wayland clients exercise configure/ack/commit, including a delayed special-state commit, popup hit testing and dismissal, workspace move-and-follow, and remap and close. Both connections survive removal of the last headless output, and presentation resumes when it returns. |
| `part4-pointer-seat` | Real clients assert keyboard focus independently of pointer enter, leave, button, and axis. The fixture also covers compositor-consumed button pairs, active grabs, lock and confinement, raw relative motion, and device removal. |
| `part3-tty-input` | The existing keyboard fixture also covers Part 4 shortcut dispatch. It verifies physical Shift+digit with explicit shifted-symbol precedence, case fallback for the default `Super+D`, unbound shifted digits reaching the client, and existing VT bindings. |

The Part 4 fixtures run with `tools/run-tests --suite part4`. A full
`tools/run-tests` includes the earlier regression suites and static analysis.

## Local verification

The Arch WSL2 build used GCC 16.2.1, wlroots 0.20.2, Wayland 1.26.0,
wayland-protocols 1.50, and GoogleTest 1.18.0.

| Command | Result |
|---|---|
| `tools/run-tests --build-dir build-part4-clean --timeout 330` | 22 passed, including formatting and static analysis. All 184 GoogleTest cases passed. The real portal and DRM probes were skipped in WSL2. |
| `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 tools/run-tests --build-dir build-asan --suite unit --suite integration --timeout 330` | 20 passed. All 184 GoogleTest cases passed with no sanitizer findings. The real portal test was skipped in WSL2. |

Earlier local runs identified static-analysis issues in DRM output cleanup and
coordinate conversions. The final normal run includes the corrections.

## Arch VirtualBox verification

The Seaside VM runner built revision `fdde1fec4df83179e13d94ebf2981e96d49bbcc4`
in a temporary guest checkout from a local Git bundle. Arch Linux
`7.3.5-arch1-1` used `vmwgfx` and Pixman. The guest runtime harness passed
22 Meson entries, including the real portal checks, and all 184 GoogleTest
cases. The passive DRM probe was skipped. Static analysis passed in WSL2 after
the first guest analyzer run encountered kernel soft lockups.

- The production TTY session reached `seaside READY cursor-plane=40`.
  It produced valid 1280×839 captures before and after VT switching, and the
  same terminal shell PID handled typing after the switch.
- Wheel input changed captured pixels in the background terminal while the
  foreground terminal retained keyboard focus. A click gave focus to the
  background terminal. Alt+Tab and the documented `Super+Shift+2` moved focus
  and followed the moved terminal, and switching back restored each workspace's
  previous focus.
- `Super+M` and `Super+F` changed the terminal grid from 21×70 to 62×213 to
  64×213 and back through 62×213 to 21×70. `Super+P` made the pinned terminal
  appear on an otherwise empty workspace. Unpinning restored the exact blank
  frame.
- Normal exit returned to the original TTY with status 0. Forced PipeWire
  failure reported `missing(-112)` and returned with status 1.

The earlier TTY run exposed the Shift+digit shortcut mismatch, which is now
covered by the keyboard fixture and the final guest trial.

Multi-output migration and zero-output survival are verified with headless
outputs. The VM exposed one usable connector. Physical multi-monitor hotplug
and native AMD display qualification remain pending for Part 8.

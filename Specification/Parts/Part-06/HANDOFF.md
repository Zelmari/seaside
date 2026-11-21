# Part 6 live config, IPC, and bindings

Seaside watches the user TOML and checks each complete edit before applying it.
A bad edit leaves the current settings and recovery snapshot in place and
shows a top-edge diagnostic, including over fullscreen. A valid repair
clears the banner. `workspaces.mode` takes effect on restart, and
`seasidectl status` shows the active and requested mode. Autostart runs once
per launch after preflight. [Part 6 plan](PLAN.md) · [Seaside design](../../SEASIDE_SPEC.md)

The `seasidectl` client can query status, outputs, workspaces, windows, and
focus. It also runs compositor actions and subscribes to events. Commands
started by Seaside inherit `SEASIDE_SOCKET`. The CLI can also find the socket
from `WAYLAND_DISPLAY` in the running session.

## Manual checks in the Arch VM

At the VirtualBox guest's logged-in TTY, run
`WLR_RENDERER=pixman ./build/seaside` from `~/Projects/seaside`.
To make temporary edits away from the normal config, launch with separate XDG
config and state directories. Open `foot` with `Super+Return`, then edit the
generated `$XDG_CONFIG_HOME/seaside/config.toml` from there.

```sh
trial=$(mktemp -d)
WLR_RENDERER=pixman XDG_CONFIG_HOME="$trial/config" XDG_STATE_HOME="$trial/state" ./build/seaside
```

| Check | Shortcut or command |
|---|---|
| Open two terminals, change focus, and toggle tile or float | `Super+Return` twice, `Super+Arrow`, `Alt+Tab`, `Super+D` |
| Move and resize windows in both layouts | `Super+Left Mouse Drag` and `Super+Right Mouse Drag`. Save `layout = "scrolling"` or `layout = "dwindle"` under `[window]` while both terminals are open. |
| Observe reload and recovery | Run `./build/seasidectl events` in one terminal. Set `gaps_outer = "invalid"` under `[window]` from the other, save, then set a valid integer. Watch the banner appear and clear. |
| Verify keyboard remapping | Change `"Super+Return"` to `"Super+F2"` in `[keybinds]`, save, and try both chords. Restore the original mapping to return to the default. |
| Check fullscreen banner and workspace controls | Use `./build/seasidectl window toggle-fullscreen`, `workspace 2`, `window to-workspace 3`, `outputs`, and `status` with the same CLI path. |
| Check restart-only mode | Change `[workspaces] mode`, check `./build/seasidectl status`, and exit and relaunch with the same XDG paths. |
| Check the physical media mapping where available | Use volume down, volume up, and mute, including the keyboard's Fn mapping for those keys. |
| Exit cleanly to the guest shell | `./build/seasidectl exit` |

## Verification commands

- Run `tools/run-tests --suite part6` for the reload, appearance, pointer
  geometry, attention, protocol, and CLI fixtures.
- Confirm an idle TTY in the background screenshot, then run
  `python3 tests/session/part6_vm.py --console-ready` for the isolated Arch VM
  trial. It uses an exact Git bundle and temporary config, checks a real TTY,
  and retains the reports and captures in the Council VM evidence directory.

## Evidence from 2026-11-21

- The full Arch VM trial at `74d5ba304937` passed 32 Meson checks and 235
  GoogleTest cases, including the installed portal smoke check. The standalone
  Part 1 DRM capability probe was skipped. The real TTY check reached cursor
  plane 40 at 1280×815, retained the same `foot` shell through a VT round trip,
  and returned to the TTY on both clean exit and the induced PipeWire failure.
- The Part 6 TTY scenario reproduced invalid edit, valid repair, and restart
  recovery over a real IPC socket. An unsupported resolution preserved the
  active mode and snapshot. A valid edit moved the output to `[64, 32]` while
  retaining its mode and cursor. It also verified Super+Return removal,
  Super+F2 remapping, IPC event watermarks, and the requested per-monitor mode
  after restart. The guest report and captures are under
  `test-results/vm-5e3b0c5ce6fc` in the usual guest checkout.
- The host ASan/UBSan run passed all 13 Part 4–6 Meson tests. The VM trial used
  Arch Linux `7.3.5-arch1-1`, Pixman, nlohmann/json `3.12.0`, PangoCairo
  `1.58.2`, libpng `1.6.59`, TurboJPEG `3.2.1`, and libwebp `1.6.0`. The VM
  build used the signed Arch `nlohmann-json` package in a user-local build
  directory because installing a system package required interactive sudo.

The expanded Arch VM trial at `6c6061e49b10` passed 34 Meson checks and 240
GoogleTest cases. Its real TTY session applied a live wallpaper and showed
the expected pixels after switching between Dwindle and scrolling. Rejected
wallpaper and layout edits preserved the active scene and recovery snapshot.
The VirtualBox output accepted live 125% scaling with the hardware cursor
and the expected logical capture size. IPC returned `not_ready` for mutations
during preflight while queries, subscriptions, and clean exit worked. Closing
a real client made its window ID return `not_found`.

Headless fixtures in the same guest run injected a failure after the first
of two output commits and checked rollback and snapshot preservation. They
also exercised the rollback failure path, queried output and workspace
state over a Unix socket across loss and return of the last headless output,
and verified a TOML binding reload canceled an active drag. All 15 Part 4–6
Meson checks passed under ASan/UBSan on the host. The expanded guest reports
are in `test-results/vm-e22c5125dcd4`.

## Remaining qualification

Native AMD scanout, tearing, fullscreen aspect, and hardware night-light
qualification are scheduled for Part 8. Idle timer integration, trace
recording and export, and external service recovery are scheduled for Part 9.

# Part 12 handoff for desktop workflow and fullscreen input

The packaged implementation is `e1cb8036e6e683f69aaa3599cac4dd73006147b6`
on `desktop-workflow`. The baseline was `8525179`, which has the same
compositor code as merged `main` at `f2f34e4`.

## Desktop behavior

- Alt+Tab follows the seat's keyboard focus history across displayed
  workspaces. The target receives keyboard input in place. Ordinary
  switching preserves pointer position and output workspace assignments.
- Super+backtick toggles the scratchpad on the primary output. The
  scratchpad keeps its clients while hidden. Use `seasidectl scratchpad
  toggle` to toggle it or `seasidectl window to-scratchpad` to send the
  focused window there.
- `[input.mouse]` accepts `auto_center_on_lock = false`. Enabling it
  centres an eligible, explicit pointer lock acquisition. A centre
  outside the effective region leaves the normal acquisition behavior
  in effect. Changing the preference while a lock is active takes effect
  on the next eligible acquisition.
- `[applications]` defaults to `default_browser = "firefox"`. An
  installed desktop entry ID can replace it. HTTP, HTTPS and supported
  HTML types follow the Seaside-specific MIME defaults.
  `seasidectl status` reports the requested and effective handlers.
  [User guide](../../USER_GUIDE.md#configure-seaside) ·
  [Config example](../../SEASIDE_CONFIG_EXAMPLE.toml)

## Cursor capture diagnosis

The two-output Wayland regression started with a 1600×1200 fullscreen
surface presented at 800×600. On the baseline, the client's confinement
activated without a click on entering the eligible region. Starting
near layout x=1290, a leftward relative input crossed the seam at x=1280.
The observer on the primary received enter at x=1269.777. Absolute tablet
motion produced the same loss of confinement. An active lock already
suppressed cursor motion and forwarded genuine relative deltas.

Confinement now transforms deltas into the presented surface coordinates,
checks the effective region and visible output, and transforms the result
back into layout coordinates. The constraint also distinguishes a null
region from an explicitly empty committed region. The VM retest kept
both input paths inside the secondary output with zero pointer enters
on the primary. The unconstrained control crossed normally. Real
XWayland fullscreen confinement and relative-grab simulations passed
with the packaged bridge's standard behavior.

A pre-guard build could activate and centre a pending windowed lock
while a compositor drag was being armed. The VM observed duplicate
activation and a later crash during client teardown. A red-to-green
Wayland test drives the button dispatch, drag, release, and ordinary
click sequence. The `a21396c` package passed the VM check for no lock
during the drag and activation after release. `e1cb803` extends
deferral through the release callback and explicitly tests destruction
of the newly acquired lock.
[Input routing](../../../src/tty_input.cpp) ·
[Wayland desktop fixture](../../../tests/integration/part4.cpp)

## Frame pacing investigation

The fixed-rate 512×320 Wayland client logged commits, callbacks and
presentation feedback. The dual-output trial used three counterbalanced
20-second pairs per output after five-second warmups, then one traced
pair per output. The single-output trial revisited all ten IDs on
`Virtual-1` for six seconds each after two-second warmups.

| Output | Workspaces | Presentations in three untraced pairs |
| --- | --- | --- |
| Virtual-1 | 2 and 3 | 907/876, 819/1151, 795/900 |
| Virtual-2 | 7 and 8 | 1116/1007, 556/62, 706/904 |

Workspace 8 recovered on revisit and exceeded workspace 7 in the
single-output sweep as well. Multi-second stalls occurred on several
IDs between client commits. Each traced output commit with a buffer
matched a successful composed presentation across 3,151 events, with
zero failed commits, discards, or dropped trace entries. The observation
at output commit measures a later pipeline stage than render work.
Callback delivery, buffer availability and VM scheduling remain
possible sources of the submission gaps.

A persistent workspace-specific slowdown remained unreproduced. The
single-output sweep was exploratory, and the timing evidence covers
VirtualBox and Pixman. Frame scheduling is unchanged.
[Frame client](../../../tests/integration/frame_pacing_client.cpp) ·
[Measurement runner](../../../tests/session/part12_frame_pacing.py)

## Test evidence

- The host unit and integration run at `a21396c` passed 54 Meson tests
  and 338 GoogleTest cases. The ASan and UBSan input and desktop run at
  `e1cb803` passed eight tests. The two focused fullscreen tests also
  passed ten repeats each.
- The local unsigned Arch package `seaside-1.0.0-1-x86_64.pkg.tar.zst`
  passed both Part 10 packaging checks and all three Part 12 integration
  tests at `e1cb803`. Its SHA-256 is
  `ca1d179b4222918cea2567d52d66cc0e7ace82e62ba7106d63a2b9d1ddb76843`.
- The guest used VirtualBox 7.2.22 r175245, VMSVGA with 3D off, Arch kernel
  7.3.6, wlroots 0.20.2, Xwayland 24.1.15 and Pixman. The real DRM outputs
  were `Virtual-1` at 1280×800 and `Virtual-2` at 800×600, with the second
  at x=1280. Both Linux relative mouse and absolute tablet paths were
  driven through VirtualBox. The fixtures were native Wayland and X11
  clients.
- Real key events confirmed Alt+Tab in both directions and the scratchpad
  toggle from the other output. A scratchpad window participated in the
  cycle while shown and restored its focus after hiding. Opt-in fullscreen
  lock acquisition from the other output reported surface centre
  (800, 600). Ordinary fullscreen and a region excluding the centre
  preserved their normal placement.
- The browser trial used two logging desktop entries to check startup,
  HTTP/HTTPS, HTML, config reload and last-good recovery, missing-browser
  fallback, real OpenURI routing and explicit chooser selection.
- Guest iterations used checksum-verified binaries unpacked from the
  candidate archive in a user-owned directory. `pacman -Qk seaside`
  verified the system package's files. A system package upgrade was
  outside the sudo-free guest workflow.

Raw logs, captures and traces are under
`test-artifacts/part12-baseline-emzr3tqk/`. The package and recipe logs
are under `test-artifacts/part12-package-e1cb803/`. The host summary is
`test-results/part12/20261127T233512Z-7c5a69b2/summary.txt`. The latest
sanitizer logs are `test-results/part12/sanitizer-e1cb803.txt` and
`test-results/part12/sanitizer-e1cb803-repeat.txt`.

## Resume from here

`Arch Seaside Testing` was left running with two virtual monitors.
The last launched Seaside session used the unpacked `a21396c` build in
`~/.cache/seaside-vm/part12-r75nw7fy` inside the guest. Verify the active
TTY and `seasidectl status` before sending guest input. The workspace
location of the VirtualBox windows is under the user's control.

1. Run the full host unit and integration suite at `e1cb803` or its
   documentation-only successor.
2. Stage the `e1cb803` package's `usr/bin` in a user-owned guest directory
   using the checksum and the existing sudo-free TTY workflow. The
   ignored `deploy-worker.py`, `drive-pointer.py` and fixture workers in
   the baseline artifacts directory document the tested transport.
3. Repeat the final drag and fullscreen VM checks, the VT round trip
   and clean exit to the original TTY. Check for a fresh compositor core
   dump. The earlier `ec7da1b` core was attributed to the pre-guard
   double activation. Leave the guest running with the final candidate.

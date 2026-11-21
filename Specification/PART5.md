# Part 5 Dwindle and scrolling layouts

Ordinary windows use Dwindle by default. Set `window.layout = "scrolling"` in
the startup TOML to use horizontal columns, or `window.tiling = false` to start
with floating windows. Both layouts apply to every workspace in either
workspace mode. [Part 5 plan](PART5_PLAN.md) · [Seaside design](SEASIDE_SPEC.md)

- Dwindle splits the eligible focused tile by default, with largest-tile
  insertion available in the config. Normal splits follow the region's
  longest axis. A drop honors its selected side at the current region size,
  then follows the longest axis when that size changes. Removal promotes the
  sibling subtree. Feasible client minima can reduce gaps and borders in
  constrained areas.
- Scrolling uses one independent width per column and retains order, custom
  widths, and viewport across workspace visits. Focus reveals columns by the
  smallest necessary shift. Horizontal focus stops at the first and last
  columns, with focus and the viewport staying in place. Directional navigation
  reveals an oversized column's entry edge. A global layout rebuild restores the
  original opening order and starts scrolling at the left edge. If the old
  focus falls outside that viewport, the first presented column takes focus.
- The existing `Super+D`, `Super+Left/Right/Up/Down`, and Super+mouse drags
  operate on tiles. Keyboard move, swap, and resize are configurable and start
  unbound. Dwindle keyboard resize moves the matching split boundary. A
  no-motion click preserves placement. A canceled move restores the tree or
  strip while retaining any intervening client membership changes.
- `State` owns identity and workspace membership. The window policy retains
  ordinary tile membership through maximize and fullscreen and frees the tile
  while pinned. The scene bridge sends tile configure states and sizes, and
  clips a partially visible scrolling window and its input region at the
  owning viewport. The layout policy accepts validated config changes and
  clears temporary overrides after actual global tiling or layout changes.
  File watching, full live reload, and public IPC are Part 6 work.

## Running the checks

Run `tools/run-tests --suite part5` for the layout geometry and window policy
tests and the real Wayland client scenarios. The geometry suite includes a
fixed seed for repeated layout operation sequences. The desktop fixture
checks configure, ack, late commits, resize and detach, viewport clipping
against an adjacent composed output, and pointer targets.

For the real Arch TTY trial, use the Council Seaside VM skill. Once its
background screenshot confirms an idle shell on TTY 1, run
`python3 tests/session/part5_vm.py --console-ready`. The trial uses an isolated
bundle of the current commit and a temporary guest config. It runs the
existing TTY/VT checks and both layout scenarios, recording terminal sizes,
focus and input results, and captures. The normal guest checkout is used for
the final build verification after publication.

## Local verification

The Arch WSL2 build used GCC 16.2.1, wlroots 0.20.2, Wayland 1.26.0,
and wayland-protocols 1.50. The harness reports are under `test-results/`.
The first clean build caught two fixture initializers that needed updating.
The subsequent runs compiled and exercised the corrected fixtures.

| Command | Result | Report directory |
|---|---|---|
| `tools/run-tests --build-dir build-part5-final --timeout 420` | 26 entries passed, including formatting and static analysis. All 207 GoogleTest cases passed. The real portal and passive DRM probes were skipped in WSL2. | `20261121T153552Z-e5b068f6` |
| `tools/run-tests --build-dir build-part5-clean --suite part5 --timeout 420` | Four entries and all 23 GoogleTest cases passed. | `20261121T153526Z-487cc7ff` |
| `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 tools/run-tests --build-dir build-asan --suite unit --suite integration --timeout 420` | 24 entries and all 207 GoogleTest cases passed with no sanitizer findings. The real portal probe was skipped in WSL2. | `20261121T153552Z-8fb0ff5c` |

## Arch VirtualBox verification

The Council runner built `f3d7bb9435f76a486c5020fbe9fcecb43b8dee06` on
Arch Linux `7.3.5-arch1-1` using Pixman. The guest harness passed 26 entries,
including the real portal smoke test, and all 207 GoogleTest cases. The
passive DRM capability probe was skipped.

- Dwindle started a terminal at 209 text columns and split it to 103 when
  another terminal opened. A right-button resize widened it to 125. A
  left-button drag reinserted the second terminal on the left. Clicks on
  each side verified the two terminal shell PIDs in their new positions.
- Scrolling opened three terminals, each 106 text columns wide. Pointer resize
  widened the first to 127 while its neighbor stayed at 106. A workspace round
  trip returned focus to the neighbor.
- The TTY trial reached `seaside READY cursor-plane=40`. It captured
  1280×839 frames with the same terminal shell PID before and after the VT
  round trip. Both layout sessions returned to TTY 1 with status 0. A
  forced PipeWire startup failure reported `missing(-112)` and exited with
  status 1.

The guest report, screenshots, input events, and logs are in
`test-results/part5-vm/20261121T153553Z-776cd892/`. The guest retained its
reports under `~/Projects/seaside/test-results/vm-776cd892cda0/`. The VM
exposed one usable connector. Adjacent-output clipping was checked in the
headless fixture. Native multi-monitor and GPU qualification remain Part 8
work.

## Scrolling edge navigation verification

The headless fixture at `3d5c0fc` checked that focus and viewport geometry
stay in place at both strip ends. The unit, integration, and runner suites
passed 25 entries and all 207 GoogleTest cases. The real portal test was
skipped in WSL2. The Part 5 sanitizer run passed four entries and 23
GoogleTest cases. Reports are under `test-results/20261121T155543Z-ebdd1d62/`
and `test-results/20261121T155543Z-f174ec3b/`.

The focused Arch TTY trial at the same revision opened three scrolling
terminals. `Super+Right` at the third and `Super+Left` at the first kept
their respective shell PIDs focused. Two intervening left presses visited
the middle and first terminals in order. The trial also passed the existing
resize, workspace, and VT checks. Its guest report, input events, and captures
are under `test-results/part5-vm/20261121T155635Z-e0f8260d/`.

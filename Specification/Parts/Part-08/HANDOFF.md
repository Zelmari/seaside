# Part 8 display and GPU behavior

Seaside remembers physical monitor assignments across connector changes when
the monitor identity can be matched. Automatic primary selection uses
advertised modes and stays on the selected display until it becomes
unavailable. Output settings continue to apply transactionally. Seaside
rechecks the hardware cursor plane after geometry or seat changes.
[Part 8 plan](PLAN.md) · [Seaside design](../../SEASIDE_SPEC.md)

Fullscreen uses the actual surface geometry and keeps the configured output
mode. The default `preserve` fits the content with black bars. `stretch`
fills the output. The scene supports fractional scale, presentation feedback,
DMA-BUF where the renderer can import it, and explicit DRM synchronization
where both the renderer and backend support timelines. Diagnostic queries show
the observed presentation path and capabilities. A rejected scanout or async
flip can fall back to composed or synchronized presentation.

Night light uses a hardware gamma LUT and starts off at a 4500 K target. Use
`seasidectl night-light on`, `off`, or `status`. The color ramp approximates
black-body channel gains. An output without a usable hardware LUT keeps its
current color and remains enabled. `seasidectl debug render` shows the
per-output presentation state and frame counters.

## Automated checks

- Run `tools/run-tests --jobs 2 --suite part8 --suite part6 --suite part7`.
  The Part 8 suite checks production mode selection, primary ranking, monitor
  matching and workspace restoration, adaptive-sync fallback and output
  rollback. Real headless clients check fractional-scale and presentation
  events, recovery from an injected rejected frame, fullscreen pixels and
  restoration, and popup, subsurface and pointer coordinates.
- Run `python3 tests/session/part8_vm.py --console-ready` from the host when
  the guest is logged in at an idle foreground TTY 1. The runner builds an
  exact revision in an isolated checkout. It accepts `--pkg-config-path` and
  `--runtime-prefix` for staged guest dependencies. Use `--renderer pixman`
  with the current virtual graphics configuration. The runner retains
  `guest.json`, summaries, logs and captures in the Council VM evidence
  directory and guest `test-results/`.

## Arch VM evidence from 2026-11-21

The full guest trial at `a68ff06e2301` passed 41 Meson checks and 251
GoogleTest cases. The passive DRM inventory check skipped because it had no
borrowed DRM descriptor. The real TTY separately confirmed KMS cursor plane
40, retained the same `foot` shell through a VT round trip, and returned to
the logged-in TTY on clean exit and on the induced PipeWire startup failure.

- `Virtual-1` accepted 90°, 180°, and 270° rotation with fractional scaling.
  Captures matched the transformed logical sizes. The runner observed a
  fresh KMS cursor-plane check after each transform. A bad mode edit preserved
  the configured state. The output switched to 640×480 and restored 1301×810.
- The fullscreen client produced black bars in preserve mode and filled the
  output in stretch mode. The output stayed at 1301×810. Real Wofi selection
  and PipeWire reception verified a 1301×810 monitor frame and a 264×176
  chosen-window frame. The client kept its identity and recovered focus after
  the chooser closed.
- The virtual gamma LUT accepted night-light enable and disable. The guest
  toggled it with the configured binding, changed the temperature through
  live config, and disabled it by IPC. The virtual output stayed usable. The
  VRR policies stayed fixed-refresh on the connector. Async flips were
  reported unsupported. The fullscreen sample recorded 32 composed frames
  with matching commit and presentation counts.

The guest ran kernel `7.3.5-arch1-1`, wlroots `0.20.2`, Wayland protocols
`1.50`, VirtualBox Guest Additions `7.2.22`, and `vmwgfx` DRM with Pixman.
The automatic renderer trial failed to initialize EGL and Vulkan on this
guest with 3D disabled. The passing trial selected Pixman explicitly. Its
reports are in Council VM evidence run `20261121T215400Z-5fe6932a` and guest
`test-results/vm-5fe6932a1fd6`.

## WSL2 software evidence

The unit, integration, runner and passive hardware suites passed 39 Meson
checks and 251 GoogleTest cases. The two installed portal tests skipped in
WSL2 and passed in the guest. The passive inventory check skipped in both
environments. The report is `20261121T215623Z-a774a8da`.

The separate ASan/UBSan build passed 26 Meson checks and 124 GoogleTest cases
across Parts 3, 6, 7 and 8 with leak detection enabled. The same two portal
tests skipped. The report is `20261121T215340Z-701b5fc9`. The initial
unfiltered WSL2 run lacked the staged `wlr-protocols` pkg-config path for
baseline collection and reached the 240-second aggregate code-quality
timeout. The corrected software run used that pkg-config path. Standalone
clang-tidy passed for the changed translation units.

## Native Arch qualification still open

The Framework Desktop trials cover real AMD renderer startup, cursor planes
and visual alignment, mixed-refresh hotplug and physical monitor identity,
resume, DMA-BUF import and explicit fences, direct scanout and frame pacing,
active VRR and async tearing, visible hardware night-light correction, and
Steam/Proton. Run the opt-in display evidence collector while exercising
those workloads. The existing Part 7 OBS, Discord and physical audio and key
tests remain in the checklist. [Native Arch desktop checklist](../../Research/NATIVE_ARCH_CHECKLIST.md)

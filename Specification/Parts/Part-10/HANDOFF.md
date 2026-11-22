# Part 10 Arch packaging and release qualification

`packaging/prepare-source` stages a `PKGBUILD` and checksummed source archive
from a selected Git commit. The package installs `seaside`, `seasidectl`, the
canonical configuration example, default wallpaper, portal routing and chooser
configuration, and the MIT notice. It declares the runtime dependencies for
the documented TTY workflow, including `foot`, PipeWire and WirePlumber,
Wofi, XWayland satellite, polkit for logind VT switching, and RealtimeKit
for PipeWire scheduling on clean Arch.
[Install on Arch](../../USER_GUIDE.md#install-on-arch) · [Arch systemd package](https://archlinux.org/packages/core/x86_64/systemd/) · [Part 10 plan](PLAN.md)

The README starts with installation and first use. The docs index directs
users to configuration, control, sharing, troubleshooting and support, and
contributors to builds, test reproduction and evidence sharing. The source
tree retains package recipes and intentional fixtures. Generated archives,
personal configuration, and raw test output stay in local test storage.
[Documentation index](../../README.md) · [Evidence sharing guide](../../CONTRIBUTING.md#share-changes-and-evidence)

## Checks

- `tools/run-tests --jobs 2` covers the full software suite and static analysis.
  `--suite part10` checks the staged payload and the assembly of the shared
  guest scenario. `makepkg --check` runs that focused suite against the
  staged build. The broader runtime-dependent tests run through the separate
  harness, and the installed TTY trial uses a booted Arch guest.
- The installed VM trial verified package ownership, exercised the real TTY
  and installed workflows, and checked that reinstall and removal preserved
  generated config. Use a package and checkout built from the same revision.
  [Reproduce the Arch build and installation checks](../../CONTRIBUTING.md#build-in-a-clean-arch-chroot)

## Package and software evidence from 2026-11-22

- Candidate `0.1.0dev.r140.g58eb8db5c8148ff868affdc3bc2dd9b9f361f06b-1`
  was built as an unprivileged user with `makepkg --check` in a clean chroot
  prepared from the signed Arch `2026.11.01` bootstrap. Its staged-payload
  and worker-assembly checks passed. A second chroot rebuilt the package
  offline with the retained signed dependencies, source and build epoch.
  The archives were byte-identical. SHA-256
  `b476187d2d04f3790e4cc0d195948295b2d32dfcc7bcb3fb28edde4453afef6d`.
  [Arch bootstrap downloads](https://archlinux.org/download/)
- A clean runtime root installed the same archive from the retained package
  cache. Pacman resolved polkit `128-1` and RealtimeKit `0.14-2`. Pacman found
  zero missing package paths or dependency database errors, and both
  installed binaries resolved all shared libraries. The package and exact source
  archive were checked against their recorded SHA-256 sums.
- The full WSL2 run passed 44 Meson checks including static analysis, and
  268 GoogleTest cases. The later run with the worker-assembly check passed
  44 Meson checks and 268 GoogleTest cases. The ASan/UBSan run passed 34
  checks and 157 GoogleTest cases. Installed portal and passive DRM checks
  skipped in WSL2 where those facilities were unavailable.

## Installed Arch VM evidence from 2026-11-22

- The exact `58eb8db` source and package passed the disposable VirtualBox
  trial. From TTY 1, the installed process generated the canonical TOML and
  presented the black default desktop with empty autostart. `Super+Enter`
  opened `foot`, and the same shell survived the VT round trip. The
  `vmwgfx` DRM output used Pixman at 1280×800 and presented cursor plane 40.
- Invalid config preserved the active state and the corrected file applied.
  The packaged media bindings changed the selected sink in five-point steps,
  applied the 100% cap and toggled mute. The receiver sampled 6,000 PCM
  samples with amplitude from −7,902 to 7,902. With a separate no-sink
  PipeWire instance, the actual volume command logged its failure and the
  session kept running.
- The portal backend and frontend recovered with the client still present.
  The installed Wofi chooser cancelled one request, then delivered a
  1280×800 monitor frame and a 264×176 selected-window frame through the real
  PipeWire receiver. Satellite started on the first X11 connection and
  restarted with a new X11 client after termination.
- The workload completed two mode-change cycles between 640×480 and
  1280×800, 48 workspace cycles with an unread IPC subscriber, and 151
  presentations during 45 seconds of fullscreen. The runner passed its
  resource checks, completed three further clean starts, and verified the
  forced PipeWire failure returned to the original TTY with exit status 1.
  Pacman reinstall and removal preserved the user's test config.

The guest used Arch kernel `7.3.5-arch1-1`, Mesa `26.3.1`, wlroots `0.20.2`,
Wayland protocols `1.50`, PipeWire `1.6.11`, WirePlumber `0.5.19`,
xdg-desktop-portal-wlr `0.8.4`, and Wofi `1.5.3`. The test used an isolated
guest with its own account and package database. That run skipped system
suspend because the runner lacked a verified guest wake path.

## Hardening checks from 2026-11-22

- Local Arch WSL2 at `cef0c9e` passed 46 checks, including formatting and
  static analysis, and 282 GoogleTest cases. The ASan/UBSan unit and
  integration run passed 43 checks and 282 GoogleTest cases. The installed
  portal cases skipped locally, and the passive DRM inventory skipped in
  the full run.
- The usual Arch VirtualBox guest used `vmwgfx` and Pixman. Part 5 at
  `8f52c71` exercised Dwindle split, resize and reinsert, and scrolling
  edge navigation, resize and workspace return. Part 6 at `b76132b` passed
  48 guest software checks and 282 GoogleTest cases, including all three
  real portal checks. The TTY trial confirmed a recreated TOML file applied
  only after close, with IPC ordering, recovery and restart still working.
- The guest Part 7 trial at `cef0c9e` passed lazy X11 restart, Wayland/X11
  clipboard transfer, real portal captures of the monitor and native and
  X11 windows, service-loss continuity and media controls. The playback
  monitor captured 10,800 PCM samples, and FFprobe decoded 61 H.264 frames
  from the output recording.
- Parts 8 and 9 passed at `1e201d4`. The guest exercised rotation,
  fractional scaling, mode rollback, fullscreen aspect, the virtual gamma
  LUT, layer-shell, portal and PipeWire recovery, trace export, and DRM
  display sleep and input wake. VRR and async tearing were reported as
  unsupported by the virtual output. The presentation sample recorded
  32 composed and presented frames, with zero discards.
- The resize trial at `58023b6` showed intermediate divider positions in
  both directions while the clients withheld replacement buffers. At the
  same revision, the disposable guest started without an active connector,
  mapped a Wayland client, and kept the graphical target active through
  separate portal frontend and backend restarts. A vmwgfx layout ioctl
  delivered the reconnect event after sysfs restored the connector. The
  original client and focus survived, the virtual cursor returned, and
  autostart ran once.
- The clean Arch chroot built and checked the package at `1e201d4`.
  The disposable guest passed install, first launch, VT return, media keys,
  portal capture, X11 restart, sleep and wake, and the 48-cycle workspace
  workload with an unread IPC subscriber. Package reinstall and removal
  preserved the generated config. Package SHA-256
  `685dadf5d70eeef287f1fe60f39e054ff2fb5fbb32518298e5c356c8af6b0dfb`.
- VirtualBox did not return a resume result after the ten-second RTC wake
  request and targeted keyboard input. The ACPI power button powered off
  the disposable guest. An additional virtual display also stayed unavailable
  after applying a two-display layout. Headless checks covered two-output
  window and workspace behavior.
- A local headless comparison used one output and no subscribers. It
  measured publication of unchanged state in 14 batches of 12,000
  calls per workspace count, with the changed code compiled at `-O2`.
  Median time per publication for 1, 8, and 64 workspaces went from
  3.20, 15.75, and 129.08 µs to 0.39, 3.20, and 33.48 µs. The resize
  fixture saw five output commits for five stationary pointer reports
  before the change and zero after it. These measurements cover the
  exercised code paths under the fixture workloads.

To repeat the zero-output check in a disposable VMSVGA guest, build
`tests/hardware/vmwgfx_layout.c` with the libdrm headers and install the
root-owned executable as `/usr/lib/seaside-vm-test/layout`. The guest
test account needs noninteractive sudo for that executable with the
single argument `single` and for
`/usr/bin/tee -- /sys/class/drm/card0-Virtual-1/status`. From the host
checkout, verify the disposable guest is at an idle TTY 1 and run
`tests/session/zero_output_vm.py --vm NAME --disposable --console-ready --connector card0-Virtual-1 --vmwgfx-layout-hotplug`
with Python 3.

## Native qualification

Native Arch release qualification remains open. It covers real AMD display
and cursor behavior, native performance, suspend and hotplug, physical media
keys and audio, and actual OBS, Discord, and Steam/Proton workflows. Keep the
package revision and checksum with that evidence. Required native gates must
pass before declaring a v1 release.
[Native Arch checklist](../../Research/NATIVE_ARCH_CHECKLIST.md) · [Release checklist](../../SEASIDE_SPEC.md#124-release-checklist)

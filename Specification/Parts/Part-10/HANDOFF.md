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
guest with its own account and package database. VirtualBox suspend/resume remains skipped because the runner has
no verified targeted wake path.

## Native qualification

Native Arch release qualification remains open. It covers real AMD display
and cursor behavior, native performance, suspend and hotplug, physical media
keys and audio, and actual OBS, Discord, and Steam/Proton workflows. Keep the
package revision and checksum with that evidence. Required native gates must
pass before declaring a v1 release.
[Native Arch checklist](../../Research/NATIVE_ARCH_CHECKLIST.md) · [Release checklist](../../SEASIDE_SPEC.md#124-release-checklist)

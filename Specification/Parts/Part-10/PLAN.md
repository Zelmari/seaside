# Part 10 plan for Arch packaging and release qualification

Turn the Parts 1–9 implementation into a clean Arch installation and qualify
the package for v1. The Part 9 guest trial provides a working source-built
reference for the real TTY, portal recovery, and virtual display power. The
native Framework Desktop evidence remains a release requirement.
[Roadmap](../../SEASIDE_SPEC.md#14-ten-development-parts-and-coding-agent-handoff) · [Part 9 handoff](../Part-09/HANDOFF.md)

## Delivery steps

| Step | Work | Acceptance evidence |
|---|---|---|
| 10A. Build the Arch package | Add a `PKGBUILD` around the existing Meson/Ninja release build. Audit every installed file and directly required command, library and service. Separate runtime requirements from build, check and optional tools. | An unprivileged clean Arch chroot builds the package from the candidate source. Inspect the package manifest, dependencies, paths and licences. Install with pacman into a clean runtime environment and verify required files and commands resolve there. |
| 10B. Verify the installed session | Run the packaged binaries from a logged-in foreground TTY. Exercise generated defaults, ordinary desktop use, strict service preflight, actual portal capture and failure recovery. | The installed binary starts with package-owned defaults, launches `foot` on demand, survives a VT round trip and returns to the same TTY on exit. Induced preflight failure returns a useful error to that TTY. A real chooser and receiver verify installed capture paths. |
| 10C. Extend the regression and stability harness | Add a requirement-tagged `part10` package suite and an installed-package leg for the existing Arch VM runner. Run the full Meson harness, code-quality checks, sanitizers and bounded repeat scenarios. | Reports include exact source and package IDs, commands, test outcomes, relevant logs and visual evidence, and cleanup observations. Prior parts' suites still run through the normal harness. |
| 10D. Qualify the native Framework Desktop | Install the candidate on native Arch and work through the carried-forward desktop checklist. Automate measurements and assertions where the native session allows them. | Record actual AMD display/input behavior, playback and capture in the intended applications, gaming, recovery, and reproducible performance measurements. Mark unavailable checks with their specific blocker. |
| 10E. Document the candidate and close release gates | Update install, first-use, troubleshooting, and supported-environment documentation. Reconcile the actual results against the v1 gates and record decisions and artifacts in `HANDOFF.md`. | Report package readiness and release qualification using observed evidence. Required failures or unavailable native evidence keep v1 release qualification open. |

## Build and install acceptance

- Audit the existing `seaside` and `seasidectl` targets, black wallpaper and
  desktop-matched `seaside-portals.conf`. Include the MIT notice and check
  third-party notices for any material shipped in the archive. Keep test
  fixtures and development tools out of the installed payload.
- Verify the dependency closure for `foot`, the audio stack and `wpctl`,
  X11 through `xwayland-satellite` and `xorg-xwayland`, the portal frontend,
  compatible backends and chooser/consent path, and fonts used by the config
  error banner. Resolve the concrete portal fallback and chooser from the
  installed routing and functional evidence. Exercise a clean user account
  with standard user-service activation and record any required setup.
- Write the source and artifact provenance into the qualification report.
  Capture the exact Git revision, archive hashes, Arch repository or snapshot
  and package artifacts, build flags and compiler, API and protocol XML
  hashes, dependency versions, and the built package's checksum. Extend
  `tools/record-baseline` where useful. Record any missing provenance and
  check that the source can be rebuilt from the retained inputs.
- Check that first launch atomically creates the canonical documented TOML
  and displays the installed default wallpaper. Verify empty autostart,
  `Super+Return` launching `foot`, and same-user `seasidectl` against the
  installed socket, including `seasidectl exit`. Exercise the default media
  bindings through five-point volume steps, the 100% cap, mute, and a
  missing playback device. Use a test account to verify an existing config
  is preserved across install, reinstall and removal, and that a bad edit
  recovers through the approved last-good path.
  [Package baseline](../../SEASIDE_SPEC.md#111-external-packagerelease-baseline) · [Session contract](../../SEASIDE_SPEC.md#3-startup-session-recovery-and-shutdown)

## Harness and Arch VM acceptance

- Keep `tools/run-tests --jobs 2` as the full regression entrypoint and add
  a focused `part10` suite for package-specific checks. Verify dependencies,
  installed files and package ownership, and make the guest prove that it
  launches package-owned binaries and resources. Repeat relevant lifetime,
  parser, service and IPC cases under ASan/UBSan. Keep sanitizer builds
  separate from optimized performance runs.
- Use a clean chroot for the build and a disposable VM snapshot or dedicated
  package-test guest for package installation. Use the dedicated Arch
  VirtualBox console and the existing Council runner workflow for the real
  foreground TTY trial. Verify an idle guest shell by screenshot before
  sending targeted VirtualBox input. Keep host focus, the owner's normal
  checkout, and guest config and state intact.
- Exercise repeated packaged startup and shutdown, VT return with live
  clients, one controlled mandatory-service failure at startup, and
  post-ready service interruption with a fresh monitor and chosen-window
  share through the real chooser. Run applicable repeated mode changes,
  config failure/recovery, satellite restart, slow IPC under client load,
  and longer mixed desktop/fullscreen sessions. Check input responsiveness,
  stable client identity, and owned process, socket and memory cleanup.
  Qualify suspend or connector changes in the guest where the controls can
  reliably exercise those paths.
- Preserve `pass`, `fail`, `skip` and `blocked` results with the requirement,
  actual environment and a concrete reason for each gap. Make the test
  duration or cycle count explicit for each stability scenario and retain
  the machine-readable and human-readable reports. Once the tested changes
  are ready for owner testing, push the verified commits, fast-forward the
  VM's usual checkout, rebuild `build/seaside`, and verify its exact revision
  and TTY startup through the Council runner.
  [Harness contract](../../SEASIDE_SPEC.md#122b-incremental-test-harness-and-minimum-human-involvement)

## Native evidence and release decision

- Complete the carried-forward Parts 7–9 checks on the Framework Desktop.
  Verify monitor and chosen-window sharing in actual OBS and Discord,
  desktop audio and microphone routing, physical media/Fn keys, Steam/Proton,
  hardware cursors, DMA-BUF and explicit sync, direct scanout, VRR/tearing,
  hardware night light, mixed-monitor hotplug, idle wake, and suspend/resume.
  Reserve manual work for actions or observations the harness cannot collect.
- Reuse the native display collector and opt-in traces to measure idle CPU/GPU
  work, frame timing and misses, scanout eligibility and fallback, and recovery
  with matched modes and workloads. Record actual renderer, hardware, build,
  settings and app versions. Physical input-to-photon claims require external
  measurement.
- Publish an accurate install/support guide and a gate-by-gate handoff.
  Continue packaging and guest verification while owner-deferred native
  testing is pending. A passing guest package can be handed off for testing.
  Declare v1 release-qualified only when the required checks pass in their
  specified environments. Isolated selected-application audio remains scoped
  beyond v1 under G15.
  [Native Arch checklist](../../Research/NATIVE_ARCH_CHECKLIST.md) · [Performance measurements](../../Research/PERFORMANCE.md) · [Release checklist](../../SEASIDE_SPEC.md#124-release-checklist)

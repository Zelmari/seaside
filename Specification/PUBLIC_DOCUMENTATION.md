# Verification and support

Seaside targets Arch Linux on a logged-in local console. Native release
qualification uses AMD graphics on physical hardware. A participating output
needs a driver that can put the cursor on a hardware plane. The native release
checks remain open. [Install prerequisites](USER_GUIDE.md#install-on-arch) · [Native Arch desktop checklist](Research/NATIVE_ARCH_CHECKLIST.md)

## Verification and support

| Environment | Evidence recorded | Release context |
|---|---|---|
| Arch development builds | Unit tests and real clients on offscreen Wayland outputs. Coverage includes configuration, layouts, IPC, sharing, layer-shell, idle inhibition, and trace export. | The source-tree fixtures exercise software behavior. |
| Arch VirtualBox guest | Real TTY start and exit, virtual cursor placement, application/workspace behavior, portal capture and audio routing with test clients, live config, display sleep and wake, and service recovery. The guest uses `vmwgfx` with the Pixman renderer. | Coverage is scoped to the virtual device and the cases recorded in the guest reports. |
| Disposable Arch package-test guest | The installed Part 10 package passed first use, VT return, media keys and playback, monitor and chosen-window capture, X11 restart, bounded stability, and package reinstall and removal. | The independently rebuilt candidate is ready for native testing. |
| Native Arch with AMD graphics | Test actions and the read-only display collector are available. | Real AMD renderer and cursor paths, DMA-BUF and explicit sync, scanout, VRR and tearing, night light, mixed outputs and hotplug, suspend/resume, physical audio and input, Steam/Proton, and OBS/Discord compatibility remain in the native checklist. |

The [Part 7 handoff](Parts/Part-07/HANDOFF.md) records the underlying sharing
and audio trials. The [Part 8 handoff](Parts/Part-08/HANDOFF.md) records the
virtual display and render trials. The [Part 9 handoff](Parts/Part-09/HANDOFF.md)
records portal recovery, layer-shell, virtual sleep and wake, and diagnostic
checks. The [Part 10 handoff](Parts/Part-10/HANDOFF.md) records the package
candidate, build provenance, installed guest checks, and remaining release
qualification.

The installed session provides compositing, layouts, local control, Wayland
layer-shell support, and integrations for external clients. An optional panel
or launcher can run separately. Seaside starts checking login services when the Wayland desktop becomes usable.
The session reports and retries portal, user bus and PipeWire failures at startup
and during operation.
[How to start and exit](USER_GUIDE.md#start-and-exit)

## Feature boundaries

- **Graphics.** Direct scanout, variable refresh, tearing, explicit sync and
  hardware night light depend on the output, driver and workload. A display
  without a usable hardware cursor plane is disabled. `seasidectl outputs`
  and `seasidectl debug render` expose observed state.
- **Sharing.** The portal chooser and audio routing run in external services.
  Actual OBS and Discord results belong to the native test matrix and must
  name the application version and path tested. Isolated audio for one
  selected application is deferred.
- **Session and platform.** Seaside starts in the foreground on a local Arch
  TTY. The recovery and input behavior in other environments depends on their
  seat, service manager and graphics stack. Display-manager integration and
  secure session locking are outside the current release scope.

[Performance verification](Research/PERFORMANCE.md)

## Keep the public documentation current

| Document | Purpose |
|---|---|
| [README](../README.md) | Starting point, short first session and qualification status |
| [User guide](USER_GUIDE.md) | Install, use, configure and troubleshoot the actual workflow |
| [Contributing](CONTRIBUTING.md) | Build and regression commands, package reproduction and publication policy |
| [Canonical TOML](SEASIDE_CONFIG_EXAMPLE.toml) | Complete example and defaults for the user configuration |
| [Development handoffs](README.md#development-parts) | Dated results with commands, environment, artifacts and unresolved checks |

Update examples and reader-facing instructions when a supported workflow
changes. Preserve dated results as the record of what was exercised. Close a
release gate after the required test passes in its intended environment.
[Native qualification checklist](Research/NATIVE_ARCH_CHECKLIST.md)

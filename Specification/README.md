# Seaside documentation

The [project README](../README.md) gives you the shortest path to a first
session. Find detailed workflows and verification below.

## Using Seaside

- [Install on Arch](USER_GUIDE.md#install-on-arch) and [start or exit a session](USER_GUIDE.md#start-and-exit).
- [Configure settings and shortcuts](USER_GUIDE.md#configure-seaside) or open the [canonical TOML example](SEASIDE_CONFIG_EXAMPLE.toml).
- [Control the session with `seasidectl`](USER_GUIDE.md#control-the-session).
- [Use X11 apps, audio, and screen sharing](USER_GUIDE.md#use-applications-and-sharing).
- [Troubleshoot startup, configuration, or capture](USER_GUIDE.md#troubleshoot).
- [Verification and support](PUBLIC_DOCUMENTATION.md) explains tested environments and remaining checks.

## Developing Seaside

- [Build and run the test harness](CONTRIBUTING.md#build-and-test).
- [Reproduce an Arch package in a clean chroot](CONTRIBUTING.md#build-in-a-clean-arch-chroot).
- [Share changes and test evidence](CONTRIBUTING.md#share-changes-and-evidence).
- [Desktop repair plan](Research/DESKTOP_REPAIR_PLAN.md) covers session reliability, window management, display, capture, and integration fixes.
- [Native Arch desktop checklist](Research/NATIVE_ARCH_CHECKLIST.md) and [performance investigations](Research/PERFORMANCE.md) describe hardware and measurement workloads.

## Development parts

Each part has a folder under `Parts/`. Plans describe the work and its
acceptance criteria. Handoffs record what shipped, what was tested, and what
still needs verification.

| Part | Topic | Plan | Handoff |
|---|---|---|---|
| 1 | Build, feasibility, and test harness | | [Handoff](Parts/Part-01/HANDOFF.md) |
| 2 | Configuration and state foundations | [Plan](Parts/Part-02/PLAN.md) | [Handoff](Parts/Part-02/HANDOFF.md) |
| 3 | TTY session and compositor loop | [Plan](Parts/Part-03/PLAN.md) | [Handoff](Parts/Part-03/HANDOFF.md) |
| 4 | Window and workspace management | [Plan](Parts/Part-04/PLAN.md) | [Handoff](Parts/Part-04/HANDOFF.md) |
| 5 | Dwindle and scrolling layouts | [Plan](Parts/Part-05/PLAN.md) | [Handoff](Parts/Part-05/HANDOFF.md) |
| 6 | Live configuration, IPC, and bindings | [Plan](Parts/Part-06/PLAN.md) | [Handoff](Parts/Part-06/HANDOFF.md) |
| 7 | X11 compatibility, clipboard, audio, and sharing | [Plan](Parts/Part-07/PLAN.md) | [Handoff](Parts/Part-07/HANDOFF.md) |
| 8 | Display and GPU behavior | [Plan](Parts/Part-08/PLAN.md) | [Handoff](Parts/Part-08/HANDOFF.md) |
| 9 | Idle, layer-shell, diagnostics, and recovery | [Plan](Parts/Part-09/PLAN.md) | [Handoff](Parts/Part-09/HANDOFF.md) |
| 10 | Packaging and release qualification | [Plan](Parts/Part-10/PLAN.md) | [Handoff](Parts/Part-10/HANDOFF.md) |

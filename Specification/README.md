# Seaside documentation

Start with the [project README](../README.md) for build and test commands.

## Design and configuration

- [Engineering design](SEASIDE_SPEC.md) for the product contract, architecture, and roadmap.
- [Example configuration](SEASIDE_CONFIG_EXAMPLE.toml) for the canonical TOML settings.
- [Public documentation record](PUBLIC_DOCUMENTATION.md) for user and contributor documentation requirements.

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

## Native testing and performance

- [Native Arch desktop checklist](Research/NATIVE_ARCH_CHECKLIST.md) for the main-desktop trial, including OBS, Discord, physical devices, and gaming.
- [Performance investigations](Research/PERFORMANCE.md) for planned latency, rendering, and resource measurements.

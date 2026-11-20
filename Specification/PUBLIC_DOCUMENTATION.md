# Public documentation record

Updated: 2026-11-20

Seaside is intended to be an open-source Arch Linux Wayland compositor.
This document collects approved decisions and material to explain to users
and contributors. Keep the same requirements in the
[specification](SEASIDE_SPEC.md); this companion record supplements it.
Update both when a relevant approved decision changes. Proposed additions
remain proposals until approved.

## Current implementation status

The repository currently contains the original C/wlroots part 1 foundation:
a private server lifecycle library, real-client headless fixture, bounded
Python test runner and safe DRM inspection tool. See the
[README](../README.md) for current commands and the
[part 1 handoff](PART1.md) for evidence and pending checks.

The C++23 stack below is selected for development. Migration, later compositor
features, an installable desktop session and native hardware qualification
remain pending. Documentation must distinguish selected, implemented and
verified behavior. WSL/headless results do not qualify physical AMD outputs
or establish input-to-screen latency.

## Approved technology choices

| Area | Selected approach | Specification |
|---|---|---|
| Language and build | C++23, GCC, Meson and Ninja; retain useful C fixtures | §14 |
| Compositor and rendering | wlroots, its existing scene graph/renderers, automatic renderer selection | §2 |
| Main loop and slow work | libwayland event loop owns compositor state; standard C++ `std::jthread` workers with a bounded task queue return results to it | §2 |
| Configuration | TOML 1.0 through toml++; Seaside validates the schema and applies configuration transactions | §8 |
| Local control | JSON Lines over a Unix stream socket, using nlohmann/json; `seasidectl` is a thin client | §10 |
| D-Bus | Direct sd-bus from libsystemd with small C++ ownership wrappers and asynchronous main-loop integration | §3.1 |
| Audio integration | libpipewire for the required startup connection check; external PipeWire/WirePlumber services and `wpctl` controls | §3.1 |
| Wallpaper | Static PNG/JPEG/WebP via libpng, libjpeg-turbo's TurboJPEG API and libwebp; reuse decoded content while unchanged | §2 |
| Configuration-error banner | Pango and Cairo/PangoCairo text rendering; wlroots displays the cached image | §8.1 |
| Tests | GoogleTest for C++ unit/component tests, Meson execution, Python-standard-library supervision and useful C fixtures | §12.2b |
| Dependencies | Arch packages by default, compatible version constraints, exact tested manifests; explicit bundled-copy exceptions and no silent downloads | §11.1 |
| Error handling | `std::expected` for recoverable failures, RAII cleanup, exceptions contained at C callback and worker boundaries | §2.2 |
| Development tools | clang-format, clang-tidy, GDB and AddressSanitizer/UndefinedBehaviorSanitizer test builds; GCC remains primary | §14.3 |

The exact compatible versions and tool configurations must be qualified during
implementation. The specification retains library-specific constraints,
including the initial toml++ release to qualify. The portal backend/chooser
and font baseline still need resolution and evidence.

## Contributor conventions to explain

- Comments explain non-obvious decisions, invariants, ownership, ordering and
  workarounds. Clear code carries ordinary syntax; comments do not narrate it.
- Implement approved behavior in the ten testable development parts, extending
  meaningful acceptance/regression coverage with each part.
- Report commands and actual results, separating simulated, headless and native
  hardware evidence. Record unresolved qualification explicitly.
- Follow [the Git workflow](../skills/GIT.md): Conventional Commit subjects and
  PR/MR titles, concise descriptions with important review details, meaningful
  coherent local commits and genuine human attribution. Agents do not add
  themselves as contributors. External Git actions need task authorization.

Formatting rules, static-analysis configuration and exact tool versions are
still to be established during implementation; selecting tools is not evidence
that their checks already run.

## Public documentation work to carry forward

| Topic | Existing source and remaining work |
|---|---|
| Project introduction and support limits | README and specification §§1–3; keep current prototype status visible and describe Arch/TTY use when the session works |
| Build and contributor setup | README, part 1 handoff and §§11.1/14; extend with qualified C++ dependencies, tool versions and check commands during migration |
| Configuration and defaults | §§8–9 and the [reference TOML](SEASIDE_CONFIG_EXAMPLE.toml); document actual reload/recovery behavior and keep examples synchronized |
| Window/workspace behavior and bindings | §§4–9; explain implemented interactions and defaults as their acceptance tests pass |
| CLI and integration reference | §10; document implemented commands, JSONL schemas, errors and version behavior |
| Audio, X11 and screen sharing | §§3/7/11; explain external-service requirements and tested client/backend combinations; isolated application audio remains deferred beyond v1 |
| Troubleshooting and diagnostics | §§3/8/10/12; document startup errors, persistent config errors, bounded opt-in diagnostics and reproducible bug-report information |
| Performance and hardware support | §12 and [performance investigations](Research/PERFORMANCE.md); publish measured results with methodology and limitations when available |
| Packaging and releases | §§11/14; document clean-install dependencies, tested versions and remaining limits once packaging is verified |
| Licence and third-party notices | Project licence is not yet selected and no root licence file exists; record the owner's choice and preserve dependency/asset notices when preparing the release |

These entries track documentation for existing scope. They do not approve new
features, a documentation website, a hosting service or a new release workflow.

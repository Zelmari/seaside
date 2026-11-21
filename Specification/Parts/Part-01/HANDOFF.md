# Part 1: build, feasibility and harness foundation

This records the software foundation for the first development part in §14 of
[the Seaside design](../../SEASIDE_SPEC.md).

The foundation was originally implemented in C/wlroots/Meson. On 2026-11-20
the lifecycle core was migrated to C++23 with a narrow C interoperability bridge,
RAII ownership, typed errors, GoogleTest and development code checks. The
original 2026-11-19 evidence below remains historical; see the C++ migration
section for migration-checkpoint verification. Subsequent configuration/state
work is recorded in [the part 2 handoff](../Part-02/HANDOFF.md).

Requirements covered are the shared event-loop/lifetime boundary (§2), bounded
test tooling and isolation (§12.2b), and actual build/API evidence for G1 (§13).
It does not implement a desktop session, window management, production IPC or
the strict external-service preflight.

## Components

- `src/server.hpp` / `server.cpp`: the private C++ lifecycle owner. The caller
  selects the backend and renderer; ownership and teardown order are documented
  at the interface.
- `src/wlroots_bridge.[ch]`: thin C wrappers around the wlroots calls needed by
  the owner. wlroots 0.20.2 headers contain C-only array parameter syntax, so C++
  uses forward declarations instead of including or modifying those headers.
- `src/server.h` / `server_c.cpp`: a nonthrowing C facade with borrowed handles,
  allowing the retained real-client fixture to exercise the C++ implementation.
- `tests/unit/server_test.cpp`: GoogleTest cases for real headless/Pixman startup,
  typed failures, ownership transfer and ordered/idempotent teardown. GNU linker
  wrapping injects failures only into this executable, not into the core API.
- `tools/check-code`, `.clang-format`, `.clang-tidy`: formatting checks for the
  migrated core/new unit fixtures and static analysis of all project translation
  units. The code checks run through the normal Meson/runner path.
- `tests/integration/headless.c`: a test-only executable that creates offscreen
  outputs, connects a real Wayland client, checks submitted SHM content through
  the renderer, removes/replaces outputs, exercises zero-output survival and
  releases clients/resources. Its serving mode is a fixture lifecycle aid,
  not a flag or alternate launcher in a shipped compositor.
- `tools/run-tests`: the development runner. It coordinates Meson commands,
  bounds runtime/log capture, cleans up owned processes, classifies actual test
  outcomes and writes run-specific JSON/human-readable reports. Its subprocess
  helper requires exclusive serial ownership: it blocks before launching a
  command if the calling process already has children. A process-local Linux
  subreaper keeps detached test descendants available for cleanup; it changes
  no system service or persistent setting.
- `tools/record-baseline`: records installed package versions, relevant cached
  package hashes, wlroots headers/libraries, protocol XML hashes, actual Meson
  build metadata and source fingerprints. Missing inputs are explicit errors or
  missing values. The record is an observation, not a fabricated Arch snapshot
  or a guarantee that every old package can be reconstructed indefinitely.
- `tools/probe-drm.c`: development-only node discovery and inherited-descriptor
  KMS capability inspection. It makes no modeset, master, cursor or client-cap
  changes. Inspect `--help` for the exact descriptor preparation contract.

## Environment and qualification boundary

The available development environment on 2026-11-19 is Arch Linux on the
`6.18.58.1-microsoft-standard-WSL2` kernel. Results from it are labeled
`arch-wsl2`; they are not full Arch-VM or native Framework/AMD qualification.
The original VM-first test plan still requires those environments' evidence.
Available software verification can proceed without representing that evidence
as present.

The observed package baseline is:

| Component | Installed version |
|---|---|
| wlroots | `wlroots0.20 0.20.2-1` (`wlroots-0.20.pc`: `0.20.2`) |
| Wayland | `1.26.0-1` |
| Wayland protocols | `1.50-1` |
| libdrm | `2.4.134-1` |
| pixman | `0.46.4-1` |
| Meson | `1.12.1-2` |
| Ninja | `1.13.2-3` |
| GCC | `16.2.1+r23+gd564253eb6c8-1` |

The build accepts the wlroots 0.20 API family starting at 0.20.2; each actual
build's manifest records the precise installed versions, inputs and exact
compile commands.
Recording protocol XML hashes does not assert that all those protocols have
been implemented or passed compatibility tests.

## DRM inspection

Run the observer separately with:

```sh
build/seaside-probe-drm
```

Opening a DRM primary node can implicitly acquire master even when opened
read-only ([kernel DRM master documentation](https://www.kernel.org/doc/html/latest/gpu/drm-uapi.html#primary-nodes-drm-master-and-authentication)).
The observer therefore does not open primary nodes itself. Default
discovery can report nodes and sysfs facts, but returns unavailable when safe
plane inspection is not possible. A session/test supervisor may supply an
already-open descriptor under the tool's documented preparation contract.
Caller-supplied assertions are labeled as such rather than treated as verified
driver state.

Cursor-plane discovery is only a capability observation. A working hardware
cursor across scale/rotation/VT/resume still needs real display tests. The
currently exposed `card0` resolves to a `vgem`/faux sysfs device; it is not native
Framework/AMD evidence. Never interpret a blocked probe as proof that a device
lacks cursors, and never replace it with a software-cursor qualification.

## Verification status

The software foundation is `implemented`; the available Arch WSL2 checks are
`automated_verified`. Overall part 1 remains `qualification_pending` until the
full Arch-VM build/run and guest cursor-capability observation are available.
The runner's `software_status` and `part1_qualification` fields preserve this
distinction even when every available software test passes.

Checks run on 2026-11-19:

- `meson setup build --buildtype=debug` and `meson compile -C build`: pass
  with GCC warnings treated as errors.
- `tools/run-tests`: three passing Meson entries and one hardware skip
  (exit 77). These include three real-client lifecycle rounds, 17 runner
  regression tests and six DRM-probe contract tests.
- `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
  tools/run-tests --build-dir build-asan`: the same results in the
  AddressSanitizer/UBSan build, with no sanitizer findings.
- The baseline collector records 125 wlroots API/library fingerprints and 58
  protocol XML hashes with no collection errors. A deliberately missing build
  directory returns exit 1 with valid JSON describing four missing metadata
  inputs; simulated required hash/tool failures also remain errors.
- A fresh suite-only Meson invocation builds its required probe executables
  and passes. `WAYLAND_SOCKET=999999` does not redirect the fixture from its
  private socket; all three lifecycle rounds still pass.

The normal and sanitizer run IDs are `20261119T232903Z-32b23218` and
`20261119T232915Z-caee4a00`. Each run's ignored `test-results/<run-id>/`
directory contains `summary.json`, `summary.txt`, `baseline.json` and bounded
logs. Rerun the commands to generate evidence on another machine; these local
artifacts are not required source files.

## C++23 migration — 2026-11-20

The lifecycle owner is now C++23, using `std::expected` for startup failures and
RAII for display/backend/renderer/allocator ownership. The thin C bridge builds
the installed wlroots headers as C17. This resolves their incompatible C array
parameter syntax without modifying headers or redefining language keywords.
The existing C integration fixture links the C++ core through the C facade.
Backend/renderer selection remains the caller's responsibility; headless/Pixman
selection stays test-only.

Current additional tool/dependency versions:

| Component | Tested version |
|---|---|
| C and C++ compiler | GCC `16.2.1+r23+gd564253eb6c8-1` |
| Standard library | `gcc-libs 16.2.1+r23+gd564253eb6c8-1`; recorded `libstdc++.so.6.0.36` artifact |
| GoogleTest | `1.18.0-1` |
| clang-format / clang-tidy | `clang 23.1.5-1` |
| GDB | `18.1-2` (version recorded; no interactive debugging needed) |

Verification:

- `tools/run-tests --build-dir build-cpp`: clean configure/build, baseline
  collection and tests passed. **Five Meson entries passed, one hardware probe
  skipped**, with no failures or blocked software checks.
- `meson setup --reconfigure build-asan --buildtype=debug -Dcpp_std=c++23
  -Dwrap_mode=nodownload -Db_sanitize=address,undefined`, followed by
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1
  tools/run-tests --build-dir build-asan`: the same result, with no sanitizer
  findings.
- These runs include six GoogleTest cases, three real-client lifecycle rounds,
  the retained 17 Python runner cases and six DRM-probe cases, plus formatting
  and static analysis. Test-only linker wrapping simulates display/startup
  failures and observes teardown order; it does not qualify hardware failures.
- `meson setup build-default-check --buildtype=debug`: fresh configuration
  selected C++23 and disabled dependency downloads without extra flags.
- Both baseline reports contain configured C/C++ compiler observations, the C++
  standard-library hash, GoogleTest/tool versions and check-configuration hashes,
  with no collection errors.

Normal run: `20261120T125935Z-3278cedc`; sanitizer run:
`20261120T125939Z-2634efa4`. Evidence remains under ignored `test-results/`.
Initial legacy-build compilation failed because Meson retained `cpp_std=none`;
the runner now explicitly reconfigures it to C++23. Initial static analysis
flagged ordinary C `fprintf`/`memset` calls under its blanket Annex K migration
check. That specific check is excluded with a configuration comment because
glibc does not supply those `_s` APIs; the other analyzer checks remain enabled.

The C++ migration is implemented and software-verified in Arch WSL2. The
full-VM, actual guest cursor observation, native hardware and session gates
remain pending. This checkpoint preceded part 2; its later implementation and
evidence are recorded in [the part 2 handoff](../Part-02/HANDOFF.md).

## Reproduce and extend

Use the commands in the root `README.md`. For a sanitizer build, use Meson's
built-in sanitizer option in a separate build directory:

```sh
meson setup build-asan --buildtype=debug -Db_sanitize=address,undefined
meson compile -C build-asan
meson test -C build-asan
```

When reusing a pre-migration build directory, reconfigure it with
`-Dcpp_std=c++23 -Dwrap_mode=nodownload`; the test runner does this automatically.

To collect baseline inputs independently of the runner:

```sh
tools/record-baseline --build-dir build --output test-results/baseline.json
```

Add later acceptance cases to the existing production modules, fixture clients
and Meson suites. Keep controls for simulated time/input/failures in test-only
adapters. Part 2 should add the TOML/default/state foundation and its regression
cases; no part 1 placeholder advertises unimplemented user functionality.

## Remaining qualification

- Repeat the clean build/fixture/runner checks in the designated full Arch VM.
- Obtain an actual guest KMS/cursor capability observation through an authorized
  descriptor; virtual driver behavior remains distinct from physical hardware.
- Preserve G2 and the later protocol, session, GPU and native-hardware gates.
- Establish a fixed Arch snapshot/complete rebuild inputs before describing the
  baseline as a reproducible release lock.

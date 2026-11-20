# Part 2 plan: configuration and state foundations

Status: implemented and software-verified in Arch WSL2; static review completed.
Designated VM/native qualification remains pending. Actual modules,
commands, evidence and limitations are recorded in [the Part 2 handoff](PART2.md).

This implements the existing [specification](SEASIDE_SPEC.md), especially
§§2.2, 8–9, 12.2b and 14. It adds the configuration/state foundations and their
tests. It does not authorize additional settings, bindings or runtime features.
The [part 1 handoff](PART1.md) records the earlier foundation evidence.

## Delivery steps

Each step includes its relevant tests and documentation before its meaningful
local commit. Extend shared fixtures when a concrete case needs them; do not
build a speculative testing framework first.

| Step | Implementation | Evidence required with the step |
|---|---|---|
| 2A. Typed configuration and defaults | Integrate and qualify toml++ in strict TOML 1.0 mode. Implement the approved schema, typed values, defaults and bounded diagnostics with source locations. | Parse the reference configuration and generated template through production code; check each documented field's default, allowed types, enums and boundaries. Record actual parser/version/build options. |
| 2B. Bindings, actions and selectors | Parse/normalize the existing chord/gesture grammar, approved action arguments and monitor selectors. Represent actions without executing them. | Every approved action form; invalid/missing/extra arguments; duplicate normalized chords; incompatible gesture/action pairs; complete-map replacement and monitor inheritance cases. |
| 2C. Bounded asynchronous work | Implement the selected standard C++ workers/task queue and lifetime-safe main-loop result handoff needed for slow configuration work. | Saturation without blocking main-loop submission; failures/cancellation; stale generations; teardown with queued/in-flight work; main-loop ownership. Use controlled synchronization and deadlines rather than guessed sleeps. |
| 2D. Loading, transactions and recovery | Implement bounded file loading, first-use generation, last-good persistence and the active/requested configuration state machine. | Real temporary-directory tests plus controlled I/O failures; atomic logical commits; recovery precedence; restart-only mode; snapshot failure after a valid commit; old work cannot replace newer accepted state. |
| 2E. Window/workspace state foundations | Add the minimal pure state/identity/ownership model needed by the approved object lifecycle and later configuration effects. | Stale/destroyed references, membership consistency, empty/zero-output state and generation/lifetime checks. Implement later window-management behavior in its assigned part. |
| 2F. Integrated acceptance and handoff | Exercise these production modules together through the development harness, update baseline collection and document exact checks/limits. | Normal and sanitizer runs, existing regressions, requirement-linked reports, source/dependency evidence and reproducible failure cases. Separate software completion from pending VM/native qualification. |

Actual window layout/focus policy remains in parts 4–5. Real editor watching and
live graphical reload integration remain in part 6. Part 2 tests the loader and
transaction mechanisms directly. The configuration-error banner is represented
by its diagnostic/state contract here; visible Pango/Cairo rendering is qualified
when the graphical integration exists.

## Small cases that must be covered

### Parsing and schema

- For every setting: omission/default, accepted types, wrong scalar/array/table
  types, valid enum values, unknown values and unknown nested keys.
- For each specified numeric range: the endpoints and just outside them; reject
  nonfinite values and unsafe conversions. Distinguish booleans, integers and
  floats where the schema does. Reject representational overflow safely.
- Valid Unicode, invalid UTF-8, truncated TOML, duplicate declarations and the
  1 MiB file-size boundary, with useful bounded diagnostics. Empty valid TOML
  supplies compiled defaults, including the omitted-table binding defaults.
- Parse generated defaults with the real parser and independently compare them
  with the approved reference. Avoid deriving expected results from the same
  implementation being tested.

### Bindings and monitor configuration

- Omitted `[keybinds]` uses defaults; present empty table disables all bindings;
  present nonempty table replaces the entire map. Deleting a default really
  disables it. Repeated reloads do not accumulate bindings.
- Modifier permutations/conflicts, missing primary keys, valid/invalid XKB names,
  supported equivalence rules, gestures and all approved action argument forms.
- Preserve shell-command strings as data: parsing/reloading must not run them.
- Exact selector grammar, inherited versus overridden fields, duplicate or
  ambiguous live matches, and disconnected selectors that remain dormant.

### Transactions, recovery and concurrency

- Missing/valid/invalid user configuration crossed with missing/valid/invalid
  recovery snapshots; never repair or overwrite invalid user configuration.
- First-use creation losing a race to another writer preserves that writer's
  file. Failed generation uses defaults and reports the failure.
- Invalid candidates change neither active settings nor the durable snapshot.
  A successful commit followed by snapshot-write failure keeps active settings
  and the previous durable snapshot, with an accurate persistence diagnostic.
- Exercise short/interrupted reads/writes, denied access and failures of the
  temporary-file, flush/sync and replacement operations. Verify cleanup and
  restrictive recovery-file permissions without relying on the host disk filling.
- Requested workspace mode can differ from active mode until restart; recovery
  retains requested values. Unrelated edits preserve runtime night-light state.
- Only committed changes to global tiling/layout request temporary-override
  resets. Unrelated/rejected edits do not. Reload never launches autostart.
- Reordered completions, destroyed recipients, queue saturation and shutdown
  while work is pending cannot cause stale commits or dangling accesses.

Stop requests are cooperative: `std::jthread` does not forcibly interrupt
blocking filesystem/library calls. Design and test worker shutdown accordingly;
do not claim bounded cancellation merely because a thread has a stop token.

Schema validity is distinct from successful graphical preparation/application.
Use explicit internal collaborators for output/wallpaper preparation failures;
label simulated results. They do not qualify actual image decoding, DRM state
changes or rollback, which need their real module integrations later.

## Harness improvements alongside the code

- Register GoogleTest cases through Meson and retain the Python supervisor for
  deadlines, isolation, bounded logs and owned-process cleanup.
- Make reports part-aware. The current runner labels reports as part 1; add
  requirement/test identifiers and retain per-part qualification boundaries so
  passing configuration tests cannot imply that hardware gates passed.
- Preserve individual GoogleTest case results and their failure diagnostics in
  the consolidated report, rather than reporting only one aggregate executable.
  Test the reporting path with intentional failures, missing/empty test discovery,
  malformed result files and cleanup errors; none may become a false pass.
  Code checks must reject a missing/empty applicable compilation database.
- Add reusable test-owned config/state directories, controlled clocks/work
  completion, and targeted filesystem failure injection. Run real filesystem
  integration cases too, so injected collaborators are not the only evidence.
- Add table-driven field/grammar cases and bounded generated malformed-input
  and state-transition sequences using recorded seeds. Failures must retain a
  reproducible input/sequence. Use existing tools; no new fuzzing service or
  framework is selected by this plan.
- Expand formatting/static analysis to new sources and extend baseline evidence
  for newly integrated dependencies. Review untested branches with available
  GCC coverage tooling; coverage counts are a gap-finding aid, not proof of
  correctness or a replacement for requirement assertions.
- Exercise worker/handoff code in a separate GCC ThreadSanitizer run where
  supported, alongside deterministic concurrency cases. Do not combine it with
  AddressSanitizer; record unavailable instrumentation as a limitation, not a
  pass. See [GCC instrumentation options](https://gcc.gnu.org/onlinedocs/gcc/Instrumentation-Options.html).
- Every fixed defect gets a focused regression case. Every requirement gets
  observable assertions, including failures, boundaries and relevant ordering
  combinations. Avoid test-per-function quotas and implementation-copy tests.

## Completion boundary

Part 2 software completion requires the new acceptance cases and relevant
part 1 regressions to pass normally and under appropriate sanitizer checks,
with code checks and error-free baseline collection. Record exact commands,
versions, inputs/seeds and remaining limitations in the handoff.

The available Arch WSL2 environment can exercise this software work. Repeat
required checks in the designated full Arch VM when available; retain that
qualification as pending until tested. Native display/input/GPU claims need
their later hardware evidence. These tests should require no routine human
interaction and must not add a production test mode or weaken session preflight.

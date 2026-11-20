# Part 2: configuration and state foundations

Status: implemented and software-verified in Arch WSL2; independent static review
completed. Designated Arch-VM and native/session/hardware qualification pending.

This implements [the Part 2 plan](PART2_PLAN.md) against the existing
[specification](SEASIDE_SPEC.md), particularly §§2.2, 8–9, 12.2b and 14.
It is a library/test foundation; there is still no installed `seaside` session.
Part 3 has not been started.

## Implemented modules

| Module | Responsibility |
|---|---|
| `src/config.hpp/.cpp` | Typed approved settings/defaults, strict TOML 1.0 validation, bounded source diagnostics and monitor override inheritance |
| `src/config_template.inc` | Compiled first-use template; tested byte-for-byte against the independent reference TOML |
| `src/bindings.hpp/.cpp` | libxkbcommon keysym names, modifier/chord normalization, typed approved action arguments and exact output selectors; no command execution |
| `src/config_store.hpp/.cpp` | Bounded stable-file reads, no-overwrite user-file creation and atomic recovery-file publication with real short/interrupted-I/O and fault-test collaborators |
| `src/config_transaction.hpp/.cpp` | Startup recovery precedence, logical requested/active configuration, runtime night-light preservation, restart-only workspace mode, main-loop commit effects and asynchronous load/save handoff |
| `src/work_queue.hpp/.cpp` | Bounded C++ workers, eventfd/libwayland completion delivery, declared memory reservations, lifetime/generation checks and cooperative shutdown |
| `src/state.hpp/.cpp` | Window/workspace/output identities, membership/lifetime invariants, initial mapping order, zero-output survival and temporary tiling-override clearing |

Configuration reads/parsing/publication are worker-side operations. The
asynchronous adapter delivers results to the owning main loop; it does not
mutate compositor state from worker threads. Its dedicated queue stays at
transport generation zero: reload-attempt generations filter parsed candidates,
while accepted revisions govern snapshot publication. A newer invalid edit
must not cancel persistence of an earlier accepted configuration.

Actual output/wallpaper preparation remains a required collaborator of the
logical commit. Tests label simulated preparation; they do not qualify DRM
changes, image decoding or visible error-banner rendering. Editor watching and
graphical live-reload integration remain in part 6. Focus/layout policy, real
window management, service preflight and IPC remain in their assigned parts.

## Internal limits and failure semantics

- Configuration input is at most 1 MiB. A lexical depth guard bounds compound
  TOML table/value paths before DOM construction; no approved schema path needs
  that depth. Diagnostics cap path/field/message lengths and preserve valid
  UTF-8 truncation boundaries.
- toml++ 3.4.0 is compiled with `TOML_ENABLE_UNRELEASED_FEATURES=0`. Schema
  validation distinguishes integer/boolean/string types and checks unsafe
  integer-to-floating conversions before accessing conversion results.
- Queue defaults are two workers, eight outstanding tasks, a 128 MiB declared
  reservation budget and four completion callbacks per dispatch. Configuration
  parsing reserves 64 MiB conservatively per job. Admission does not wait for
  capacity. Reservations cover queue-owned declared payloads, not arbitrary
  library allocations, thread stacks or a process-wide RSS cap.
- Stop requests are cooperative. All workers are joined before removing their
  main-loop source/FD. Blocking filesystem calls are not magically interrupted
  by `std::jthread`; no hard I/O cancellation bound is claimed.
- User configuration symlinks are supported; private recovery-file symlinks and
  nonregular configuration sources are rejected. First-use publication does not
  overwrite a competing writer. New file/directory permissions are restrictive;
  new directory chains and file publication are synchronized.
- Before publication, errors keep the previous snapshot. A successful rollback
  restores it after a failed directory sync. Failed rollback or a concurrent
  writer is reported as recovery uncertainty; retain the old backup when safe.
  A second synchronization failure cannot guarantee crash durability. A cleanup
  warning after successful publication preserves the known persisted status
  and reports the private leftover separately.
- Logical commits retain active workspace mode until restart, store requested
  mode separately, preserve runtime night-light overrides across unrelated
  edits, and emit override-reset effects only for committed tiling/layout
  changes. Configuration parsing/reloading never executes shell commands.

## Harness changes

GoogleTest executables declare their part/requirements in Meson. The Python
runner independently lists their cases under the existing process supervisor,
then compares actual execution XML against that discovery. Reports retain
individual case results/properties alongside aggregate Meson rows and per-part
software status. Runtime-skipped and disabled cases remain skips.

The runner rejects missing/empty/contradictory test discovery, malformed or
oversized XML, unsafe XML file types, non-UTF-8/entity-bearing XML, inconsistent
suite totals, duplicate case IDs and contradictory successful exit metadata.
Ambient GoogleTest filters/shards are cleared, and manifest overrides cannot
silently shrink the expected case set. All subprocesses retain deadlines,
bounded logs and owned-process cleanup.

`tools/check-code` rejects empty/inapplicable compilation databases. It checks
the migrated core/new unit fixtures with clang-format and analyzes all project
translation units with at most two concurrent Clang processes. The increased
suite uses a 300-second runner command deadline and 240-second Meson quality
test deadline; per-file analysis remains bounded at 60 seconds.

The baseline includes exact toml++/libxkbcommon versions, header/library hashes,
compiler flags, tool versions and source/check-configuration hashes. It remains
observed evidence, not an Arch snapshot or complete reproducible rebuild lock.

## Verification

The final implementation passed these checks in Arch WSL2:

| Check | Command / result |
|---|---|
| Full normal suite | `tools/run-tests --build-dir build`: seven Meson entries passed, one hardware skip; 88 GoogleTest cases passed |
| Full ASan/UBSan suite | `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 tools/run-tests --build-dir build-asan`: same result, no sanitizer findings |
| Separate race instrumentation | `TSAN_OPTIONS=halt_on_error=1 meson test -C build-tsan --suite part2 --print-errorlogs`: both Part 2 executables passed |
| Harness self-tests | 50 Python cases run through Meson, including the real-client fixture environment |
| Earlier real-client/probe regressions | Three headless lifecycle rounds and six DRM probe contract cases retained |
| Code and dependency evidence | Formatting/static analysis and independent GoogleTest discovery passed; baseline collection reported no errors and recorded 56 supporting API/library artifacts |

Final full reports: `20261120T142054Z-c7c4e35d` (normal) and
`20261120T142054Z-8589a042` (ASan/UBSan), under ignored `test-results/`.
The initial implementation is committed as `3de2b18`; subsequent fixes are
`964577b` (snapshot/worker lifetime) and `937562c` (harness/process cleanup).
The earlier pre-review reports remain available as
`20261120T135108Z-cca9ed02` and `20261120T135109Z-b1b7b0d8`.

GCC coverage was inspected using `-Db_coverage=true` and native `gcov` JSON
output to identify untested parser error propagation and graph-rejection cases;
those cases were added as targeted regressions. Coverage is diagnostic evidence,
not a claim of exhaustive correctness or a substitute for behavior assertions.

The requested external static review was completed through
`agy --model gemini-4-neon-high --effort high`, using fixed source/requirements
packets. Its first attempt could not read files because headless command
permission was denied; direct packets required no tool access or CLI permission
changes. This was static inspection, separate from the automated test evidence.

Confirmed findings were fixed and regression-tested: overlapping snapshot writes
now serialize; failed admission does not cancel an older accepted save; callback
captures retire on the owning main loop outside the queue mutex; and unexpected
save-job failures retain path/uncertainty information. Tests now guard result
variants, check the actual interrupted read, and exercise live stale/dead-recipient
dispatch rather than relying on shutdown to suppress callbacks.

Harness fixes cover alternate filter/output environments, contradictory counts
and exit statuses, global failure propagation, threaded descendants and safe
process signaling. Linux pidfds pin live process identities and are retired
as processes finish, avoiding both PID-reuse signaling and descriptor buildup.

Follow-up runtime and harness responses reported **no confirmed remaining
P0/P1/P2 findings in the reviewed code**. Prompts, source hashes and responses
are retained under ignored `test-results/part2-review/`; this is scoped review
evidence, not a guarantee that no defects remain.

The first full integrated runs timed out at the previous 180-second command
limit during serial static analysis. Runtime C++ cases had passed. Analysis now
uses bounded parallelism and the expanded deadlines above; those failed runs
are retained rather than relabeled successful.

Full Arch-VM and native/session/hardware qualification remain pending. Arch
WSL2 software results do not establish hardware cursor, display latency, actual
mode rollback or service integration behavior.

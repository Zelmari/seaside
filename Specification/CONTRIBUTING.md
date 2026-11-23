# Build, test, and contribute

Use an Arch Linux development environment for the GCC C++23, Meson and Ninja
build. Work from the root of a Git checkout. The package recipe declares the
runtime libraries and commands, build dependencies, and test dependencies.
The [package build instructions](USER_GUIDE.md#build-and-install-from-source)
prepare those dependencies on Arch. Install `clang` for clang-format and
clang-tidy when running the full development suite. GDB is useful for
interactive debugging.

## Build and test

```sh
meson setup build --buildtype=debug
meson compile -C build
meson test -C build --print-errorlogs
```

These commands configure a debug build, build the tools and fixtures, and run
the registered tests. Use `meson setup --reconfigure build` when changing
options in an existing build directory. Meson resolves system dependencies
locally and uses `wrap_mode=nodownload`. The installed session workflow starts
from the Arch package so it can use the package-owned wallpaper and portal
configuration. [Install and run](USER_GUIDE.md#install-on-arch)

Use the project runner for regression evidence. It configures and builds,
collects the dependency baseline, discovers individual GoogleTest cases, runs
Meson tests, and retains bounded results in `test-results/`. A full run
includes formatting and static analysis. Use `--suite` to select a Meson
suite and `--jobs` to cap build and test concurrency.

```sh
tools/run-tests --jobs 2
tools/run-tests --jobs 2 --suite part10
```

Use `--buildtype release` or `--buildtype debugoptimized` with a separate
`--build-dir` to test an optimized build. On a development host with the Arch
portal packages staged in a separate root, set `SEASIDE_TEST_PORTAL_PREFIX`
to that root's `usr` directory. The three real portal checks then use the
staged binaries and report a failure if a required tool is missing.

Read the run's `summary.txt` first. `summary.json` records the same outcomes
in machine-readable form and links to more detailed logs. Use
`tools/check-code --build-dir build` to rerun formatting and static analysis
with the build's compilation database. For an interactive debugging session,
run `gdb --args build/seaside-server-tests` after building that target.

Use a separate build directory to rerun parser, state and lifetime checks
under AddressSanitizer and UndefinedBehaviorSanitizer.

```sh
meson setup build-sanitize -Db_sanitize=address,undefined
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  tools/run-tests --build-dir build-sanitize --jobs 2 --suite unit --suite integration
```

Record the relevant case IDs and outcome (`pass`, `fail`, `skip`, or
`blocked`) and keep any reason for a missing result. Offscreen tests cover
software and client protocols. The installed TTY fixture covers the real
guest session. Use the recorded evidence for each environment when describing
support. [Verification and support](PUBLIC_DOCUMENTATION.md#verification-and-support)

On the dedicated Arch VM, inspect the console and confirm TTY 1 shows an idle
logged-in shell before each trial. These repository runners use the Council VM
transport to build the checked-out revision and send input to the guest.

```sh
python3 tests/session/repair_services_vm.py --console-ready --tty-only --sanitize
python3 tests/session/part7_vm.py --console-ready
python3 tests/session/part9_vm.py --console-ready --tty-only
```

The service trial checks activation, recovery after a killed compositor,
diagnostics during shutdown and a login with an unavailable integration. The
Part 7 and Part 9 trials exercise clipboard transfers, portal video streams,
monitor sleep and wake, and typing across a VT switch. Each runner prints the
directory containing its guest log and results.

## Build in a clean Arch chroot

Use Arch's `devtools` and a fresh root with `base-devel` for a package
qualification run. `mkarchroot` initializes the base root. Run the preparation
script from the project checkout, then run `makechrootpkg` from the resulting
staging directory. It uses a clean working copy and runs `makepkg` as an
ordinary user. `makepkg --verifysource` checks the staged source archive before
building. [mkarchroot manual](https://man.archlinux.org/man/mkarchroot.1.en) · [makechrootpkg manual](https://man.archlinux.org/man/makechrootpkg.1.en) · [makepkg manual](https://man.archlinux.org/man/makepkg.8.en)

```sh
sudo pacman -Syu --needed base-devel devtools git
chroot=$(mktemp -d)
sudo mkarchroot "$chroot/root" base-devel
staging=$(mktemp -d)
packaging/prepare-source "$staging" "$(git rev-parse HEAD)"
cd "$staging"
makepkg --verifysource
makechrootpkg -c -r "$chroot"
for package in ./*.pkg.tar.*; do
  case "$package" in *.sig) continue ;; esac
  pacman -Qip "$package"
  pacman -Qlp "$package"
  bsdtar -xOf "$package" .BUILDINFO
  sha256sum "$package"
done
```

Resolve any failing command before continuing. The final commands inspect
package metadata, list installed paths and print the checksum to record
with the candidate. The version includes the commit count and full Git SHA.
Retain the staged recipe and source archive, the build
log, built archive, revision, repository or snapshot details, dependency
versions and checksums. The archive's `.BUILDINFO` records the clean chroot's
actual package inputs. `tools/record-baseline --build-dir build --output
test-results/baseline.json` records compiler and protocol details for the
build directory it is run against. Describe any missing build inputs.

Install the package in a clean or disposable Arch environment to verify
package ownership and runtime dependencies independently of the build.
Copy the archive into that environment and set `package` to its path. Compare
the first command’s digest with the published SHA-256 checksum before running
the installation commands.

```sh
sha256sum "$package"
sudo pacman -U "$package"
pacman -Qk seaside
pacman -Qoq /usr/bin/seaside /usr/bin/seasidectl
```

Use a test account on the guest's logged-in local console and follow the
[user guide](USER_GUIDE.md#start-and-exit). Verify first launch, `foot`,
return to the same terminal across a VT switch, and a clean exit. Use the
[sharing steps](USER_GUIDE.md#use-applications-and-sharing) to exercise the
portal. For package lifecycle tests, record the generated config's checksum
and compare it after reinstall and removal in the disposable environment.
Record the exact package revision, guest driver and renderer with the results.
The test plan covers further acceptance cases.
[Part 10 test plan](Parts/Part-10/PLAN.md#harness-and-arch-vm-acceptance) · [Native desktop checklist](Research/NATIVE_ARCH_CHECKLIST.md)

## Share changes and evidence

Keep each change focused and add an acceptance or regression check where
practical. Use a short Conventional Commit subject such as
`fix(capture): preserve the selected source on workspace change`.

Explain what the change fixes, how to reproduce or verify it, and what
remains open. Include the revision, package version when installed, exact
commands, relevant test case names and results, and the versions of the dependencies
or apps involved. For display issues also include the output mode, kernel,
renderer, backend, and GPU. Use a short local trace only for a relevant
problem. [Troubleshooting and trace export](USER_GUIDE.md#troubleshoot)

| Destination | What belongs there |
|---|---|
| Git | Source, tests and intentional fixtures, the package recipe, project assets, canonical configuration, guides, dated handoffs, and concise evidence with reproduction steps |
| Package handoff or release assets | The built archive, checksum, exact source revision, and build provenance needed to check or reproduce that candidate |
| Local or approved test storage | Build trees, generated archives, test reports and captures, working traces and logs, maintainer-specific instructions, machine inventories, user config, and state |

Use `git status --short` and `git check-ignore -v PATH` to check which paths
would be shared and which rule ignored an output. Store generated work under
an ignored build, `test-results/`, or `test-artifacts/` directory, or use a
temporary directory. Before sharing a log, trace or screenshot, inspect it
and keep the excerpt relevant to the issue. Remove tokens,
user or device identifiers, machine-specific paths, and unrelated desktop
content. Keep credentials out of documentation and shared test evidence.
Keep reusable anonymized inputs as intentional test fixtures.

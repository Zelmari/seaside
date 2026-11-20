# Seaside

An Arch Linux Wayland compositor using **C++23 and wlroots**, **GCC** as the
primary compiler and **Meson + Ninja** for builds. The current implementation is
the **parts 1–2 foundations**: a C++ lifecycle owner with RAII and typed errors,
a narrow C bridge for wlroots, typed TOML configuration and bindings, bounded
workers, recovery-file transactions and a logical window/workspace registry.
The real-client headless fixture, bounded Python test runner and DRM inspection
tool remain development infrastructure. Useful clients/probes remain C17.
The installable compositor session and window manager belong to later parts.

The product contract is in [the design](Specification/SEASIDE_SPEC.md). Build and
verification details are in [the part 1 handoff](Specification/PART1.md).
Configuration/state implementation and checks are recorded in
[the part 2 handoff](Specification/PART2.md).
Future latency investigations are tracked in
[the performance research notes](Specification/Research/PERFORMANCE.md).
Approved choices and documentation needs for users and contributors are also
recorded in [the public documentation record](Specification/PUBLIC_DOCUMENTATION.md).

## Build and test

Install the build dependencies in the Arch development environment:

```sh
sudo pacman -S --needed gcc pkgconf meson ninja python wlroots0.20 wayland wayland-protocols libdrm pixman gtest clang gdb tomlplusplus libxkbcommon
```

Then run:

```sh
meson setup build --buildtype=debug
meson compile -C build
meson test -C build
```

For a build directory created before the C++ migration, first run
`meson setup --reconfigure build -Dcpp_std=c++23 -Dwrap_mode=nodownload`.
Fresh builds use those defaults automatically; no dependency is downloaded.

The development runner also builds, independently discovers GoogleTest cases,
runs tests and collects bounded logs, individual case results with part and
requirement metadata, and a baseline manifest:

```sh
tools/run-tests
```

Meson also runs GoogleTest ownership/failure-path cases and the `code-quality`
suite. Run `tools/check-code --build-dir build` independently to check
clang-format on the migrated core/new unit fixtures and clang-tidy on every
project translation unit using the actual compilation database. Retained legacy
C integration clients/probes keep their existing formatting. GDB is available
for interactive debugging, for example `gdb --args build/seaside-server-tests`.
The tool versions tested with this foundation are recorded in the part 1 handoff.

Static analysis runs at most two Clang processes concurrently, with a per-file
deadline. The runner's default command deadline is 300 seconds; `--timeout`
can change it for a slower guest. Missing tests, inconsistent reports or missing
GoogleTest case results are failures rather than evidence of completion.

Reports go under the ignored `test-results/` directory. A skipped hardware check
is explicitly reported as skipped; it does not qualify a cursor plane or GPU.
The headless fixture uses an offscreen pixman renderer and never controls a
physical display. None of the part 1 executables is installed as `seaside` or
adds a way to bypass the production session/preflight requirements.

## Licence

Seaside is licensed under the [MIT License](LICENSE). Third-party dependencies
retain their own licences.

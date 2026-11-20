# Seaside

An Arch Linux Wayland compositor using **C++23 and wlroots**, **GCC** as the
primary compiler and **Meson + Ninja** for builds. The current implementation is
the **part 1 build and test foundation**: a C++ lifecycle owner with RAII and
typed errors, a narrow C bridge for wlroots headers, a real-client headless
fixture, a bounded Python test runner and a DRM inspection tool. Useful test
clients and probes remain C17; later feature libraries are selected in the design.
The installable compositor session and window manager belong to later parts.

The product contract is in [the design](Specification/SEASIDE_SPEC.md). Build and
verification details are in [the part 1 handoff](Specification/PART1.md).
Future latency investigations are tracked in
[the performance research notes](Specification/Research/PERFORMANCE.md).
Approved choices and documentation needs for users and contributors are also
recorded in [the public documentation record](Specification/PUBLIC_DOCUMENTATION.md).

## Build and test

Install the build dependencies in the Arch development environment:

```sh
sudo pacman -S --needed gcc pkgconf meson ninja python wlroots0.20 wayland wayland-protocols libdrm pixman gtest clang gdb
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

The development runner also builds, runs tests and collects bounded logs,
per-test results and a baseline manifest:

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

Reports go under the ignored `test-results/` directory. A skipped hardware check
is explicitly reported as skipped; it does not qualify a cursor plane or GPU.
The headless fixture uses an offscreen pixman renderer and never controls a
physical display. None of the part 1 executables is installed as `seaside` or
adds a way to bypass the production session/preflight requirements.

## Licence

Seaside is licensed under the [MIT License](LICENSE). Third-party dependencies
retain their own licences.

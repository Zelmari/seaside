# Seaside

An Arch Linux Wayland compositor with **C++23 and wlroots selected as the
foundation**, retaining **Meson + Ninja** for builds. Supporting libraries remain
under review. The current implementation is
the original **C/wlroots part 1 build and test foundation**: a private server
lifecycle library, a real-client headless fixture, a bounded test runner and a
DRM inspection tool.
The installable compositor session and window manager belong to later parts.

The product contract is in [the design](Specification/SEASIDE_SPEC.md). Build and
verification details are in [the part 1 handoff](Specification/PART1.md).
Future latency investigations are tracked in
[the performance research notes](Specification/Research/PERFORMANCE.md).

## Build and test

Install the build dependencies in the Arch development environment:

```sh
sudo pacman -S --needed gcc pkgconf meson ninja python wlroots0.20 wayland wayland-protocols libdrm pixman
```

Then run:

```sh
meson setup build --buildtype=debug
meson compile -C build
meson test -C build
```

The development runner also builds, runs tests and collects bounded logs,
per-test results and a baseline manifest:

```sh
tools/run-tests
```

Reports go under the ignored `test-results/` directory. A skipped hardware check
is explicitly reported as skipped; it does not qualify a cursor plane or GPU.
The headless fixture uses an offscreen pixman renderer and never controls a
physical display. None of the part 1 executables is installed as `seaside` or
adds a way to bypass the production session/preflight requirements.

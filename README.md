# Seaside

An Arch Linux Wayland compositor using **C++23 and wlroots**, **GCC** as the
primary compiler and **Meson + Ninja** for builds. The TTY session owns a live
Wayland display, enforces hardware cursors, and checks mandatory session
services before it starts configured commands. It manages XDG windows,
workspaces, focus, maximize, fullscreen, and pin. Dwindle is the default
tiling layout, and scrolling columns are selectable in the TOML config.
TOML edits apply live, and `seasidectl` exposes local status, actions, and events.

The product contract is in [the design](Specification/SEASIDE_SPEC.md). Find
development plans, handoffs, native testing, and performance notes in the
[documentation index](Specification/README.md).

## Build and test

Install the build and test dependencies in the Arch development environment:

```sh
sudo pacman -S --needed gcc pkgconf meson ninja python wlroots0.20 wayland wayland-protocols \
  wlr-protocols libdrm libinput pixman gtest clang gdb tomlplusplus nlohmann-json \
  libxkbcommon libxau libxcb systemd-libs \
  libpipewire pipewire dbus libpng libjpeg-turbo libwebp pango cairo ttf-dejavu
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
C integration clients/probes keep their existing formatting; the new Part 3
integration fixture is checked too. GDB is available
for interactive debugging, for example `gdb --args build/seaside-server-tests`.
The tool versions tested with this foundation are recorded in the
[Part 1 handoff](Specification/Parts/Part-01/HANDOFF.md).

Static analysis runs at most two Clang processes concurrently, with a per-file
deadline. The runner's default command deadline is 540 seconds; `--timeout`
can change it for a slower guest. Missing tests, inconsistent reports or missing
GoogleTest case results are failures rather than evidence of completion.

Reports go under the ignored `test-results/` directory. A skipped hardware check
is explicitly reported as skipped; it does not qualify a cursor plane or GPU.
The headless fixture uses an offscreen pixman renderer and never controls a
physical display. No foundation executable is installed as `seaside` or adds a
way to bypass the production session/preflight requirements. The Part 3 fixture
uses real XDG clients and inspects offscreen scene pixels; private D-Bus/PipeWire
tests do not change the normal user's service environment. Their results do not
establish a real TTY session, hardware cursors, portal compatibility, audio
playback or sharing.

## Licence

Seaside is licensed under the [MIT License](LICENSE). Third-party dependencies
retain their own licences.

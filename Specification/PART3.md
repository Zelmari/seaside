# Part 3: headless session groundwork

Status: headless milestone software-verified in Arch WSL2. The Arch VirtualBox
guest passed the original harness and the live console fixture with Pixman.
Automatic renderer selection failed on this guest. Part 3 and release
qualification remain pending. The keyboard and terminal trial passed both the
offscreen software checks and the VirtualBox run.

This implements the bounded [Part 3 plan](PART3_PLAN.md) against specification
§§2.1–2.2, 3–3.1, 12.2b and 14. The checks below cover the headless milestone
and the subsequent Arch VM checkpoints. The TTY entrypoint is a development
fixture.

## Implemented mechanisms

| Module | Responsibility and boundary |
|---|---|
| `server.hpp/.cpp` | Display-owned socket publication, explicit shutdown-signal ownership, live libwayland dispatch and clean termination; not a session-ready declaration |
| `scene.hpp/.cpp`, `scene_bridge.h/.c` | Minimal XDG configure/map/unmap/destroy ownership and wlroots scene/output frame integration; no window-manager, placement or seat policy |
| `session_bus.hpp/.cpp` | Direct asynchronous sd-bus connection/calls, changing FD interests, monotonic deadlines, bounded pending work/dispatch, cancellation and disconnect handling |
| `session_systemd.hpp/.cpp` | Read-only user-manager version and bus credentials check with one deadline across both calls |
| `session_pipewire.hpp/.cpp` | Official libpipewire core sync/done connection check integrated with the Wayland loop; no playback, capture, mixer or routing |
| `startup_gate.hpp/.cpp` | Mandatory systemd-user, PipeWire, portal-frontend and capture-backend result coordination |

The minimal scene leaves initial window sizing to the client and advertises no
window-management capabilities. Popup placement/grabs, seat/input, focus and
normal window policies remain later integration work. Output callers own mode,
renderer and hardware-cursor policy; the scene does not select or enable a
physical output. It remains reusable without introducing a software cursor path.

Shutdown signals must be installed before creating any worker/helper threads,
so every thread inherits their blocked mask. Workers are joined before signal
sources/masks are restored. A real worker in the fixture verifies this ordering.

The D-Bus adapter accepts one explicitly supplied local Unix bus address. It
starts authentication without synchronous reply waits and never changes session
activation variables or starts/stops user services. Defaults are 16 pending
calls and a 32-message/completion dispatch budget, with eventfd continuations
that yield to other ready Wayland sources. Cancellation and teardown suppress
undelivered callbacks; callbacks can reenter or destroy their owner.
If wake notification fails permanently, the existing timer drains failures in
bounded turns. If both notification mechanisms fail, accepted failures drain
after sd-bus unwinds, bounded by admission capacity; this fatal-only path may
notify synchronously and exceed the normal per-turn budget. Rejected new calls
receive no callback. Other Wayland sources and safe owner destruction remain
covered by fault tests.

The PipeWire adapter uses a native socket and PipeWire's normal remote
selection. Fixtures can supply a specific remote. Only a matching
`PW_ID_CORE` sync sequence establishes the roundtrip result. Resource
construction can load client configuration and modules. Protocol
communication uses nonblocking loop iterations. No private PipeWire
server is created by production code: private servers belong exclusively to
tests. Delivery is separate from PipeWire dispatch so callbacks can destroy
their probes safely.

The startup gate requires all four mandatory results. Its default overall
deadline is five seconds, with internal callers restricted to 1–60,000 ms.
Reports distinguish missing service, activation failure, wrong environment,
incompatibility, timeout, disconnect and internal failure. Absolute monotonic
deadline checks prevent late replies from defeating an overdue timer. Duplicate
reports cannot fill another requirement, and cancelled gates cannot become
ready. Completion is deferred; if allocating its notification fails, a typed
`system` error requires the caller to abort synchronously. The deadline callback
itself has an asynchronous failure-delivery fallback.

## What the tests establish

- The new real-client fixture uses explicit headless/Pixman resources and
  generated client bindings from the installed XDG-shell XML. It checks actual
  configure/ack/SHM-buffer/frame traffic and black/client pixels in composed
  output buffers across three rounds: remap, abrupt disconnect and shutdown
  while a mapped client is still connected. Pixel-readback tests deliberately
  force offscreen composition; they do not qualify direct scanout.
- Socket tests cover automatic/named publication, conflicts, ownership, invalid
  arguments, cleanup, live dispatch and restoration of owned signal-mask bits.
- D-Bus cases use an isolated real `dbus-daemon`, including remote errors,
  stalled authentication, overdue replies, cancellation, disconnect, reentry,
  callback destruction and event-loop fairness.
- User-manager cases exercise the version and credentials exchange with a
  controlled bus peer, including missing service, invalid replies, wrong UID,
  and cancellation.
- PipeWire cases use isolated real servers and controlled silent/disconnected
  peers, covering real roundtrips, deadlines, cancellation, failure delivery and
  lifetime/resource cleanup. A successful exchange does not establish working
  audio, microphone, portal permission flows or capture frames.
- Startup coordination tests explicitly simulate mandatory-service reports.
  They verify required results, failure categories and dispatch ordering; they
  cannot qualify the systemd user manager or portal backend.
- The harness discovers Part 3 cases separately and records its qualification
  as a partial headless milestone, even when every software check passes.
  Fixture directories live below runner-owned runtime/temp roots, allowing the
  supervisor to remove their parents after hard timeouts too.

## Dependency and tooling evidence

The local Arch WSL2 build uses libsystemd API version 262 (`systemd-libs`
262.2-1) and libpipewire 1.6.11 (`libpipewire`/`pipewire` 1:1.6.11-2).
PipeWire and SPA headers are third-party system includes; project code retains
strict C++23 and warnings-as-errors. Existing wlroots 0.20.2 and Wayland/protocol
dependencies remain the baseline. No dependency source is downloaded or embedded.

The baseline collector records the new libraries, headers, client modules,
runtime fixture binaries and standard D-Bus/PipeWire configuration artifacts.
This records observed inputs, not a complete reproducible Arch snapshot.
The package installation used standard Arch hooks (including the PipeWire
socket symlink/user-manager reload); tests do not manually activate or alter
the normal user's services. New integration sources participate in formatting
and actual-compilation-database analysis alongside the existing checks.

## Verification

| Check | Actual command and result |
|---|---|
| Full normal harness | `tools/run-tests --build-dir build`: 12 Meson entries passed, one hardware skip; all 135 GoogleTest cases passed |
| ASan/UBSan runtime harness | `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 tools/run-tests --build-dir build-asan --suite unit --suite integration --suite runner --suite hardware`: 11 entries passed, one hardware skip; all 135 GoogleTest cases passed, no sanitizer findings |
| Harness regressions | Both runs executed all 52 Python self-tests, including real fixture/process cleanup; all passed |
| Static analysis/formatting | Normal harness `code-quality` passed for the actual compilation database, including the new integration sources; sanitizer run selected runtime suites rather than repeating static analysis |
| Dependency/source evidence | Both baseline collections reported no errors, recorded 371 supporting API/library artifacts and four session-support artifacts; tested source hashes match the final implementation |
| Independent code review | Three implementation agents cross-reviewed lifetimes, deadlines, signals and failure paths; reproduced findings were fixed and regression-tested |

Reports are retained under ignored `test-results/`:
`20261120T165531Z-6eafadf0` (normal) and
`20261120T165628Z-84593b06` (ASan/UBSan runtime).
The sanitizer build was prepared with
`meson setup --reconfigure build-asan -Db_sanitize=address,undefined` and
`meson compile -C build-asan -j 2`.

The 47 new GoogleTest cases comprise five server-loop, ten startup-gate,
15 D-Bus and 17 PipeWire cases. The separate real-XDG fixture executes three
rounds. Earlier configuration/state/worker, headless-client and DRM-probe
contract regressions also pass. Software checks do not close any VM/hardware
gate, and this milestone does not add an installed compositor entrypoint.

Cross-review caught and independently reproduced overdue-success races, stranded
D-Bus callbacks after a broken wake FD and the need to install process shutdown
signals before workers. Failure tests now cover both notification mechanisms
failing with reentrant cancellation/destruction. Removing PipeWire's absolute
deadline checks in an ignored counterfactual build makes the overdue-reply
regression fail, demonstrating that it protects the intended ordering.

## VirtualBox console checkpoint

The first Arch guest harness run passed. The console reported `vmwgfx` on
`/dev/dri/card0` with a render node, and the owner counted eight cursor-plane
entries in `drm_info`. Those observations establish that the guest is a
candidate for testing real cursor presentation. The watchdog reported stalls
during that run, and the harness completed successfully.

The guest built revision `b2c172f` after Ninja regenerated the build files.
The first console attempt stopped at `stage=seat-drm-renderer` with automatic
renderer selection. The owner reran the same fixture with
`WLR_RENDERER=pixman build/seaside-tty-fixture`. It enabled `Virtual-1` at
1280 × 800 and 60 Hz, with DRM connector 44 and hardware cursor plane 40.
The report counted 5,565 presented frames and 5,564 presented position changes
from 5,575 accepted moves, then printed `PASS stop=observation-ended` and returned
to the same shell. The owner confirmed that the visible white cross followed
their pointer. The explicit renderer choice applies to this VM check. Automatic
renderer selection remains a separate qualification item.

`seaside-tty-fixture` is a development executable for one DRM output. It opens
the guest seat through wlroots, starts the DRM and libinput backends, uses the
existing scene to render black, and moves a hardware cursor from real pointer
events. It checks presentation and records the DRM connector and cursor plane.
It uses a fixture-specific Wayland socket and leaves user service activation
for the graphical session milestone. From the active guest console, use Ninja
to regenerate the existing Meson build files and build the updated project:

```sh
ninja -C build && build/seaside-tty-fixture
```

Move the pointer across the black screen for a few seconds. Wait for the
fixture to return to the same console, or press Escape after moving the
pointer. The display trial runs for 15 seconds after startup. The startup
observation has a five-second deadline for a presented cursor frame and
pointer discovery. A final `TTY fixture PASS` line requires at least one
presented pointer move. Record that line and whether the pointer moved visibly
and the original console returned. If setup fails, record the last diagnostic
and the `TTY fixture FAIL` or `BLOCKED` line. The dedicated fixture runs only
when invoked explicitly. The normal suite exercises console refusal and input
lifetimes independently.

Local verification for this checkpoint:

- `tools/run-tests --build-dir build` passed 14 Meson entries and all 136
  GoogleTest cases. The discovery-only hardware probe reported one skip.
  The report is `test-results/20261120T174302Z-ef6bd079/summary.txt`.
- All seven Part 3 entries passed under ASan/UBSan with leak detection using
  `meson test -C build-asan --suite part3 --print-errorlogs`.
- The signal regression verifies that shutdown during startup prevents later
  dispatch and that shutdown signals stay blocked while destroying the display.
  A separate synthetic input test exercises relative and absolute pointer
  delivery, the fixture exit gesture, and input/backend teardown order.

## VirtualBox keyboard and terminal checkpoint

The TTY fixture's `--terminal` argument adds a keyboard seat with the default
XKB keymap and repeat information. Super+Return starts one standalone `foot`
process. The shortcut is handled by the fixture and its Return press and
release stay out of the client event stream. A mapped XDG surface receives
keyboard focus and activation when its Wayland peer PID matches the owned
child. Focus clears on unmap and shutdown. `foot` requires the Wayland
data-device global to connect. Clipboard actions and drag-and-drop remain in
their assigned milestone. [Foot 1.28.0 startup checks](https://codeberg.org/dnkl/foot/src/tag/1.28.0/wayland.c#L1835-L1863)

The fixture uses an empty `foot` configuration for repeatable tests and passes
the private Wayland socket name to the child. It watches that child without
waiting for exec from the input callback and reaps it when the trial ends.
The original cursor-only invocation still uses Escape or a 15-second timeout.
The terminal trial forwards Escape normally and has a 60-second time limit.

Install `foot` and a monospace font in the VM. A minimal Arch install can use
`sudo pacman -S --needed foot ttf-dejavu`. After updating the checkout and
building with `ninja -C build`, run the trial from the active guest console:

```sh
WLR_RENDERER=pixman build/seaside-tty-fixture --terminal
```

Press Super+Return, then type `echo ok` and press Enter in `foot`. Verify that
the terminal displays `ok`. Type `exit` and press Enter to return to the same
console. Record that observation and the final `TTY terminal` result line. A
pass requires a hardware cursor presentation, a mapped terminal, at least one
forwarded key press and a clean exit from the owned child.

The VirtualBox run at `8c47639` reported `TTY terminal PASS`,
`stop=terminal-exited`, `mapped=yes`, `keys=56` and `cursor-plane=40` with
Pixman. The guest returned to the original shell, and the owner confirmed
typing in `foot` worked. The fixture sends keyboard events to the terminal
and draws the cursor through DRM. Client mouse buttons and window dragging
remain in the window and input milestone.

An offscreen run with Arch `foot` 1.28.0-2 and DejaVu Sans Mono used the new code
with a synthetic keyboard and a test shell. Super+Return opened the real
terminal, the watched surface mapped, and three key presses delivered `ab`
followed by Enter to that shell. It returned zero after checking the exact
received line. The VirtualBox run supplied the physical keyboard and terminal
observation separately.

`tools/run-tests --build-dir build` passed 15 Meson entries, all 139 GoogleTest
cases and the formatting and static analysis checks. The discovery-only DRM
probe reported one hardware skip. The report is
`test-results/20261120T180118Z-a82f50db/summary.txt`. All eight Part 3 entries
also passed with ASan/UBSan and leak detection using
`meson test -C build-asan --suite part3 --print-errorlogs`.

### Core session services

Run `ninja -C build && build/seaside-session-check` from the existing checkout
in the Arch guest, logged in as the user who will run Seaside. Use the
normal login environment. The uninstalled fixture connects to the same user's
login bus, verifies the responding systemd manager, and waits for a matching
core reply from the PipeWire remote. It can exercise ordinary PipeWire socket
activation. Record the final `session core` line. Both services must report
`PASS(0)` for the command to succeed.

Arch WSL2 returned `systemd=PASS(0)` using the login bus and `pipewire=PASS(0)`
using a private native PipeWire server. A missing socket, a silent server and
a daemon-mode override produced distinct failures in both the normal and
ASan/UBSan builds.

The Arch VirtualBox guest at `cc40d8b` first reported
`systemd=missing(-6) pipewire=PASS(0)`. The next command,
`systemctl --user show -p Version`, returned `262.2-1-arch`, and the immediate
retry returned `session core systemd=PASS(0) pipewire=PASS(0)`. This confirms
both services answered on retry. The first attempt did not establish why the
systemd name was missing.

The regression harness passed 15 entries and all 148 GoogleTest cases, with
the DRM discovery probe skipped in WSL. The report is
`test-results/20261120T181715Z-554be2dd/summary.txt`. All eight Part 3 entries
also passed with ASan/UBSan and leak detection.

The full startup trial needs a live Wayland socket, session-owned activation
environment, and the genuine capture globals expected by the selected portal
backend. Test portal routing and startup failure-to-TTY in the dedicated
graphical guest. Qualify VT switching and isolation with the session lifecycle.

The portal candidate (`xdg-desktop-portal-wlr` 0.8.4) requests embedded cursors
by default. The wlroots 0.20.2 output capture helper responds by switching the
output to software cursors. Qualifying this backend requires verifying cursor
composition in the captured frames while preserving the hardware cursor on
the display. The portal gate also needs a real capture source before it can
test backend startup. [Portal capture request](https://github.com/emersion/xdg-desktop-portal-wlr/blob/v0.8.4/src/screencast/ext_image_copy.c#L278-L284)
and [wlroots output capture behavior](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/0.20.2/types/ext_image_capture_source_v1/output.c#L40-L70).

Native hardware cursor, latency, VRR, tearing, colour and suspend/resume evidence
remain separate. Later window/workspace/layout/IPC/compatibility parts have not
been started by this milestone.

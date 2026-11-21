# Part 7 compatibility audio and sharing

Seaside reserves a local X11 display and starts `xwayland-satellite` on
its first X11 connection. Commands launched by Seaside inherit `DISPLAY`
and `XAUTHORITY`. If satellite exits, the next X11 connection can start
another child. The session owns the listening sockets and the private
Xauthority file. [Part 7 plan](PLAN.md) · [Seaside design](../../SEASIDE_SPEC.md)

The focused client can receive clipboard and primary selection data. A
Wayland client with a valid seat serial can publish a selection, including
a delayed selection from satellite after focus changes. Pointer drag-and-drop
uses the initiating grab and cancels on Escape or input-device loss.

The Wayland capture protocols expose each monitor and each mapped toplevel as
its own source. Portal clients choose the source through an external chooser.
A selected window keeps its identity when it moves or resizes. Active capture
supplies frame callbacks to hidden windows so they can continue drawing. The
stream ends when the target unmaps or is destroyed. Capture activity stops
when the last consumer releases the source.

Direct capture is available to local clients with access to the session's
Wayland socket. Portal applications use the external backend's chooser and
restricted PipeWire connection. Clipboard history and persistence belong to
external utilities.

## Try Part 7 in an Arch session

Install the standard satellite and audio packages and a compatible chooser.
The portal route in the Part 3 handoff remains required. [Portal setup](../Part-03/HANDOFF.md)

```sh
sudo pacman -S --needed xwayland-satellite xorg-xwayland wofi \
  pipewire pipewire-audio pipewire-pulse wireplumber grim wf-recorder
```

For a Wofi chooser in a normal Wayland window, set these values in
`~/.config/xdg-desktop-portal-wlr/Seaside` before launching the session.

```ini
[screencast]
chooser_type=dmenu
chooser_cmd=wofi -d -n --no-custom-entry
```

| Check | Action |
|---|---|
| First X11 connection | Open `foot` with `Super+Return`, then run `./build/seaside-part7-x11-client`. Its title is `Seaside Part 7 X11 capture`. Send `color c24122` to repaint and `quit` to close. |
| Screenshot and recording | Run `grim -c /tmp/seaside.png`. Start `wf-recorder --no-dmabuf -c libx264 -f /tmp/seaside.mkv` and stop it with Ctrl+C. Use `-g` to request an even-pixel region when encoding an odd-sized output. |
| Portal capture | Run `./build/seaside-portal-stream monitor frame` or `./build/seaside-portal-stream window frame` inside the session. Choose the requested source and check the emitted frame size and sampled RGB values. |
| Media keys | Use volume up, volume down, and mute. Check the default sink with `wpctl get-volume @DEFAULT_AUDIO_SINK@`. |

## Verification commands

- Run `tools/run-tests --suite part7` for the satellite, seat, and capture
  cases. Run the Part 3 suite for the original session and capture checks.
- From an idle TTY in the dedicated guest, run
  `python3 tests/session/part7_vm.py --console-ready` from the host checkout.
  The runner builds an exact bundle in an isolated guest checkout, uses its
  own config and state, and records its evidence under the Council VM state
  directory. Use `--pkg-config-path` for staged build dependencies and
  `--runtime-prefix` or `--recorder-prefix` for extracted guest runtime packages.
- Use `tools/run-tests --jobs 2` to cap build and test concurrency. The guest
  trial samples Seaside's resident memory and file descriptors during its real
  TTY checks. It retains intermediate results if a later step is interrupted.

## Arch VM verification from 2026-11-21

The full trial at `1134ae2c7a84` passed 38 Meson checks and 246 GoogleTest
cases. The standalone passive DRM probe was skipped. The real TTY used
hardware cursor plane 40 at 1280×815, kept the same terminal shell across
the VT round trip, returned to TTY on normal exit, and failed to TTY when
PipeWire was deliberately unavailable at launch.

- The installed portal frontend and backend selected sources through Wofi.
  The PipeWire receiver verified monitor pixels and native and X11 window
  pixels. The selected windows repainted on a hidden workspace without
  changing focus. Native overlap stayed out of the stream. Canceling the
  chooser and closing both captured targets produced the expected results.
- The external recorder produced 61 H.264 frames at 1280×814 in the
  requested even-pixel region. FFprobe decoded the recording. Seaside's RSS
  was 20,492 KiB and its descriptor count was 71 before and after recording.
- The first X11 client started satellite. A new client mapped after the
  owned child was terminated and a second child started. Two real GTK
  clients exchanged clipboard and primary selection in both directions.
- The existing media bindings changed the default sink by five percentage
  points, capped increases at 100%, and toggled mute. The playback monitor
  received 55,200 samples of the test signal. The session and IPC survived
  termination of the owned portal backend.

The guest used kernel `7.3.5-arch1-1`, wlroots `0.20.2`, Wayland protocols
`1.50`, Pixman, portal `1.22.1`, xdpw `0.8.4`, PipeWire `1.6.11`, and
WirePlumber `0.5.19`. The user-local signed packages supplied satellite
`0.8.3`, Xwayland `24.1.15`, Wofi `1.5.3`, `wf-recorder` `0.6.0`, and
FFmpeg `9.1`. Reports are in `test-results/vm-932e00cac307` in the usual
guest checkout and in Council VM evidence run `20261121T203557Z-932e00ca`.

The WSL2 ASan/UBSan run used `tools/run-tests --jobs 2 --suite part7 --suite
part3`. It passed 15 checks and 88 GoogleTest cases with leak detection
enabled. Both installed-portal checks were skipped on that host and passed
in the Arch VM. The host sanitizer report is `20261121T204126Z-4a99f18b`.

The normal WSL2 run used `tools/run-tests --jobs 2 --timeout 440` and passed
36 checks and 246 GoogleTest cases. The two portal checks and passive DRM
probe were skipped. The aggregate code-quality test reached its 240-second
limit after formatting and most static checks completed. Running clang-tidy
on its 10 remaining translation units with the same compilation database
passed. The normal report is `20261121T204203Z-dc54b3d5`.

## Memory observation

An earlier guest run stopped responding. Its recovered kernel journal contains
timer and watchdog stalls. The VirtualBox log records timer lag from before
that trial. There was no recorded OOM kill in the recovered journal.

The monitored rerun at `c744c6b42b32` held Seaside at 20,020 KiB RSS and 70
descriptors before and after 24 consecutive captures following warmup. It
sampled a 24,180 KiB RSS peak across the entire session, with at least
7,523,596 KiB of guest memory available. The sample included GTK clipboard
transfers, live X11-window streaming, and playback and media-key checks.

At `2160d7217794`, Seaside briefly reached 85,376 KiB RSS during a 119-frame
`wf-recorder` capture. It returned to 20,160 KiB at the next half-second
sample. The file-descriptor count returned from 89 to 71. The archived clip
decoded as H.264 at 1280×814, one row below the 1280×815 monitor. The final
runner requests that even-height region explicitly.

## Remaining qualification

The external Wofi chooser and scripted PipeWire receiver exercise real desktop
portal selection. OBS and Discord are absent from the test guest. Sharing in
those applications, desktop audio in their receiving clients, and physical
microphone and Fn-key behavior remain open. Native gaming and GPU checks
remain on the reference system. Isolated application audio is deferred beyond
v1. [Open Part 7 checks on native Arch](../../Research/NATIVE_ARCH_CHECKLIST.md#part-7-checks-still-open) · [Capture and audio acceptance](../../SEASIDE_SPEC.md#122-required-test-suite)

Earlier trials occasionally reached a linked PipeWire stream with no samples
at the receiver. A recovered xdpw trace showed that the first Wayland frame
had reached the backend. The final trial exercised changing content during
stream startup and passed all streams on their first attempt.

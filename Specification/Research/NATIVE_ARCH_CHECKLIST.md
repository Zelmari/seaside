# Native Arch desktop checklist

Use this checklist when Seaside becomes the main desktop on the native Arch
installation. Start with the first-login checks, then exercise the apps,
devices, and workflows used day to day. Run each item once its implementation
is available. Record results here as the native trial progresses.
[Seaside design](../SEASIDE_SPEC.md) · [Part 7 handoff](../Parts/Part-07/HANDOFF.md) · [Part 8 handoff](../Parts/Part-08/HANDOFF.md) · [Part 9 handoff](../Parts/Part-09/HANDOFF.md) · [Performance investigations](PERFORMANCE.md)

## First login

- [ ] Start Seaside from the logged-in TTY. Open `foot`, use the keyboard and
  pointer, and exit back to the same TTY. Check the hardware cursor on every
  enabled output and verify the expected resolution, refresh rate, and scale.
- [ ] Switch away from the active VT and back with applications open. Check
  input, focus, display layout, and cursor recovery. Repeat after suspend and
  resume once that path is ready.

## Part 7 checks still open

The Arch VM verified the underlying sharing and playback paths. The checks
below use the actual apps, receiving clients, and physical devices on native
Arch. [Part 7 VM evidence](../Parts/Part-07/HANDOFF.md#arch-vm-verification-from-2026-11-21)

### OBS and Discord sharing

Run the sharing checks for both apps and record the outcome separately for
each. Check OBS in the preview and a local recording. Check Discord from the
receiving end of a test call.

- [ ] Share a whole monitor. Verify the chosen output, cursor behavior, and
  live motion. Cancel the chooser, then start and stop sharing again.
- [ ] Share a native Wayland window and a satellite-backed X11 window. Move
  and resize each target, overlap it with another application, and switch to
  another workspace while the target keeps changing. Verify the receiver
  still sees the original window, overlapping content stays out of the
  stream, and capture leaves focus where the user put it.
- [ ] Close the shared window and confirm the stream ends. Start a new share
  and verify that the chooser can select a different source.
- [ ] Share a monitor with desktop sound enabled. Play two distinguishable
  sounds on the intended playback device and confirm both reach the receiver.
  Check audio/video sync, mute, and changing the playback device mid-share.
  Record which devices the app actually includes in the mix.

### Physical audio and keys

- [ ] Check normal playback through the speakers, headphones, and any other
  audio devices normally used. Switch the default output while audio plays.
- [ ] Test microphone input in OBS and a Discord call. Check the intended
  input, mute, and switching microphones where applicable. Verify desktop
  share audio and microphone audio can be controlled independently.
- [ ] Press the physical volume-up, volume-down, and mute keys. Try the
  keyboard's Fn and Fn Lock combinations where available. Verify five-point
  changes on the current default output, the 100% cap, and mute toggle.
  Check that ordinary F10–F12 reach a focused app when they emit function-key
  events, and try any media-key remapping used on the main desktop.

## Part 8 checks on the Framework Desktop

Start Seaside from a logged-in native Arch TTY with the actual AMD renderer.
Use the read-only collector inside the running session to record the display
and presentation state around each trial. Give each run a short label, such as
`fullscreen before sharing` or `after VT return`.

```sh
python3 tests/hardware/part8_display.py 'fullscreen before sharing' --seconds 5
```

- [ ] Check active hardware cursor planes and cursor alignment on each AMD
  output at 1.0, 1.25 and 1.5 scale and at the supported rotations. Repeat after
  a mode change, VT switch, suspend/resume, and reconnect. Move the pointer
  across monitor boundaries and check that it reaches the expected pixels.
- [ ] Use two physical monitors with different refresh rates and, where
  available, two similar displays with ambiguous EDIDs. Reconnect on another
  port, unplug the primary and secondary in turn, and reconnect after removing
  every output. Verify primary ranking and stability, workspace migration and
  restoration, preservation of manual workspace moves, and client survival.
- [ ] Use a native DMA-BUF client with `linux-drm-syncobj`. Observe acquire and
  release fences through composition, direct scanout and selected-window
  capture, including a hidden window. Exercise replacement or destruction of
  a buffer, client or output with a fence pending. Include removing the
  surface's sync object before committing a replacement buffer. Check
  implicit-sync clients on the same GPU and look for corruption or stuck
  buffers.
- [ ] Compare an eligible fullscreen or ordinary surface with composition for
  overlap, scaling, rotation, the config-error banner, and active capture.
  Verify actual AMD scanout and recovery after each obstacle is removed. Record
  presentation feedback, KMS state, fallback reasons, idle work, frame pacing
  and missed presentations with matched workloads. Use external equipment for
  physical input-to-photon timing if it is measured.
- [ ] Run fullscreen Steam/Proton and native clients on a VRR-capable monitor.
  Verify fixed refresh with VRR off, per-output fullscreen VRR, and `always`
  where supported. Use a client that sets an async hint to verify supported
  fullscreen-only tearing and synchronized fallback for a rejected flip.
  Check preserve/stretch, focus, pointer lock and shortcuts during game play.
- [ ] Enable night light at 4500 K, check the hardware LUT and visible warmth,
  adjust the temperature, and turn it off. Verify color restoration through
  a VT switch, modeset and resume, and on mixed-capability outputs. Observe
  direct scanout with and without the LUT. An unsupported monitor should stay
  available with its prior colors.

## Part 9 checks on the Framework Desktop

- [ ] With multiple physical monitors attached, let an unfocused monitor sleep
  while a visible fullscreen client keeps its own monitor awake. Repeat with
  an idle-inhibiting Wayland client. Move it between outputs and hide, unmap,
  or close it, then check that the affected idle timer resumes. Wake each
  monitor separately with its relevant keyboard or pointer input.
- [ ] On AMD, verify visible display sleep, input wake, the restored mode and
  hardware cursor, and continuity of focus and workspace assignment. Include
  different refresh rates and scales, an active share when sleep is requested,
  and live idle or display configuration edits while a monitor sleeps.
- [ ] Suspend and resume with several clients open. Repeat with one display
  asleep, across a VT switch, and after a connector or input device changes.
  Check display power, cursor readback, pointer and keyboard focus, workspaces,
  layer reservations and active capture behavior. Try a fresh share after
  resume when the session services return.
- [ ] Use the intended external panel or launcher on the actual multi-monitor
  layout. Check work-area changes, keyboard and pointer focus, and panel
  visibility on each monitor when a game enters and exits fullscreen.

## Main desktop use

- [ ] Use the usual browser, chat, terminal, and X11 apps across workspaces.
  Try everyday clipboard and primary selection transfers and drag-and-drop.
- [ ] Plug and unplug the monitors and input devices normally used. Check
  workspace placement, focus, hardware cursor, and recovery when a display
  returns. Try the usual mixed refresh rates, scaling, and rotations.
- [ ] Run representative fullscreen games, including the usual Steam/Proton
  titles. Check first launch, relative pointer and pointer lock, focus on
  return, and fullscreen presentation with the normal monitor settings.
- [ ] Verify native direct scanout and composition transitions, VRR and tearing
  where supported, fullscreen aspect behavior, and hardware night light.
  Include capture starting and stopping during fullscreen use.
- [ ] During longer sessions, note any input pauses, freezes, capture stalls,
  or memory growth that persists after closing a share or recording. Record
  what was running and what operation preceded the issue. Use the performance
  notes for repeatable timing measurements. [Performance investigations](PERFORMANCE.md)

Record the date, Seaside revision, kernel, Mesa and renderer, monitor modes,
keyboard and audio devices, and app, portal, PipeWire, and WirePlumber
versions. For each failure, include the steps, expected and observed result,
and any relevant log or test recording. Mark checks as passed, failed, or
pending so the checklist reflects what actually ran.

Isolated application audio for a selected-window share is deferred beyond v1.
[Audio scope](../SEASIDE_SPEC.md#75-audio-boundary-desktop-sharing-and-deferred-application-isolation)

# Native Arch desktop checklist

Use this checklist when Seaside becomes the main desktop on the native Arch
installation. Start with the first-login checks, then exercise the apps,
devices, and workflows used day to day. Run each item once its implementation
is available. Record results here as the native trial progresses.
[Seaside design](../SEASIDE_SPEC.md) · [Part 7 handoff](../PART7.md) · [Performance investigations](PERFORMANCE.md)

## First login

- [ ] Start Seaside from the logged-in TTY. Open `foot`, use the keyboard and
  pointer, and exit back to the same TTY. Check the hardware cursor on every
  enabled output and verify the expected resolution, refresh rate, and scale.
- [ ] Switch away from the active VT and back with applications open. Check
  input, focus, display layout, and cursor recovery. Repeat after suspend and
  resume once that path is ready.

## OBS and Discord

Run the applicable checks in each installed app and record its version. Check
OBS in the preview and a local recording. Check Discord from the receiving end
of a test call.

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

## Physical audio and keys

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

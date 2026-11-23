# Part 11 plan for desktop behavior and session preferences

Start with desktop geometry and input, then add the session preferences and
package integration. Develop backdrop blur separately so its rendering and
capture checks can run against the simpler changes.

## Desktop geometry and input

- **Config error banner.** Use the banner's rendered logical height as a
  top reservation on each output. Feed that reservation through the existing
  layer-shell work-area calculation so panels and ordinary windows start
  below the banner. Measure the configured outer gap from the new work-area
  edge. Publish the banner and reservation together, recompute when the
  diagnostic or output geometry changes, and release the reservation when
  config recovery clears the banner. Test an invalid save and recovery with
  tiled, floating and maximized windows, an exclusive top panel, and
  differently scaled monitors.
- **Maximize and fullscreen.** Have Super+F use the output work area inset by
  the configured outer gaps. Have Super+Shift+F use the full output bounds
  with no outer gap, subject to the existing fullscreen aspect policy.
  The proposed banner policy is to hide it on a fullscreen output and show
  it again on exit if the error is still active. Test both shortcuts from
  tiled and floating states and check that leaving either mode restores the
  prior placement.
- **Drag resize.** Give Super+right-drag a fixed edge or corner for the
  gesture. Use the pointer's starting position to choose a nearby side or
  corner and use the first intentional motion to resolve a center start.
  Reuse the anchored floating resize path so the opposite sides stay fixed.
  For tiles, move only the corresponding available shared split handles.
  Test all sides and corners, pointer reversal, minimum sizes, and both
  Dwindle and Scrolling layouts.
- **Scroll direction.** Add a live `scroll_direction = "normal"` setting for
  pointer scrolling, with `"inverse"` as the alternate value. Apply it
  to mouse wheels and touchpad scrolling. Use libinput's natural-scroll
  support where available and account for both continuous and discrete
  events when applying direction at the compositor boundary. Verify vertical
  and horizontal motion, high-resolution wheels, reload, and device hotplug.
  [libinput device configuration](https://wayland.freedesktop.org/libinput/doc/latest/api/group__config.html)

## Session preferences and package integration

- **Default audio devices.** Add independent `[audio]` keys for playback
  and microphone selection. Accept the stable PipeWire node name or
  `"auto"`. Put `wpctl status --name` in the config comment and specify
  which names to copy from Sinks and Sources. Apply selections through
  WirePlumber when PipeWire is ready, and retry a configured device when
  it reconnects. Delegate unavailable devices to normal WirePlumber
  fallback and report the effective routing. Verify with multiple devices
  and new playback and recording streams.
  [WirePlumber wpctl reference](https://pipewire.pages.freedesktop.org/wireplumber/man/wpctl.html)
- **Light and dark appearance.** Add a global preference with dark as the
  default and light as the alternate value. Apply it to Seaside's own
  palette and publish the standard color-scheme preference through
  the existing desktop portal and toolkit integration. Verify the Settings
  portal value and change notification, a GTK app, a portal-aware app,
  and the default launcher. Apps with their own theme settings can honor
  or override the session preference.
  [XDG Settings portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.impl.portal.Settings.html)
- **Window border colors.** Add separate active and inactive color keys in
  `[window]`. Document the quoted hex format, such as `"#BFBFBF"` for
  active and `"#595959"` for inactive, which approximate the current
  default colors. Validate the format on reload and feed the colors into
  the existing focus-dependent border drawing path. Test focus changes,
  invalid hex recovery, and zero-width borders.
- **Power profile.** Add the Arch `power-profiles-daemon` dependency and a
  `[power]` profile setting accepting `"saver"`, `"balanced"`, and
  `"performance"`. Map saver to the daemon's `power-saver` profile.
  Let an omitted preference follow the system policy. Query supported
  profiles over D-Bus, apply an explicit selection asynchronously, and
  report denied or unsupported requests alongside the effective profile.
  Test fresh package installation, live changes, a VM without performance
  support, and performance where native hardware offers it.
  [Arch package](https://archlinux.org/packages/extra/x86_64/power-profiles-daemon/) ·
  [Power Profiles daemon API](https://upower.pages.freedesktop.org/power-profiles-daemon/gdbus-org.freedesktop.UPower.PowerProfiles.html)

Update the canonical example and generated template together. Test valid and
invalid edits, old configs with omitted settings, and last-good recovery.

## Refresh rate investigation

The affected HDMI output is running 1920×1080 at 180.003 Hz. Its EDID gives
the underlying timing as 180.002599 Hz. Older Seaside code required an exact
millihertz match for explicit refresh rates, so 180 Hz could fail while
`auto` selected the same nominal mode. Commit `077c6fb` added the
half-hertz tolerance and is included in the installed host build.

The current parser accepts unquoted `180`, `180.0`, and `180.003`. A
fixture using the observed timing selects 180.003 Hz for both `auto` and
explicit 180 Hz. Quoted numeric values are rejected by the parser. The
original failing edit is unavailable, so the historical cause and the
current matcher are verified independently of that edit.
[Nominal refresh fix](https://github.com/Zelmari/seaside/commit/077c6fbde9e4f08cba0f511f4955ef1d3a7ecf94)

For native qualification, record the effective resolution and output
override and test `refresh_rate = 180` in a controlled session. If it
fails, capture the exact diagnostic and separate parser rejection from
per-output mode selection, DRM testing, and commit/rollback. Test that an
explicitly unavailable mode keeps the last usable display state. Use the
VM to exercise mode selection and rollback with its advertised timings.

## Backdrop blur

Implement blur in the compositor behind translucent window regions. Add
a live `[render.blur]` setting with `enabled = false` and normalized
`strength` from 0 to 1, with 0 producing no blur. Consider exposing a
bounded pass count after the first rendering and frame-timing checks.

Reuse the scene stacking order, sample only the backdrop beneath the
window, and clip the result to its translucent coverage. Account for blur
radius in damage tracking and force composition where the effect contributes
to an output. Verify that opaque windows render correctly and that individual
window capture exposes only that window's content. Test deterministic
transparent-over-pattern samples, overlapping windows, resize and movement,
full-output capture, and frame timing on native hardware. Use the VM's
Pixman renderer to exercise any supported software path and to verify
desktop behavior when that renderer cannot provide the effect.

## Verification

Run the focused config, layout, Wayland input, render, and packaging fixtures
as each group lands. Install the resulting Arch package in the dedicated VM
and test from its real TTY with VirtualBox input and framebuffer captures,
leaving the host workspace and focus alone. Record the installed revision
and results. Check physical 180 Hz mode application, headset and microphone
routing, touchpad direction, power-profile support, and blur frame timing in
a controlled native session.

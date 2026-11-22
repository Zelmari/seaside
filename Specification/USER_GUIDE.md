# Use Seaside

Start with the [project README](../README.md) for a short first-session guide
and the [verification and support status](PUBLIC_DOCUMENTATION.md) for the
currently tested environments.

## Install on Arch

Use an up-to-date x86-64 Arch Linux system with a normal local login managed
by systemd and a graphics driver that exposes a working hardware cursor plane
for your output. The package uses Arch's PipeWire audio stack, desktop portal
services, and `foot`. The package manager resolves the declared runtime
dependencies.

### Install a supplied package

If you received an Arch package archive and its published SHA-256 checksum,
compare the checksum first. In the commands below, replace the sample filename
with the exact archive you received. `pacman -U` installs or upgrades Seaside
from that file.

```sh
sha256sum ./seaside-example-x86_64.pkg.tar.zst
sudo pacman -U ./seaside-example-x86_64.pkg.tar.zst
```

Run `pacman -Q seaside` for the installed version and `pacman -Ql seaside`
for the installed files. To uninstall Seaside, run `sudo pacman -R seaside`.
Your config and state are in your home directory, outside the package's files.
[pacman manual](https://man.archlinux.org/man/pacman.8.en)

### Build and install from source

Install Arch's package toolchain, then run the remaining commands in the Git
checkout as your normal user. `pacman -Syu` updates the system before installing
build tools. Use a full Git checkout so the preparation script can count its
commits for the candidate version. The script stages the selected commit and its
checksummed source archive in a temporary directory. Commit any intended source
changes first so the archive uses the requested revision. `makepkg --syncdeps`
installs declared dependencies as needed and builds the package.

```sh
sudo pacman -Syu --needed base-devel git
staging=$(mktemp -d)
packaging/prepare-source "$staging" "$(git rev-parse HEAD)"
cd "$staging"
makepkg --syncdeps
sudo pacman -U "$(makepkg --packagelist)"
```

Run these commands in order and resolve any failing command before continuing.
`makepkg --packagelist` prints the archive path for the configured package
format. Keep the staging directory for as long as you need the archive or its
build inputs. See the [contributor guide](CONTRIBUTING.md#build-in-a-clean-arch-chroot)
for the isolated build used to qualify candidates.

## Start and exit

Log in on the active Linux virtual console (TTY) and run `seaside` in the
foreground as your normal user. A TTY is the Linux login console reached
through your machine's virtual-terminal keys, typically **Ctrl+Alt+F2** or
another function key. The command uses the logged-in user's runtime directory
and seat. SSH, an existing desktop terminal emulator, and a background job
cannot provide the required foreground console.

After required session services pass startup checks, expect a black desktop
with no windows. A panel is optional. Press **Super+Enter** to open `foot`.
In that terminal, run `seasidectl status` to confirm the session is ready or
`seasidectl exit` to return to the same logged-in TTY. Close or save your work
before ending the desktop session. [Startup, recovery, and shutdown design](SEASIDE_SPEC.md#3-startup-session-recovery-and-shutdown)

## Configure Seaside

The first launch creates `~/.config/seaside/config.toml`. If you use an
absolute `$XDG_CONFIG_HOME`, the path is `$XDG_CONFIG_HOME/seaside/config.toml`.
The canonical example explains the settings and their defaults. The package
also installs it at `/usr/share/doc/seaside/config.example.toml`. Change one
existing value, such as `gaps_inner` in `[window]`, and save. A valid live
change applies to the running session. Most settings take effect this way.
`workspaces.mode` and the autostart command list take effect on the next start.
[Canonical TOML](SEASIDE_CONFIG_EXAMPLE.toml) · [Settings and validation](SEASIDE_SPEC.md#82-full-settings-registry-and-validation)

If an edit is invalid, Seaside keeps the active settings and displays an error
banner. `seasidectl status` reports the config path and error. Correct the
file and save again. A subsequent launch with invalid TOML uses the last
accepted configuration, or built-in defaults when there is no accepted file.
The last-good copy is in `$XDG_STATE_HOME/seaside/last-good-config.toml`,
with `~/.local/state` as the fallback state directory.
[Config loading and recovery](SEASIDE_SPEC.md#81-loading-validation-reload-and-recovery)

### Default shortcuts

| Shortcut | Action |
|---|---|
| Super+Enter | Open `foot` |
| Super+D | Toggle tiling for the focused window |
| Super+arrow keys | Focus a window in that direction |
| Alt+Tab | Cycle through the current workspace's recent focus history |
| Super+left mouse drag | Move a window |
| Super+right mouse drag | Resize a window |
| Media volume down/up and mute | Adjust the default playback device in five-point steps, cap at 100%, or toggle mute |

**Super** is usually the Windows or Meta key. Use `Return` for Enter in a
binding, as shown in the example. The `[keybinds]` table is the
complete active list when present. Removing a line disables that shortcut,
an empty table disables every shortcut, and removing the table restores the
built-in defaults. The example includes optional chords for workspace,
fullscreen and exit actions. Use `seasidectl exit` for the first session's
clean exit. [Key names, actions, and examples](SEASIDE_SPEC.md#83-binding-grammar-and-action-catalog-engineering-choice)

### Monitors and layouts

Use `seasidectl outputs` to find the identity or connector selector reported
for each display. Use that value in a `[[monitors.output]]` override to set
mode, scale, rotation or position for one monitor. The example shows how to
write an override; the actual advertised modes depend on the output. Dwindle
splits space around a window as new windows open. Scrolling places windows in
horizontal columns. Set `layout = "scrolling"` in `[window]` to switch the
layout. [Monitor settings](SEASIDE_CONFIG_EXAMPLE.toml) · [Window layouts and focus](SEASIDE_SPEC.md#4-windows-layouts-focus-and-movement)

## Control the session

Run `seasidectl` in `foot` or another terminal launched inside the running
session. That process inherits the session's socket information. The client
prints a JSON response. An error response includes a code and message.

| Command | What it does |
|---|---|
| `seasidectl status` | Check readiness, active and requested config, and the last error |
| `seasidectl outputs` | List displays and selectors, modes, and output capabilities |
| `seasidectl workspaces` / `seasidectl windows` | Inspect workspace and window IDs and state |
| `seasidectl focus` | Inspect the focused output, workspace, and window |
| `seasidectl workspace 2` | Show workspace 2 |
| `seasidectl window to-workspace 2` | Move the focused window to workspace 2 and follow it |
| `seasidectl window toggle-fullscreen` | Toggle fullscreen on the focused window |
| `seasidectl night-light status` | Show the requested night light and the display's applied state |
| `seasidectl debug render` | Show rendering, presentation, and trace status |
| `seasidectl events` | Print live events until interrupted with Ctrl+C |
| `seasidectl exit` | End the session and return to the console |
| `seasidectl --help` | List the available CLI forms |

Clients that use the local IPC socket directly can read the [JSONL protocol and
error formats](SEASIDE_SPEC.md#10-ipc-cli-command-and-event-protocol--v1).

## Use applications and sharing

- **Wayland and X11 apps.** Run the app in a Seaside terminal or launch it from
  your own panel. The first X11 client starts `xwayland-satellite` on demand.
  If that service exits, a subsequent X11 connection can start it again.
- **Audio.** The media keys adjust the current default playback device through
  WirePlumber. Run `wpctl status` to list devices and
  `wpctl get-volume @DEFAULT_AUDIO_SINK@` to inspect the current volume and
  mute state. Physical Fn mappings depend on which events the keyboard emits.
- **Screen sharing.** Use the application's desktop-portal sharing option.
  Choose a monitor or a window in the external picker and then confirm in
  your app if prompted. The chosen window stays selected while it moves or its
  workspace is hidden. Closing the source ends its stream. The package's
  default chooser is Wofi, configured in `/etc/xdg/xdg-desktop-portal-wlr/Seaside`.
  The portal uses your per-user chooser config if you supply one. Isolated
  audio from one selected application is deferred. Receiving apps handle
  desktop audio and microphone selection.
- **Screenshots and panels.** `grim` is an optional screenshot utility. Once
  installed, run `grim -c "$HOME/seaside.png"` in the session to capture the
  desktop with the cursor. External panels that use Wayland layer-shell can
  run alongside Seaside. Ordinary layers hide while an app is fullscreen on
  that display and return afterward.

[Verified app and hardware coverage](PUBLIC_DOCUMENTATION.md#verification-and-support) · [Capture and audio design](SEASIDE_SPEC.md#7-shell-interoperability-capture-clipboard-and-idle-power)

## Troubleshoot

| Symptom | What to check |
|---|---|
| `seaside FAIL: start on the active Linux console as a foreground job` | Log in on the currently visible virtual console and run `seaside` there as your normal user. `tty` should print a numbered `/dev/ttyN`. |
| The login has no standard Arch user runtime directory | Check that the normal systemd-managed login set `XDG_RUNTIME_DIR` to `/run/user/` followed by your user ID. Sign into a fresh local TTY if the login environment is stale. |
| `seaside FAIL service=...` | Note the named service and reason. Use `systemctl --user status pipewire.socket wireplumber.service` and `journalctl --user -b` to inspect the relevant login services. If a portal name already belongs to another graphical session, exit that session normally or use another account. Seaside keeps the existing owner. |
| The console reports a seat, DRM, or hardware-cursor error | Check the `seaside FAIL` line and earlier graphics messages. Seaside requires placement on a hardware cursor plane. On a running output, inspect `seasidectl outputs` and `seasidectl debug render`. |
| The screen is black after launch | Press **Super+Enter**. A working terminal on a black background is the default empty desktop. If the screen remains dark, press a key or move the pointer to wake it and check the console log on exit. |
| A `seasidectl` command cannot connect | Run it as your normal user from the Seaside terminal. It needs the running session's `SEASIDE_SOCKET` or `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR`. |
| A config error banner persists | Use the path and error from `seasidectl status`. Fix the reported file and save again. |
| Sharing has no picker or stream | Check the application's portal sharing mode and the installed portal backend and Wofi. Review the session log and `journalctl --user -b` for the failing component. If you use a local portal override, compare it with the package's installed routing and chooser defaults. |

For a reproducible startup log, run this on the active TTY as an ordinary
foreground command. The log captures the same output shown on the console.

```sh
seaside 2>&1 | tee "$HOME/seaside-session.log"
```

Inside a running session, collect detailed frame observations only if needed.
Start tracing, reproduce the issue, then stop and export. The export reply
reports the JSON Lines file path under the Seaside state directory. A trace may
already be stopped if it reached its limit.

```sh
seasidectl trace start
# Reproduce the rendering issue.
seasidectl trace stop
seasidectl trace export
```

Report what you expected, the exact steps, the package version or Git revision,
and the relevant part of the log or trace. Include output mode, renderer and
driver information for display problems. [How to share useful evidence](CONTRIBUTING.md#share-changes-and-evidence)

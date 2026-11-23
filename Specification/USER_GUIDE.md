# Use Seaside

Start with the [project README](../README.md) for a short first-session guide
and the [verification and support status](PUBLIC_DOCUMENTATION.md) for the
currently tested environments.

## Install on Arch

Use an up-to-date x86-64 Arch Linux system with a normal local login managed
by systemd and a graphics driver that exposes a working hardware cursor plane
for your output. The package uses Arch's PipeWire audio stack, desktop portal
services, and `foot`. The package manager resolves the runtime dependencies,
including polkit for logind VT switching and RealtimeKit for PipeWire's audio
and capture scheduling. Set up any Wi-Fi connection in Arch before installing
Seaside. The package leaves networking to the operating system.

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
git clone https://github.com/Zelmari/seaside.git
cd seaside
staging=$(mktemp -d)
packaging/prepare-source "$staging" "$(git rev-parse HEAD)"
cd "$staging"
makepkg --syncdeps --install
```

Run these commands in order and resolve any failing command before continuing.
`makepkg --packagelist` prints the archive paths, including the separate
debug package when enabled by makepkg. Keep the staging directory for as long as you need the archive or its
build inputs. See the [contributor guide](CONTRIBUTING.md#build-in-a-clean-arch-chroot)
for the isolated build used to qualify candidates.

## Start and exit

Log in on the active Linux virtual console (TTY) and run `seaside` in the
foreground as your normal user. A TTY is the Linux login console reached
through your machine's virtual-terminal keys, typically **Ctrl+Alt+F2** or
another function key. The command uses the logged-in user's runtime directory
and seat. SSH, an existing desktop terminal emulator, and a background job
cannot provide the required foreground console.

In a VirtualBox guest with 3D acceleration disabled, run
`WLR_RENDERER=pixman seaside` from the TTY. Pixman renders the desktop on the
CPU. The virtual display still needs a usable DRM cursor plane.

At startup, expect a black desktop with no windows. A panel is optional. Press **Super+Enter** to open `foot`.
In that terminal, run `seasidectl status` to confirm the session is ready or
`seasidectl exit` to return to the same logged-in TTY. Close or save your work
before ending the desktop session. Seaside checks PipeWire, the user service
manager and portals in the background. The `integrations` field in
`seasidectl status` shows which are checking, available or unavailable.
You can use windows and workspaces while Seaside retries an unavailable service.
Configured autostart commands run once when Seaside has claimed its portal
names, when the initial activation fails, or after ten seconds. This gives
applications that request a portal immediately the session’s display and
portal owner. You can launch programs manually as soon as the desktop is ready.
A portal waiting for another service reports `dependency-unavailable` and names
that service.

Seaside publishes `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE`
and, when X11 is available, `DISPLAY` and `XAUTHORITY` to the systemd user
manager. Arch's dbus-broker launches D-Bus services through this manager, so
applications activated that way receive the session display too. The old values
are restored after exit or a compositor crash for variables whose values still
match Seaside’s when cleanup reads the manager. Removal checks the current
value atomically. Restoring a previously set value uses a separate manager
call, so a concurrent write between the check and that call can be overwritten.
Services activated by classic dbus-daemon’s own launcher require separate
configuration.

The three private portal units stop when their owning compositor exits.
A portal already running under another desktop or left over from an older
Seaside release keeps its name. Seaside keeps running and retries after the
owner releases it.

## Configure Seaside

The first launch creates `~/.config/seaside/config.toml`. If you use an
absolute `$XDG_CONFIG_HOME`, the path is `$XDG_CONFIG_HOME/seaside/config.toml`.
The canonical example explains the settings and their defaults. The package
also installs it at `/usr/share/doc/seaside/config.example.toml`. Change one
existing value, such as `gaps_inner` in `[window]`, and save. A valid live
change applies to the running session. Most settings take effect this way.
`workspaces.mode` and the autostart command list take effect on the next start.
[Canonical TOML](SEASIDE_CONFIG_EXAMPLE.toml)

If an edit is invalid, Seaside keeps the active settings and displays an error
banner. `seasidectl status` reports the config path and error. Correct the
file and save again. A subsequent launch with invalid TOML uses the last
accepted configuration, or built-in defaults when there is no accepted file.
The last-good copy is in `$XDG_STATE_HOME/seaside/last-good-config.toml`,
with `~/.local/state` as the fallback state directory.

Seaside follows chains of symlinks such as a dotfile profile selected by an
intermediate link. On local filesystems that allow Linux read leases on your
config file, it also recognizes pathname truncation and hard-link publication,
and can recover when an inotify queue overflow drops a close notification.

When the filesystem denies leases, for example on a system-owned dotfile,
use a single writer and close it or rename a completed file or profile link
into place. Seaside can confirm those events once it watches the corresponding
inode or directory. If a save finishes before a replacement directory is
watched, or a newly created symlink points to an edited file, save again
after the new path is in place. At startup, a readable managed config works
without a last-good copy. If its contents differ from an existing last-good
copy and the file cannot be leased, Seaside keeps the last-good settings
until it observes a completed save.

### Default shortcuts

| Shortcut | Action |
|---|---|
| Super+Enter | Open `foot` |
| Super+Q | Close the focused window |
| Super+D | Toggle tiling for the focused window |
| Super+F | Toggle maximization for the focused window |
| Super+Shift+F | Toggle fullscreen for the focused window |
| Super+1 through Super+9, Super+0 | Switch to workspaces 1 through 9, then 10 |
| Super+arrow keys | Focus a window in that direction |
| Alt+Tab | Cycle through the current workspace's recent focus history |
| Super+left mouse drag | Move a window |
| Super+right mouse drag | Resize a window |
| Media volume down/up and mute | Adjust the default playback device in five-point steps, cap at 100%, or toggle mute |

**Super** is usually the Windows or Meta key. Use `Return` for Enter in a
binding, as shown in the example. The `[keybinds]` table is the
complete active list when present. Removing a line disables that shortcut,
an empty table disables every shortcut, and removing the table restores the
built-in defaults. Add your own app shortcuts with `exec` in the existing
`[keybinds]` table. The generated config includes optional examples you can
uncomment and save.

```toml
"Super+B" = "exec firefox"
```

Run `seasidectl bindings` to see the shortcuts the running session is using.
[Binding examples and action names](SEASIDE_CONFIG_EXAMPLE.toml)

The default pointer is a black arrow with a white outline. Apps can request
their own pointer shapes. Ordinary tiled and floating windows use
`window.border_width`. A brighter border marks the focused window. Focus
navigation brings a selected tile in front of a maximized window that covers
it and preserves that window's maximized state. To move or resize a floating
window, drag its title bar or resize handles where the app provides them.
During a resize, Seaside can briefly scale the app's last drawn frame to keep
the window aligned with the pointer while the app redraws.

Set `titlebars = false` in the existing `[window]` table to hide app-drawn
title bars in Foot and other Wayland apps that support decoration negotiation.
The setting updates open windows when you save the file. Set it to `true`
to restore app-drawn title bars. Seaside's focus border is controlled by
`border_width`. Apps that draw a header as part of their content can keep it.

### Monitors and layouts

Use `seasidectl outputs` to find the identity or connector selector reported
for each display. Use that value in a `[[monitors.output]]` override to set
mode, scale, rotation or position for one monitor. The example shows how to
write an override. Seaside selects the closest advertised refresh at the
requested resolution within 0.5 Hz. For example, 60 Hz can select 59.951 Hz.
The `power_state` field shows whether the monitor is
awake, asleep, or disabled. A sleeping monitor stays logically enabled so
its workspace remains assigned. Keyboard or pointer activity wakes the seat's
sleeping displays. A fullscreen app or a visible idle inhibitor keeps its own
display awake while the seat is idle. Dwindle
splits space around a window as new windows open. Scrolling places windows in
horizontal columns. Set `layout = "scrolling"` in `[window]` to switch the
layout. [Monitor settings](SEASIDE_CONFIG_EXAMPLE.toml)

## Control the session

Run `seasidectl` in `foot` or another terminal launched inside the running
session. That process inherits the session's socket information. The client
prints a JSON response. An error response includes a code and message.

| Command | What it does |
|---|---|
| `seasidectl status` | Check readiness, active layout and tiling, pending restart settings, and the last error |
| `seasidectl bindings` | List active shortcuts and actions from the current configuration |
| `seasidectl outputs` | List displays, selectors, modes, power state, and capabilities |
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

Both `seaside --version` and `seasidectl --version` report the build version
and source revision. These commands also work outside a Seaside session.

The protocol implementation lists the methods and parameters accepted on the
local IPC socket. [JSONL protocol](../src/ipc_protocol.cpp)

## Use applications and sharing

- **Wayland and X11 apps.** Run the app in a Seaside terminal or launch it from
  your own panel. The first X11 client starts Seaside’s packaged copy of
  `xwayland-satellite` on demand.
  If that service exits, a subsequent X11 connection can start it again.
- **Clipboard.** Local Wayland clients running as your user can read and replace
  clipboard and primary selections in the background. Use `wl-copy` and
  `wl-paste` for command-line transfers. For history, run an external manager
  such as `wl-paste --watch cliphist store`. A background X11 app can also
  exchange either selection with a Wayland app while your current window keeps
  keyboard focus. For example, text copied with `xclip -selection clipboard`
  is available to `wl-paste`. Seaside’s bridge includes the upstream
  data-control change for this purpose.
  [Upstream implementation](https://github.com/Supreeeme/xwayland-satellite/pull/431)
- **Audio.** The media keys adjust the current default playback device through
  WirePlumber. Run `wpctl status` to list devices and
  `wpctl get-volume @DEFAULT_AUDIO_SINK@` to inspect the current volume and
  mute state. Physical Fn mappings depend on which events the keyboard emits.
- **Screen sharing.** Use the application's desktop-portal sharing option.
  Choose a monitor or a window in the external picker and then confirm in
  your app if prompted. The chosen window stays selected while it moves or its
  workspace is hidden. Closing the source ends its stream. The package's
  default chooser is a Wofi overlay that leaves the tiling layout in place.
  Its command is configured in `/etc/xdg/xdg-desktop-portal-wlr/Seaside`.
  The portal uses your per-user chooser config if you supply one. Isolated
  audio from one selected application is deferred. Receiving apps handle
  desktop audio and microphone selection.
- **Screenshots and panels.** `grim` is an optional screenshot utility. Once
  installed, run `grim -c "$HOME/seaside.png"` in the session to capture the
  desktop with the cursor. External panels that use Wayland layer-shell can
  run alongside Seaside. Ordinary layers hide while an app is fullscreen on
  that display and return afterward.

[Verified app and hardware coverage](PUBLIC_DOCUMENTATION.md#verification-and-support)

## Troubleshoot

| Symptom | What to check |
|---|---|
| `seaside FAIL: start on the active Linux console as a foreground job` | Log in on the currently visible virtual console and run `seaside` there as your normal user. `tty` should print a numbered `/dev/ttyN`. |
| The login has no standard Arch user runtime directory | Check that the normal systemd-managed login set `XDG_RUNTIME_DIR` to `/run/user/` followed by your user ID. Sign into a fresh local TTY if the login environment is stale. |
| `seaside` starts an older version | Run `type -a seaside seasidectl` to check for executables earlier on `PATH`, such as a previous source install under `/usr/local/bin`. Compare `seaside --version` with `/usr/bin/seaside --version`, and check the package version with `pacman -Q seaside`. |
| A service is unavailable in `seasidectl status` | Note the named service and reason. Use `systemctl --user status pipewire.socket wireplumber.service` and `journalctl --user -b` to inspect the relevant login services. If a portal name already belongs to another graphical session, exit that session normally or use another account. Seaside keeps the existing owner. |
| `seaside FAIL stage=seat-drm-renderer` in VirtualBox with 3D disabled | Run `WLR_RENDERER=pixman seaside` from the logged-in console. |
| The console reports a seat, DRM, or hardware-cursor error | Check the `seaside FAIL` line and earlier graphics messages. Seaside requires placement on a hardware cursor plane. On a running output, inspect `seasidectl outputs` and `seasidectl debug render`. |
| The screen is black after launch | Press **Super+Enter**. A working terminal on a black background is the default empty desktop. If the screen remains dark, press a key or move the pointer to wake it and check the console log on exit. |
| A `seasidectl` command cannot connect | Run it as your normal user from the Seaside terminal. It needs the running session's `SEASIDE_SOCKET` or `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR`. |
| A config error banner persists | Use the path and error from `seasidectl status`. Fix the reported file and save again. |
| Sharing has no picker or stream | Check the application's portal sharing mode and the installed portal backend and Wofi. Review the session log and `journalctl --user -b` for the failing component. If you use a local portal override, compare it with the package's installed routing and chooser defaults. |

For a reproducible startup log, run this on the active TTY as an ordinary
foreground command. After Seaside returns, inspect the log and exit status.

```sh
seaside >"$HOME/seaside-session.log" 2>&1
printf 'Seaside exit status: %s\n' "$?"
tail -n 50 "$HOME/seaside-session.log"
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

# Seaside

Seaside is a Wayland compositor for Arch Linux. It starts from a logged-in
Linux console and provides tiling or floating windows, workspaces, live TOML
configuration, and the `seasidectl` command-line client. Dwindle is the default
tiling layout. Scrolling columns are also available. You can add your own
Wayland panel or keep the default empty desktop.

Seaside is in development. An Arch VirtualBox guest has verified
TTY startup, window management, virtual display power, and real portal capture.
Native AMD display, gaming, and application compatibility checks are still
open. [Verification and support status](Specification/PUBLIC_DOCUMENTATION.md)

## Start a session

1. Install the Arch package from a supplied archive or build it from this
   repository's package recipe. [Install on Arch](Specification/USER_GUIDE.md#install-on-arch)
2. Log into an active Linux virtual console (TTY) as your normal user and
   run `seaside` in the foreground. The default desktop is black with no
   windows. Press **Super+Enter** to open the `foot` terminal. **Super** is
   usually the Windows or Meta key.
3. Edit the generated `~/.config/seaside/config.toml` to change settings and
   shortcuts, or run `seasidectl status` in `foot` to inspect the session.
   Run `seasidectl exit` in `foot` to return to the original console.
   [Configuration and shortcuts](Specification/USER_GUIDE.md#configure-seaside) · [Command reference](Specification/USER_GUIDE.md#control-the-session)

## Find what you need

- [Troubleshoot startup, output, or capture](Specification/USER_GUIDE.md#troubleshoot)
- [Use X11 apps, audio, and screen sharing](Specification/USER_GUIDE.md#use-applications-and-sharing)
- [Build, test, and contribute](Specification/CONTRIBUTING.md)
- [Engineering design, development handoffs, and native checklist](Specification/README.md)

Seaside is licensed under the [MIT License](LICENSE). Third-party dependencies
retain their own licences.

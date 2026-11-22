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

## Install on Arch

Use an up-to-date x86-64 Arch Linux installation.
[System requirements](Specification/USER_GUIDE.md#install-on-arch)

### Build and install from the checkout

Run these commands as your normal user. `makepkg -si` installs dependencies,
builds the package, and installs it through pacman.

```sh
sudo pacman -Syu --needed base-devel git
git clone https://github.com/Zelmari/seaside.git
cd seaside
staging=$(mktemp -d)
packaging/prepare-source "$staging"
cd "$staging"
makepkg -si
```

### Install a package archive

Download the `.pkg.tar.zst` archive and `SHA256SUMS` from the
[GitHub releases page](https://github.com/Zelmari/seaside/releases). Substitute
the archive filename below and compare the first command's output with its
entry in `SHA256SUMS` before installing. Use the same commands with a newer
archive to upgrade. Your configuration stays in `~/.config/seaside/` unless
you use a custom `$XDG_CONFIG_HOME`.

```sh
sha256sum ./seaside-example-x86_64.pkg.tar.zst
sudo pacman -U ./seaside-example-x86_64.pkg.tar.zst
```

## Start a session

1. Log into an active Linux virtual console (TTY) as your normal user and
   run `seaside` in the foreground. The default desktop is black with no
   windows. Press **Super+Enter** to open the `foot` terminal. **Super** is
   usually the Windows or Meta key.
2. Edit the generated `~/.config/seaside/config.toml` to change settings and
   shortcuts, or run `seasidectl status` in `foot` to inspect the session.
   Run `seasidectl exit` in `foot` to return to the original console.
   [Configuration and shortcuts](Specification/USER_GUIDE.md#configure-seaside) · [Command reference](Specification/USER_GUIDE.md#control-the-session)

In VirtualBox with 3D acceleration disabled, launch with
`WLR_RENDERER=pixman seaside`. [Startup help](Specification/USER_GUIDE.md#start-and-exit)

## Find what you need

- [Troubleshoot startup, output, or capture](Specification/USER_GUIDE.md#troubleshoot)
- [Use X11 apps, audio, and screen sharing](Specification/USER_GUIDE.md#use-applications-and-sharing)
- [Build, test, and contribute](Specification/CONTRIBUTING.md)
- [Development handoffs and native checklist](Specification/README.md)

Seaside is licensed under the [MIT License](LICENSE). Third-party dependencies
retain their own licences.

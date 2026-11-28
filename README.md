# Seaside

Seaside is a Wayland compositor for Arch Linux. It starts from a logged-in
Linux console and provides tiling or floating windows, workspaces, live TOML
configuration, and the `seasidectl` command-line client. Dwindle is the default
tiling layout. Scrolling columns are also available. You can add your own
Wayland panel or keep the default empty desktop.

Stable releases are available for Arch Linux. An Arch VirtualBox guest has
verified TTY startup, window management, virtual display power, and real portal
capture.
Native AMD display, gaming, and application compatibility checks are still
open. [Verification and support status](Specification/PUBLIC_DOCUMENTATION.md)

## Install or upgrade on Arch

On x86-64 Arch Linux, paste this block into a Bash terminal. It updates Arch,
downloads the latest stable Seaside release, verifies its checksum, and
installs it with pacman. Use the same block for a first install or an upgrade.

```bash
(
  set -euo pipefail
  sudo pacman -Syu --needed curl jq

  downloads=$(mktemp -d)
  trap 'rm -rf -- "$downloads"' EXIT
  cd "$downloads"

  release_info=$(curl -fsSL 'https://api.github.com/repos/Zelmari/seaside/releases/latest' |
    jq -er '[
      .tag_name,
      ([.assets[].name | select(test("^seaside-[0-9][A-Za-z0-9._+-]*-x86_64\\.pkg\\.tar\\.zst$"))] |
        if length == 1 then .[0] else error("Expected one Seaside Arch package") end)
    ] | @tsv')
  IFS=$'\t' read -r tag package <<< "$release_info"
  release="https://github.com/Zelmari/seaside/releases/download/$tag"
  curl -fsSL --fail-early -O "$release/$package" -O "$release/SHA256SUMS"
  awk -v package="$package" '$2 == package' SHA256SUMS | sha256sum --check --strict
  sudo pacman -U "./$package"
)
```

After upgrading, save your work, run `seasidectl exit`, and start `seaside`
again from the TTY to use the new version. Pacman keeps your user configuration.
[Release notes](https://github.com/Zelmari/seaside/releases) ·
[System requirements](Specification/USER_GUIDE.md#install-on-arch) ·
[Build from source](Specification/USER_GUIDE.md#build-and-install-from-source)

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

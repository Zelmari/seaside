# Seaside’s private Xwayland bridge

The helper installed as `seaside-xwayland-satellite` is xwayland-satellite 0.8.3 with support for transferring the clipboard and PRIMARY selection through `ext-data-control-v1`. Seaside launches a private copy for its own X11 display. [Upstream project](https://github.com/Supreeeme/xwayland-satellite)

The upstream authors license xwayland-satellite under the Mozilla Public License 2.0. The same license covers the modified bridge sources and the accompanying patch. The embedded Open Sans font is copyright 2020 The Open Sans Project Authors and is distributed under the SIL Open Font License 1.1. The package includes both license texts and the copyright and license notices for Rust libraries linked into the Arch executable. [Mozilla Public License](LICENSE) · [Open Sans license](OFL.txt) · [Linked library notices](THIRD-PARTY.txt)

## Obtain the exact modified source

Use the Seaside source revision printed by `seaside --version`. The directory `packaging/xwayland-bridge/` at that revision contains `data-control.patch`. The installed package also includes the patch under `/usr/share/doc/seaside/xwayland-bridge/`. The data-control backport incorporates changes by Dzming Li and ParticleG in upstream pull request 431, through commit `090e9e7449f7f45ac2b64231acc6fed46a5e882a`, relative to `8575d0e`. Seaside also uses the current X server time to claim a new Wayland selection after the previous owner clears it. [Seaside source](https://github.com/Zelmari/seaside) · [Upstream changes](https://github.com/Supreeeme/xwayland-satellite/pull/431)

The upstream release commit is `b83eab900644e4c7c77982ce3d44cb490f0c5e1d`. Its archive has SHA-256 `45f06d0c7017d793fa20c85a6fa7e8b26ad3c681c579d455f697faddf3c57bdf`. Extract that archive and apply `patch -p1 < /path/to/data-control.patch` in its top-level directory to obtain the modified source. [Exact upstream archive](https://codeload.github.com/Supreeeme/xwayland-satellite/tar.gz/b83eab900644e4c7c77982ce3d44cb490f0c5e1d)

From the matching Seaside checkout, `tools/prepare-xwayland-bridge` verifies the archive, fetches the upstream lockfile's Cargo dependencies and builds the bridge. Use `--prepare --archive PATH --work-dir DIR` followed by `--build --work-dir DIR --output PATH` for separate source preparation and offline compilation. The release build enables the upstream `systemd` feature.

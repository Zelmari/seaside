# Seaside — v1 Engineering Design

**Status:** Ready to start scoped engineering/prototype work; build, integration and hardware qualification pending (§§12–14)  
**Updated:** 2026-11-19  
**Owner-approved product behavior:** frozen provisionally; changes require explicit approval  
**Target:** Arch Linux only (initial development and testing inside an Arch VM hosted on Windows; final validation on native Arch / Framework Desktop, Ryzen AI Max+ 395 / Radeon 8060S)  
**Implementation:** standalone C Wayland compositor using wlroots; no required desktop shell

> **Source of truth:** This file and the owner's subsequent explicit corrections define the current requirements. The historical files `SEASIDE_DECISION_ARCHIVE.md` and `SEASIDE_REPO_AUDIT.md` were referenced by the supplied draft but are **not present in this workspace**. Do not invent their contents or assume they authorize features. If recovered, treat the former as historical decisions and the latter as research; the newest explicit owner decision controls. Commented TOML examples and implementation convenience do not create approved features.
>
> **Labels:** **APPROVED** = confirmed behavior; **ENGINEERING CHOICE** = minimal internal implementation of already-approved behavior; **OPEN** = details not yet specified; **APPROVAL NEEDED** = user-facing behavior or a consequential technical tradeoff that needs the owner's decision; **DEFERRED** = explicitly excluded from v1. No coding agent may silently promote **OPEN/APPROVAL NEEDED** to **APPROVED**.

**Owner-directed use of existing protocol implementations:** Wayland protocols define communication and surface/input/buffer contracts; wlroots provides compositor building blocks and helpers. Neither supplies Seaside's window-management policy or a complete set of desktop defaults. Implement the behavior approved in this document using those existing mechanisms and the selected external tools. Do not ask the owner to design every callback, parser tie-break or ordinary implementation detail. Where a protocol leaves a choice, follow Seaside's specified policy; agents may resolve remaining routine engineering details as **ENGINEERING CHOICE**, recording the choice and relevant verification with the task. Ask only when a real conflict would add functionality, change an approved interaction, weaken an explicit requirement, or make a consequential tradeoff. Do not import another compositor's policies or add settings, alternate modes, custom permission UI or bespoke media infrastructure merely to resolve an edge case. This rule does not turn unapproved product proposals into approved features.

## 1. Purpose, scope, and non-goals

Seaside is a **Wayland compositor with an integrated window manager**, not a complete desktop environment. The future **Conch** desktop shell is a separate project. Seaside must launch and work without Quickshell, Conch, a dock, a taskbar, a launcher, a notification daemon, or any persistent graphical shell process. It supplies a wallpaper/background by itself and optionally accepts external layer-shell clients.

**Goals:** excellent gaming responsiveness and frame pacing; reliable daily work/study/coding; minimal code and idle CPU/GPU work; maintainable small modules; explicit diagnostics for capability limits; predictable behavior. Reducing source lines is not a performance metric.

**APPROVED appearance defaults:** 10 logical px outer tiling gaps, 5 logical px inner gaps, and 2 logical px square borders. These are user-configurable defaults through `window.gaps_outer`, `window.gaps_inner`, and `window.border_width` in the TOML file; accepted ranges and live application are specified in §8.2.

**Fixed aesthetic constraints:** Square borders; no rounded corners, blurs, shadows, fading, sliding, layout easing, or any compositor animation engine. Game and application animations are unaffected.

**Initial platform:** develop, compile, run and package for **Arch Linux only**. **All early testing will occur in an Arch Linux guest VM hosted on Windows.** The Windows host is a development/test host only, not a supported Seaside target and not a reason to add Windows-specific compositor code. Later, qualify rendering and gaming features on native Arch using the Framework Desktop AMD graphics stack. Do not promise first-release Intel/NVIDIA qualifications; don't add unnecessary AMD-specific branching when standard DRM/wlroots APIs suffice.

**Explicit v1 exclusions:** no Conch source or Conch configuration; no shell-required features; no graphical settings utility; no compositor notification service; no application-specific window rules; no minimization; no built-in clipboard persistence/history; no secure session locking; no HDR yet; no color-effects shaders for night light; no software cursor fallback; no dedicated system shutdown/reboot/suspend code, confirmation UI, or power IPC; no display manager integration; no automatic Seaside relaunch after reboot; no per-device input rules unless separately approved; no scratchpad, always-on-top, or opacity rules unless separately approved.

**Future intention:** HDR and proper HDR/SDR color handling are planned for a later release, and secure session locking was deferred. Avoid architectural dead ends, but **don't add speculative HDR/locking infrastructure to v1**.

**DEFERRED audio integration:** Isolated application audio accompanying a chosen-window share is a future requirement (§7.5, G15), not a v1 implementation task or release blocker. Preserve the accepted application-level granularity in the design; do not add speculative audio-routing infrastructure while upstream client/portal support is unresolved. Single-window **video** sharing remains required in v1.

## 2. Runtime architecture and process boundaries

```text
TTY shell -> seaside (one compositor process)
                   |
                   +-- libwayland-server / Wayland clients
                   +-- wlroots: DRM/KMS, seat/input, output, scene, renderer
                   +-- internal WM (windows/workspaces/dwindle/scrolling)
                   +-- internal config, idle, diagnostics and local IPC
                   +-- on-demand xwayland-satellite -> Xwayland (external)
                   |
                   +-- optional normal Wayland clients:
                       Quickshell today / Conch later / terminal / game / bar

External OS user services: systemd user manager, D-Bus, PipeWire,
xdg-desktop-portal frontend + tested portal backend.
```

- **APPROVED:** No second compositor, web service, GUI daemon, persistent Conch process, or custom session manager. `xwayland-satellite` and the approved OS integrations are separate *existing* software, not code copied into Seaside.
- **APPROVED:** Seaside presents desktop wallpaper (the shipped default asset is a fully black image) without an external shell. A configurable absolute wallpaper path selects a different image. Covered wallpaper must not be needlessly drawn in fullscreen.
- **APPROVED:** `wlr-layer-shell` support permits external panels, overlays and launchers; nonessential shell overlays are suppressed on an output with fullscreen content. This is not a blanket promise to hide security-critical or protocol-required surfaces.
- **APPROVED:** Unix-socket IPC is served inside the compositor, without a separate daemon. No IPC work may block the render/input event loop.

### 2.1 Internal C modules (engineering structure; not additional features)

| Module | Owns | Communicates with |
|---|---|---|
| `server` | Wayland display, startup/shutdown, central event dispatch | all modules |
| `session` | TTY/seat acquisition, VT pause/resume, systemd/D-Bus environment, startup health checks | output, input |
| `output` | DRM/KMS modes, scale, transform, placement, primary output, hotplug, hardware cursor and hardware color capability | scene, workspaces |
| `render` | scene, damage, frame callbacks, direct scanout attempt/fallback, explicit sync, commit/presentation timing | output, diagnostic |
| `input` | libinput/XKB, focus events, cursor/pointer constraints, wheel dispatch, keyboard/mouse bindings | WM and scene |
| `window` | xdg_toplevel mapping, surfaces, geometry, floating/maximized/fullscreen/dialog states, ephemeral IPC IDs | workspace, layouts |
| `workspace` | global IDs, mode/ownership, visible workspace per output, histories and reconnect records | window, output |
| `layout/dwindle` | binary split trees, split/insert/remove/resize/drag reinsert | workspace |
| `layout/scrolling` | horizontal one-window columns, sizes, order, viewport offset | workspace |
| `config` | TOML parser, compiled defaults, validator, snapshot/reload, diagnostic banner | all affected modules |
| `ipc` | same-user socket, status/commands/events, backpressure, `seasidectl` client contract | server |
| `idle` | 15-minute idle-off timer, supported inhibitors, wake | output/input |
| `capture` | approved output and selected-window capture protocols; no encoder/UI/audio mixer | render, window |
| `satellite` | X11 socket setup and lazy external child activation | session |
| `diagnostic` | on-demand queries and bounded temporary tracing | render, output |

**Invariants:** The main event loop must not block on subprocess startup, D-Bus replies, disk writes, IPC subscribers or trace export. Outputs are not windows; shell layer surfaces are not ordinary tiled clients; layout engines compute geometry without owning GPU buffers. The exact threading strategy is **ENGINEERING CHOICE**; no separate permanent worker processes are authorized.

### 2.2 Core object model and lifetimes

```text
Server
  Output[0..N]         physical or supported output endpoint, possibly none
    currently_shown: WorkspaceID or none
    cursor_capability, frame_state, mode, scale, transform
  Workspace[1..N]      globally unique positive ID, never mirrored
    owner_or_last_output, focus_history, window membership, layout_state
  Window[0..N]         ephemeral internal ID + Wayland toplevel reference
    home_workspace, initial_mapping_seq, temporary_tiling_override
    original_floating_rect, pinned, fullscreen, maximized, dialog_parent
  DragState?          temporary move/reinsert operation, not persistent mode
  Config              active effective settings + pending-restart values
  RecoverySnapshot?   last fully accepted TOML on disk
```

**Lifecycle contract:** Map only fully initialized toplevel surfaces; track pending configure/ack/commit; unmap removes a surface from focus/layout without assuming the Wayland object is destroyed; destroy invalidates all references and IPC IDs; a closing focused window selects the approved focus successor. Transfer/drag/pin/maximize operations must not leave duplicate window ownership or dangling scene nodes. Never treat unmapped windows as focus candidates. All state transitions must be testable headlessly.

**One output can disappear without destroying windows.** If all physical outputs disappear, keep the Wayland server, clients, workspaces, and logical state running, but stop physical presentation until an eligible output returns. A physical output that cannot use a hardware cursor is disabled, not replaced with a software cursor.

## 3. Startup, session, recovery and shutdown

**APPROVED launch path:** Log into Arch on a TTY and run `seaside`. No display manager or session `.desktop` entry in v1. On clean exit, return to the same logged-in TTY; `exit`/Ctrl+D then logs out normally. Other TTY logins are unaffected. The compositor survives VT switching and system suspend/resume, reacquiring input/DRM/output/hardware-cursor resources as necessary.

**Boot sequence / staging contract:**

1. Parse or generate the canonical TOML file (§9). If invalid, use the last-known-good snapshot; if none exists, use built-in defaults. Show the persistent error until user TOML is corrected. A missing first-use file creates a fully documented default atomically, never overwriting a pre-existing file.
2. Acquire the already-active TTY session and seat/DRM resources through supported Linux APIs. Initialize Wayland socket, backends, required protocols, output scene and input. Select/validate hardware cursor for each enabled output; disable output individually when unsupported.
3. Prepare `XDG_SESSION_TYPE=wayland`, `XDG_CURRENT_DESKTOP=Seaside`, `WAYLAND_DISPLAY` and the session bus/systemd user environment. Prepare the lazy X11 listening socket and `DISPLAY`; do **not** start satellite until an X11 client connects.
4. **Strict preflight (APPROVED):** Before declaring the graphical session ready or running autostart, verify every *declared mandatory* session integration, specifically systemd user/D-Bus, portal frontend + compatible backend, and PipeWire. Checks must be **bounded, noninteractive, and distinguish missing service, failed activation, wrong environment and incompatibility**. On failure, release resources and exit to TTY with a useful error. Do not treat mere package installation as proof of operational readiness.
5. Start approved configured autostart commands **once**. The default list is empty. Enter ordinary event-driven service loop. During operation, errors of **external** portals/PipeWire are reported but do not close the Wayland session; allow normal external recovery. Fatal internal graphics/session failures are a different class.
6. On exit, close spawned integrations/sockets under Seaside's control, restore/clean up session-specific environment and release seat/DRM resources. Never end unrelated user sessions. Seaside does not auto-restart after system reboot.

**Important integration feasibility gate:** Some portal backends require an active Wayland compositor and may be D-Bus-activated only when needed. The above strict owner-approved startup gate must be *prototyped* with the actual chosen portal backend to avoid circular startup or a check that fails merely because an idle on-demand service has not yet started. **No backend is declared tested or preflight implementation declared resolved at this design stage.** Do not silently weaken the owner's strict policy; document and raise a blocker if it cannot be fulfilled correctly.

### 3.1 Arch dependency and TTY-session feasibility review — 2026-11-19

This is **ENGINEERING RESEARCH**, not a new feature authorization and **not a successful test on the Framework Desktop**. Package versions below are observed candidates from Arch's package index; they are **not a reproducible build lock** until an actual Arch build records package versions and artifact hashes.

| Candidate component | Observed Arch package/version | What the published packaging establishes |
|---|---|---|
| C compositor library | `wlroots0.20` `0.20.2-1` | Headers, shared object and `wlroots-0.20.pc` are shipped; this is a concrete C API target for the first prototype. [Arch package](https://archlinux.org/packages/extra/x86_64/wlroots0.20/) · [file list](https://archlinux.org/packages/extra/x86_64/wlroots0.20/files/) |
| On-demand X11 integration | `xwayland-satellite` `0.8.3-1` | Package depends on `xorg-xwayland`; Niri's documented working design keeps a listening socket and starts satellite on first X11 connection. [Arch package](https://archlinux.org/packages/extra/x86_64/xwayland-satellite/) · [Niri integration](https://niri-wm.github.io/niri/Xwayland.html) |
| Portal frontend | `xdg-desktop-portal` `1.22.1-2` | D-Bus-activated frontend; it selects backend implementations using desktop-specific `portals.conf`. [Arch package](https://archlinux.org/packages/extra/x86_64/xdg-desktop-portal/) · [portal integration](https://flatpak.github.io/xdg-desktop-portal/docs/system-integration.html) |
| Screenshot/screencast backend | `xdg-desktop-portal-wlr` `0.8.4-1` | Depends on PipeWire and a PipeWire session manager; implements screenshot and screencast backends, not the whole portal suite. The backend supports both ext-image-copy-capture and legacy wlr-screencopy paths; actual Seaside compatibility needs testing. [Arch package](https://archlinux.org/packages/extra/x86_64/xdg-desktop-portal-wlr/) · [upstream](https://github.com/emersion/xdg-desktop-portal-wlr) |
| Other portal methods | `xdg-desktop-portal-gtk` `1.15.3-2` | Candidate generic portal backend (e.g., file chooser); **whether it is mandatory in Seaside's package is not yet finalized**. [Arch package](https://archlinux.org/packages/extra/x86_64/xdg-desktop-portal-gtk/) |
| Stream service | `pipewire` (Arch repository) | Exposes a user `pipewire.socket`/service and normally starts when needed. **Do not fail preflight simply because `pipewire.service` has not started yet.** [ArchWiki](https://wiki.archlinux.org/title/PipeWire) |
| PipeWire session manager | `wireplumber` (Arch repository) | Available user service implementing PipeWire policy; required through `pipewire-session-manager` by the chosen screencast backend. [Arch package](https://archlinux.org/packages/extra/x86_64/wireplumber/) |

**Package policy:** A Seaside `PKGBUILD` will list needed runtime binaries and libraries as explicit hard dependencies when required by approved v1 functionality. **APPROVED:** include Arch’s `foot` package as a required default-terminal dependency; Seaside does not embed a terminal, start it automatically, or run a terminal daemon. Pacman manages transitive dependencies. Packaging is not proof that an activated user service can actually reach the Seaside Wayland socket. Do not embed PipeWire, D-Bus, systemd or portal implementations inside Seaside. Distinguish build-time version pins from optional debugging programs (`grim`, `slurp`, etc., some already pulled transitively).

**Audio dependency baseline (ENGINEERING CHOICE supporting approved defaults):** Declare the standard Arch packages `pipewire`, `pipewire-audio`, `pipewire-pulse` and `wireplumber` as runtime dependencies for the existing audio/capture integration and approved volume/mute defaults. `wireplumber` supplies `wpctl`; `pipewire-pulse` provides PulseAudio compatibility. They remain separately installed, maintained OS services: Seaside does not embed their source or binaries, create private audio-server instances, or start them through `autostart.commands`. Use and qualify the packages' standard user-service/socket activation during session integration. Pacman resolves dependencies during installation ([dependency contract](https://man.archlinux.org/man/PKGBUILD.5.en)); it must not be replaced with compositor-owned package installation or silent audio-service replacement. Record actual versions after a successful Arch build, rather than inferring compatibility from package presence. This does not authorize new mandatory startup probes beyond the existing §3 preflight contract.

**Portal routing (engineering integration, not a second Seaside user config):** Package a desktop-matched portal selection file under the system portal configuration directory. An illustrative file paired with `XDG_CURRENT_DESKTOP=Seaside` is `/usr/share/xdg-desktop-portal/seaside-portals.conf`:

```ini
[preferred]
default=gtk
org.freedesktop.impl.portal.Screenshot=wlr
org.freedesktop.impl.portal.ScreenCast=wlr
```

The desktop-specific filename is lowercase (`seaside-portals.conf`) per the portal lookup convention; **the backend names and actual selection are not final until tested**. This is a package-owned **OS service-selection file**, not another user-editable `seaside` settings file. External portal backends are necessary for the previously approved capture/Flatpak integration; their own implementation is not part of Seaside. [Portal backend configuration reference](https://flatpak.github.io/xdg-desktop-portal/docs/configuration-file.html).

**Session activation ordering (provisional technical contract consistent with Questions 83–85):**

1. Obtain the TTY/logind seat, construct a live Wayland server socket and required compositor globals, and begin servicing the event loop **before attempting to activate backends that must connect as Wayland clients**.
2. Propagate `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP`, `XDG_SESSION_TYPE` and any required `DISPLAY`/service environment into the D-Bus activation environment and the existing systemd user manager; coordinate `graphical-session.target` lifecycle without killing an unrelated graphical/TTY session. The normal `seaside` TTY entrypoint performs this integration, without a display manager or custom session daemon. [Portal system integration](https://flatpak.github.io/xdg-desktop-portal/docs/system-integration.html) · [systemd graphical session target](https://www.freedesktop.org/software/systemd/man/latest/systemd.special.html) · [Niri TTY session reference](https://github.com/niri-wm/niri/blob/main/resources/niri-session).
3. Perform the **owner-approved strict** startup health gate using *bounded, asynchronous, noninteractive* checks while continuing to dispatch Wayland/D-Bus events: verify the user service manager/bus, prove a real PipeWire core connection (not merely process presence), and activate/query the portal frontend and its intended capture backend against this session. Services that are socket- or D-Bus-activated are allowed to start as part of the check. Abort startup and cleanly return to the TTY if any mandatory check fails.
4. Only after the gate passes mark session ready and run configured autostart once. If an external service fails **after** readiness, report the fault and preserve clients/gameplay per Question 85.

**Feasibility limit:** Portal D-Bus introspection and PipeWire connections can demonstrate service reachability; they cannot establish that interactive selection, permission grants, DMA-BUF negotiation and actual screencast frames will work. Exercising those requires an end-to-end test on a running graphical session, not a headless preflight. **G2 is OPEN/BLOCKING** until we test a noninteractive service readiness probe and independently run successful capture/recording tests on the target AMD hardware. Do not convert "service process not currently active" into an error, fake a healthy portal, hang the renderer waiting for the portal, or silently downgrade the owner's strict validation rule.

**Concrete Arch validation commands to run inside a prototype Seaside session (not performed in this review):**

```sh
pacman -Q wlroots0.20 xwayland-satellite xdg-desktop-portal xdg-desktop-portal-wlr pipewire wireplumber
pkg-config --modversion wlroots-0.20
systemctl --user is-active graphical-session.target
systemctl --user show-environment | grep -E '^(WAYLAND_DISPLAY|XDG_CURRENT_DESKTOP|XDG_SESSION_TYPE|DISPLAY)='
systemctl --user status pipewire.socket
gdbus introspect --session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop
```

A working PipeWire connection, correctly selected active backend and successful OBS/Discord/screenshot capture require additional active runtime checks; merely passing these shell commands is **not** a release pass. Do not launch the compositor itself inside the build agent's container and claim native DRM/seat qualification.


**External power operations:** `systemctl suspend`, `systemctl reboot` and `systemctl poweroff` are Arch/systemd commands. They may be run through the already-approved generic `exec` binding, with **no default power bindings**. No Seaside-owned power menu, confirmation API, or special power IPC.

### 3.2 Session resource ownership (ENGINEERING CHOICE)

- Track compositor-owned sockets, child processes and environment changes explicitly. Close/reap the owned satellite child at shutdown. Arbitrary `exec`/autostart applications and pre-existing PipeWire, D-Bus or portal services are not permission to kill a user's process tree or stop unrelated services.
- The systemd user manager and D-Bus activation environment can be shared by other logins. Save the previous values of variables Seaside changes and restore them only if their current values still match this instance's writes. Coordinate the graphical-session target through the standard service lifecycle; never stop a target or restart a shared portal solely to repair another session's environment.
- A pre-existing portal bound to the wrong Wayland session is a real readiness failure, not proof that this session is healthy. Detect/report the conflict during bounded preflight and return to TTY if it cannot be resolved through the chosen standard integration without disrupting another session. Do not invent a multi-session manager or silently weaken startup checks.
- Implement preflight as asynchronous states with an initial engineering budget of 30 seconds overall and at most 10 seconds per probe; continue dispatching Wayland/D-Bus during waits. Adjust technical timing only with recorded prototype evidence. Do not add a user-facing timeout setting, interactive wizard or success result based solely on process presence.

## 4. Windows, layouts, focus and movement

### 4.1 Global layout authority and window states

- **APPROVED:** `[window] tiling = true` and `layout = "dwindle"` by default; allowed layout values `dwindle` and `scrolling`. These defaults apply to **every** workspace, regardless of workspace mode. Global `tiling=false` makes ordinary windows floating.
- **APPROVED:** `Super+D` temporarily toggles the currently focused *ordinary* window's tiled/floating status; it does **not** change the global config. Configuring another shortcut is allowed. On **actual change to the global tiling or layout value**, clear temporary per-window layout overrides on **all** workspaces. Unrelated config edits do not reset overrides.
- **APPROVED:** Preserve usable floating geometry when a window is tiled then returned to floating. Fullscreen and maximization are separate states. Dialogs float regardless of global tiling.
- **APPROVED:** Maximized windows use the available work area (taking layer-shell reservations into account); fullscreen windows occupy the selected output, suppress nonessential shell overlays and are eligible for direct scanout. Both support returning to the prior state and geometry. Applications cannot request minimization in v1; ignore iconify requests rather than introducing a hidden minimized state.
- **APPROVED:** Sticky *pinning* keeps one ordinary window visible while switching workspaces **on its own monitor**. It does not put it above everything or clone it. If tiled, pinning temporarily detaches it and makes it floating, reclaiming its old tile; unpinning returns it to tiled state if that was its pre-pin status. Originally floating windows stay floating after unpinning. Fullscreen games still suppress obstructing nonessential pinned/shell content as previously specified.
- **APPROVED:** No application-specific initial-state or workspace matching rules.

### 4.2 Dwindle: binary split tree (default)

- Each workspace owns a binary tree of tiled leaves and split nodes. The first eligible window occupies the available tiling area; subsequent windows split a selected tile at ratio `0.5` by default.
- `insertion = "focused"` (default) splits the currently focused eligible tile; `"largest"` splits the eligible leaf with greatest available tile area.
- **Split orientation:** If the selected available region is wider than tall, split left/right; if taller, split top/bottom. Dynamically recalculate orientation as geometry changes. A new window always gets the **right or bottom** side; the previous tile remains left/top. No optional alternating mode. Treat `split_direction="auto"` and `preserve_split=false` as fixed behavior rather than implementing more toggles.
- **Remove:** Remove the closing tiled leaf and its split parent, promote the sibling (possibly a subtree) to the parent's slot; keep the rest of the tree arrangement. This is the approved Hyprland-style collapse.
- **Resize:** `Super + Right Mouse Drag` adjusts affected split ratios, not window floating status, respecting client min-size constraints where feasible. Keyboard resize is supported but **unbound** by default. Ratios must be clamped to safe geometry; exact numeric bounds are engineering details.
- **Move/reinsert:** `Super + Left Mouse Drag` on a tile temporarily detaches it, collapses the previous tree position and lets it follow the pointer. On valid drop, reinsert near the target at the cursor-selected side, including left/top; the window is tiled afterward. This differs from normal right/bottom new-window insertion. There are **no animations**. Invalid drop and cancellation use conservative deterministic behavior (e.g. restore previous valid membership), to be tested rather than exposed as a new product option.
- **Keyboard actions:** Directional tile move/reorder, swap, and resize exist but have **no default bindings**. Their full grammar must be specified in §9 before a generator/agent adds active keys.

### 4.3 Scrolling: one window per horizontal column

- **APPROVED:** A workspace has an ordered unbounded horizontal strip; each column contains **one** ordinary tiled window, with **no vertical stacking**. New windows go immediately **right of the focused column**, receive focus, and are revealed immediately.
- **APPROVED:** New width defaults to **50%** of available logical monitor width (`default_column_width = 0.5`). Respect reported minimum window dimensions wherever physically possible; columns may be wider than 50% or even wider than the viewport. Columns can be individually resized to full usable width or another width. A column resize changes **only that column's width**; neighboring widths remain unchanged and their positions shift.
- **APPROVED:** The initial strip starts at the **left usable edge**, not centered. Focus navigation includes off-screen columns. Shift the viewport by the **smallest distance necessary** to reveal a selected column if it fits, otherwise reveal its relevant edge. Never animate. When the focused column is already completely visible, don't move the viewport. Wrap only at the actual logical first/last column, not the visible screen edge.
- **APPROVED:** When a column closes, all columns on the right slide left to eliminate the gap with their widths unchanged. If the closed column was focused, focus its immediate right neighbor; if none, its left neighbor. Closing an unfocused column does not steal focus.
- **APPROVED:** Switching to another workspace and back preserves that workspace's order, per-window custom widths, viewport offset, and eligible focus. **Global layout changes discard inactive layout history.** Rebuild the new selected layout from surviving windows' **original opening order (oldest to newest)**. On rebuild to scrolling, widths reset to the configured 50% default and the strip restarts at the left. Switching to dwindle rebuilds its tree using the normal insertion policy. An optional future spatial-order conversion is **not a v1 requirement**.

### 4.4 Focus and input routing

- **APPROVED:** Click-to-focus only; moving the cursor over a window does not change keyboard focus. Clicking a floating window raises it, clicking a tile does not reorder it. Wheel/trackpad scroll over an unfocused window is delivered to the hovered pointer surface **without changing keyboard focus**. Client pointer locks/constraints take precedence over hover scrolling.
- **APPROVED:** `Super + Arrow` focuses a spatially adjacent eligible window on the **same workspace and output**; at the edge it wraps to the opposite side within that workspace/output. The mouse cursor never warps when keyboard focus changes, even when keyboard focus moves between monitors. Scrolling windows outside the viewport remain navigation candidates and are revealed instantly.
- **APPROVED:** `Alt+Tab` cycles eligible windows by most-recent focus on the **current workspace only**; no UI and no cross-workspace jump. It is remappable.
- **APPROVED:** New ordinary windows mapped on the currently focused workspace receive keyboard focus. New windows on an unfocused/hidden workspace do **not** steal focus or change displayed workspaces. The optional Conch shell may learn about them through an event feed; Seaside doesn't draw a notification.
- **APPROVED:** Returning to a workspace restores its last-focused mapped eligible window, or next most recent eligible, or no client. When the focused dwindle window closes, prefer a suitable spatial sibling/nearby eligible window, falling back to MRU focus; for scrolling use the right-then-left rule above.
- **APPROVED dialog behavior:** Parent-associated dialogs float, open centered over parent (otherwise on destination monitor), follow parent movement/resize keeping a user-adjusted relative offset, and follow parent across workspaces/monitors. Their stacking is **independent**: raising the parent can place it in front of its dialog. Dialogs from the active application may focus; dialogs from background applications never steal focus. Follow protocol-defined popup and modality requirements rather than inventing new configurable dialog rules.
- **APPROVED:** Every configured compositor shortcut remains available during fullscreen gaming, taking priority over conflicting client chords. Do not advertise keyboard-shortcut inhibition if Seaside will not grant it. Pointer lock and relative-pointer support remain mandatory for games.

### 4.5 Engineering rules for ordinary transitions

Use the approved state model with standard protocol lifetimes, rather than adding more user options. The following resolve implementation details within that model:

- **State restoration:** Fullscreen temporarily overrides maximized/ordinary geometry; leaving it restores the prior state, including maximization when applicable. Maximization restores the saved ordinary geometry on exit. Preserve ordinary layout membership and floating restore geometry separately from the currently displayed rectangle. Protocol-defined configure/ack/commit sequencing still applies.
- **Pinning:** Pinned windows remain floating while pinned and stay below fullscreen content as already required. Global layout changes rebuild tiled participants without unpinning windows. On unpin, apply the previously approved return-to-tiling rule, respecting any intervening global tiling change and cleared temporary overrides. A move across workspaces/outputs updates the one window's home/monitor association; never create a second copy.
- **Determinism:** Resolve equal split dimensions with a left/right split, equal largest-tile candidates by original opening order, and exact remaining ties by internal ID. A missing eligible focused tile uses the largest eligible tile. Keep these as internal rules, not new settings.
- **MRU cycling:** Use the conventional modifier-held Alt+Tab behavior: keep the eligible MRU list stable during the sequence, advance on repeated activations, then commit focus history when the modifier is released or another action ends the sequence. A standalone IPC invocation performs one MRU step without a UI or synthetic held modifier. Removed/unmapped windows are never candidates.
- **Directional operations:** They act within the window's workspace/output. Focus wraps as specified; move/swap/resize do not transfer to another monitor or workspace as an implicit edge fallback. Scrolling columns support horizontal reorder/width changes; a vertical operation with no applicable target is a no-op for a binding and `invalid_state`/`no_target` for IPC. Floating movement uses a 32-logical-pixel engineering step; swap is tiled-only. Implement the minimal layout-specific geometry operation, document its deterministic behavior with the task, and do not introduce movement modes or extra actions.
- **Protocol mechanics and Seaside policy:** Use existing implementations for popup grabs, surface trees, dialog lifecycle and client size constraints. Apply Seaside's approved window-management policy within the freedoms those protocols permit. If an approved behavior conflicts with a mandatory protocol rule, report that concrete conflict rather than violating the protocol or silently changing the requirement. Do not assume wlroots chooses focus, workspace, tiling or stacking policy, and do not build an application-specific rule system to work around a client quirk.

## 5. Workspaces, monitors, and disconnections

### 5.1 Workspace identities and switching

- **APPROVED:** Workspace IDs are **globally unique integers in both modes**. A workspace is never visible on more than one monitor. No mirrored/cloned workspaces.
- `[workspaces] mode="global"` is the default. Existing visible workspaces selected by shortcut receive focus **in place** even on another monitor. Existing *hidden* workspaces open on their **last assigned connected monitor**, otherwise the currently focused one. Newly created workspaces begin on the **currently focused monitor**, remembering it as their initial assignment.
- `mode="per-monitor"` is the alternative. Workspaces have monitor owners but still globally unique IDs. Selecting one owned elsewhere focuses that monitor and reveals the workspace **there**, without transferring ownership. Newly created workspaces belong to the currently focused monitor.
- **APPROVED:** A workspace mode change is **restart-only**. Live reload can validate and report the new value as *pending restart*, but must not migrate existing ownership or restart the Wayland session; other valid changes still apply.
- **APPROVED:** On startup, the designated primary monitor is focused even if no client has focus. Exactly one connected usable monitor is logically focused while any exists. Workspace selection restores focus history; pointer location stays unchanged.
- **APPROVED window move follows focus:** `move_to_workspace ID` and IPC `action.window_to_workspace` move the target ordinary window, select/reveal the destination workspace, and give keyboard focus to the moved window. Resolve the destination's output using the normal workspace-selection rules above: an existing destination on another connected monitor is followed there, without transferring the workspace to the source monitor. A new destination is created on the currently focused monitor. The explicit moved-window focus takes precedence over the destination's saved focus history. This behavior is the same in both workspace modes; the pointer does not warp.
- **APPROVED explicit transfer:** Moving a workspace to another monitor shows it there, hides (not swaps) the destination's former workspace, reassigns ownership in per-monitor mode, and focuses the moved workspace. The source shows its most recently used eligible hidden workspace, or creates an empty uniquely numbered workspace. Transfer is distinct from selecting an already-visible workspace.

### 5.2 Monitor selection and configuration

- **APPROVED:** `[monitors] primary="auto"` by default; a user can choose a physical monitor explicitly. For automatic primary selection, rank available monitors by **maximum supported refresh Hz**, then maximum supported pixel resolution, then **deterministic connector order**. Keep the selected primary stable until it disappears; do not opportunistically switch merely because a faster monitor appears.
- **APPROVED:** Physical monitor identity is preferred even if its cable or connector changes. If hardware serial data is absent/ambiguous, use **best-effort matching from model, resolution and connection history**, accepting the risk of an incorrect match. Never merge distinct outputs into one merely because they share a model.
- **APPROVED:** Per-output default display mode = **native/preferred resolution at the highest refresh rate available at that resolution**. `resolution` and `refresh_rate` are independently overrideable per monitor. Do not cap game FPS or automatically switch modes to match a fullscreen game's render resolution.
- **APPROVED:** Per-monitor scale defaults to `1.0`; fractional values (e.g. `1.25`, `1.5`) are supported from v1. Rotation can be `0`, `90`, `180`, or `270` degrees (default `0`). Place outputs automatically left-to-right beginning with the primary, with manual exact per-monitor positions supported. Account for transformed logical sizes under scaling/rotation.
- **APPROVED:** Hardware cursors are **mandatory** for every usable output, with **no software cursor or config escape hatch**. If hardware cursor support is absent, disable only that output and log/report the reason. Other outputs and clients continue; if none remain, keep the compositor running without physical presentation.

### 5.3 Hotplug, migration, restoration

1. On a secondary monitor disconnect, move its workspace set/windows temporarily to the designated **primary monitor**, preserving them and their contents. If the configured primary itself disconnects, use the **most recently focused surviving monitor** as temporary destination; if none has focus history, use the automatic primary ranking (refresh -> resolution -> connector order).
2. Keep a **migration record** of the output's physical identity, workspace membership, previous arrangement and whether the move was automatic. When the monitor returns, restore workspaces/windows that were *only* automatically migrated.
3. A manual reassignment of a temporarily migrated workspace while the original monitor is unavailable **takes priority** and cancels restoration for that workspace. Automatic focus/selection never counts as a manual reassignment.
4. Re-identify outputs using physical metadata where possible and best-effort history otherwise. Changes of HDMI/DP connector alone should not prevent restoration.
5. When **every** output disconnects, preserve all Wayland clients/windows/workspaces and logical state, stop presentation, and restore on reconnection. One output being disabled for mandatory hardware cursor failure is equivalent to it being unusable for the migration/zero-output policy.

**Engineering contract:** A monitor's stable identity, its current connector, its configured primary designation, its last known workspace ownership, and an automatic migration record are distinct values. A temporary output loss must not destroy the object representing its remembered assignment. Pointer and keyboard focus must reconcile safely when the prior focused output is gone.

## 6. Rendering, gaming, input devices and color

### 6.1 Per-output frame lifecycle (normative properties)

`client surface commit -> damage/presentation scheduling -> validate candidate direct scanout -> (direct scanout OR damage-aware GPU composition) -> output commit -> page-flip/presentation feedback -> next scheduled frame`

- **APPROVED:** Attempt direct scanout on *every eligible frame/surface*, not just games. Do not force it for multiple opaque tiles, overlapping windows, incompatible buffer modifiers/formats, scaling/transforms, capture, colour requirements, cursor constraints or other ineligible states. Normal composition is the required correct fallback; it is not a failure by itself.
- **APPROVED:** No artificial compositor-added queues, mandatory extra GPU passes, blind full-screen redraws, constant busy rendering or decorative animations. Idle without damage should be largely event-driven. Correct GPU synchronization and frame-callback scheduling take precedence over arbitrary CPU optimizations.
- **APPROVED:** Support explicit GPU synchronization where available (`linux-drm-syncobj` family and compatible wlroots APIs); correctly fall back to supported implicit synchronization for clients that don't use it. It is a buffer-lifecycle correctness requirement, not a toggle that forces unsupported clients to fail.
- **APPROVED:** The physical output uses the configured mode even when a fullscreen application renders at another resolution. When necessary, scale the *presented* surface. `[render] fullscreen_aspect` chooses `"preserve"` (**default**, letterbox/pillarbox) or `"stretch"` (distort to fill). Do not infer presented aspect ratio from a game's internal render resolution if the app already produces an output-sized buffer.
- **APPROVED:** A visible fullscreen application suppresses nonessential shell overlays on that output, does not force another monitor to change, and prevents that output's 15-minute idle sleep while visible. A config-error diagnostic banner is an approved exceptional overlay even in fullscreen and may prevent direct scanout until corrected.
- **APPROVED:** Real hardware cursors required, not configurable. Cursor plane behavior, transform/scale compatibility and cursor buffer constraints must be qualified on the target AMD GPU. If unsupported on an output, disable that output. **Nested development backends may not meet this requirement**; use headless layout tests and native DRM/KMS tests instead of silently introducing a software cursor path.
- **APPROVED VRR:** `adaptive_sync="off"` (**default**), `"fullscreen"` (eligible fullscreen outputs), `"always"` (where supported). Unsupported VRR remains fixed refresh; report actual applied state. Tearing remains `false` by default; when enabled, permit asynchronous presentation **only when an eligible fullscreen client requests it and hardware supports it**. Windowed clients remain synchronized. VRR and tearing are independent knobs.
- **APPROVED:** SDR v1. HDR implementation planned later; no half-working HDR color pipeline in v1. Hardware-only night light below must not force a GPU color shader.

**Engineering tracking:** Each output has a finite frame state (e.g. idle / scheduled / waiting for present / inactive), a bounded damage set, latest commit/presentation result and actual scanout/composition mode for diagnostics. Rejected atomic commits must trigger safe fallback/retry, not a render busy loop. Trace only when requested; actual input-to-photon latency needs external measurement equipment and cannot be inferred from software timestamps alone.

### 6.2 Input and cursor specifics

- **APPROVED:** Use libinput's `flat` pointer acceleration profile, speed/sensitivity `0.0` by default. No Windows pointer-curve emulation. The public libinput speed API accepts `-1.0 .. +1.0` for eligible devices; unsupported device features are reported rather than faked. No user-approved per-device configuration rules in v1.
- **APPROVED:** Correct Wayland pointer enter/leave/axis, relative pointer and pointer constraints/lock for games; maintain unaccelerated deltas when provided. Distinguish pointer focus from keyboard focus so background scroll does not steal keyboard focus. Do not warp the cursor during directional focus or monitor/workspace changes.
- **APPROVED:** Keybindings are fully configurable and remain effective even in fullscreen apps. Implement normal XKB keyboard mapping, modifiers and key repeat sufficient for desktop use as part of basic input correctness. Do **not** add new user-facing keyboard-layout/input-method configuration features without separate owner approval.

### 6.3 Night light (approved v1)

- A **single global enable state** (`false` by default) and **owner-approved 4500 K default warmth** apply to all outputs that can perform a suitable **hardware output color adjustment**. The temperature remains adjustable through `night_light.temperature_kelvin`.
- **APPROVED runtime control precedence:** A shortcut or IPC night-light action changes the effective enable state for this Seaside process. An unrelated TOML reload preserves that state. Only a committed change to the configured `night_light.enabled` value replaces it; compare against the previous accepted configuration value, not the effective runtime state. A temperature-only edit updates warmth while preserving the effective enable state. On a new Seaside process, initialize from the loaded configuration; runtime actions do not rewrite the user file or the recovery snapshot.
- No scheduling, sunrise/sunset/geolocation services, GPU shader fallback, per-monitor warmth, color transition animation or daemon.
- If an output lacks the requisite hardware control, leave its colors **and output availability** unchanged; report unsupported night light in logs/IPC. Unlike the mandatory hardware-cursor rule, night-light failure **must not disable a monitor**.
- Apply/remove settings through normal output capabilities on connection and reload; measure whether scanout eligibility changes on AMD and do not guarantee it. **ENGINEERING OPEN:** Qualify the exact hardware gamma/LUT route and any existing output colour-state interaction on the pinned wlroots version and target AMD GPU. The defensively proposed accepted range of 1000–10000 K is an engineering validation bound, not a second product feature. The 4500 K default is **approved**, not provisional.
- **API research, not hardware proof:** wlroots documents `wlr_output_state_set_gamma_lut()` on its output-state API ([reference](https://wlroots.pages.freedesktop.org/wlroots/wlr/types/wlr_output.h.html)); confirm availability and interaction with output commits in the **pinned 0.20 headers** before relying on it. A Kelvin value is a colour-temperature target, not a guarantee of calibrated output colour. Where hardware adjustment is unsupported, leave the monitor and its colours unchanged.

## 7. Shell interoperability, capture, clipboard and idle power

### 7.1 External shell boundary

- **APPROVED:** wlr-layer-shell permits independent Conch/Quickshell-like Wayland clients to place surfaces via layer ordering, anchors, exclusive zones and keyboard policies. Shell processes are never required or started by default; an empty autostart list remains empty.
- **APPROVED:** No compositor-drawn notification service, taskbar, dock, launcher or Conch-specific UI. Seaside provides events (window mapped/attention etc.) for a future shell to decide how to react; it never switches focus or displays a popup purely because of that event.
- **APPROVED:** A fullscreen output hides nonessential shell overlays and restores normal layer behavior when fullscreen ends. Handle protocol-required layer/keyboard semantics correctly.

### 7.2 Capture and portals

- **APPROVED:** Provide compositor-side protocols for existing tools (`grim`, compatible `wf-recorder`, OBS, screen-sharing apps via desktop portals/PipeWire). Do **not** write screenshot encoding, a recording application, capture UI, or the portal service inside Seaside.
- **APPROVED selected-window sharing:** v1 must support sharing one user-chosen application window through a compatible portal/backend and external sharing application. Whole-output capture alone does not satisfy this requirement. The stream targets the selected window's content rather than a rectangle cropped from the output; unrelated applications must not appear when they overlap it. Keep the same selected-window identity through movement and resize; never replace a lost/destroyed target with a whole-output capture or another window. Selection/consent is supplied by external tools/services. Native Wayland and satellite-backed X11 windows require compatibility tests. Keep video capture tied to the original selected toplevel, using the standard capture protocol's surface semantics; do not add custom aggregation of separate dialog windows or all windows belonging to the application. Ordinary surfaces belonging to the selected capture source follow that protocol rather than new Seaside-specific inclusion rules.
- **APPROVED sharing across workspace switches:** Switching to another workspace continues sharing the originally selected window, including live content updates while its workspace is hidden. The stream does not switch to the newly visible workspace or focused window, and capture does not bring the selected window's workspace back into view or take keyboard focus. Retain the selected-window target until the external capture session ends or the target is lost/destroyed. Capture scheduling must supply the frame callbacks/damage handling needed for an actively captured hidden window; ordinary visibility-based throttling must not freeze it merely because the user changed workspace. This work is scoped to an active request and stops when capture is unused.
- Capture must be **inactive when unused** and only do copying/compositing necessary for an active capture request. Direct scanout may become ineligible during capture; do not claim the capture path is free.
- Design toward `ext-image-copy-capture-v1` with both output and foreign-toplevel capture sources from `ext-image-capture-source-v1`, and `ext-foreign-toplevel-list-v1` for selecting a window; retain legacy `wlr-screencopy` where necessary for approved output-capture tools. **Select, implement and test the exact supported versions, external chooser and portal backend before release.** Capability and permission boundaries must be explicit. A capture protocol alone does not make a working portal or Discord screen share.
- **ENGINEERING RESEARCH:** The candidate `xdg-desktop-portal-wlr` v0.8.4 sources include the image-capture and foreign-toplevel-list protocol families ([protocol dependencies](https://raw.githubusercontent.com/emersion/xdg-desktop-portal-wlr/v0.8.4/protocols/meson.build)). This is a compatibility lead, not a passing Seaside test or a finalized chooser dependency. The portal distinguishes monitor and window source types ([ScreenCast API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)). Successful chosen-window capture in the actual external applications is required to close G6; an output-only fallback does not close it.
- **APPROVED Question 84:** Compatible portal/PipeWire services are **mandatory at launch** and undergo strict operational preflight; **APPROVED Question 85:** an external-service failure later must not crash/exit Seaside or games. This does not authorize remote-desktop input injection, additional portal APIs, or extra notification systems.

### 7.3 Clipboard and drag-and-drop

- Implement standard Wayland data-device/primary selection functionality needed for normal cut/copy/paste and drag-and-drop; copy/paste to/from XWayland satellite apps must be tested.
- **APPROVED:** Clipboard history and content persistence after the source application exits belong to external utilities. Seaside must not store a user clipboard-history database, provide clipboard UI, or introduce hidden persistent caches. A minimal compatible external clipboard-manager protocol may be necessary for the already-approved external-tool workflow; the exact access policy requires technical verification.

### 7.4 Built-in idle display power

- **APPROVED:** Event-driven idle power-off after `15` minutes by default, `0` disables. Honor supported client idle-inhibition requests. Any **visible fullscreen** application keeps its **own output** awake even without an explicit inhibition request; other outputs can sleep. Wake an asleep output from relevant local input.
- Idle power-off does **not** terminate applications or log out, and is not secure session locking. No idle daemon, custom power manager, or lock screen. This is separate from externally executed system suspend/reboot/poweroff.

### 7.5 Audio boundary, desktop sharing and deferred application isolation

**APPROVED sharing intent:** When shared sound is enabled in the external sharing application, a monitor share should carry desktop playback audio. For future isolated sound with a chosen-window share, **application-level isolation is acceptable** when individual windows of the same application cannot be distinguished. In that case, audio from other windows of the selected application may be included; unrelated applications must remain excluded from an application-audio share. These are external media-integration responsibilities, not authorization for a Seaside audio engine or mixer.

**DEFERRED beyond v1:** Isolated application audio for a chosen-window share does not block implementation or release of v1. Keep this requirement and its future acceptance cases recorded, but do not implement a custom solution or wait for upstream support before completing unrelated v1 work. Single-window video sharing remains mandatory and must not be advertised as including isolated audio without a successful end-to-end test.

| Video source | Intended shared audio | Boundary |
|---|---|---|
| One chosen window | Selected application's playback streams — **DEFERRED beyond v1** | Future application-level isolation is acceptable; v1 single-window video does not require isolated audio |
| Monitor | Desktop playback mix | Audio is not limited to applications whose windows are on that monitor; exact handling of multiple audio output devices remains OPEN |

**APPROVED default playback controls:** Bind the unmodified XKB media symbols `XF86AudioLowerVolume`, `XF86AudioRaiseVolume` and `XF86AudioMute` by default, using generic `exec` calls to external `wpctl` (§9). They control the current default playback device. On keyboards whose firmware uses Fn with F10/F11/F12 (or another row), bind the resulting media symbol; the physical function-key positions and Fn-Lock behavior are keyboard-specific. `Fn` is not a Seaside binding modifier, and ordinary F10/F11/F12 events do not gain volume actions by default. All three defaults can be remapped or disabled through `[keybinds]`. No default microphone-mute key, play/pause binding or volume-display UI is approved.

**ENGINEERING default command values:** Use 5-percentage-point volume steps and cap increases at 100%, as spelled out in the editable command strings in §9. Mute toggles the default playback device. This supplies concrete behavior for the approved controls without a new `[audio]` table or native volume action. The relevant media key names are standard [XKB symbols](https://raw.githubusercontent.com/xkbcommon/libxkbcommon/master/include/xkbcommon/xkbcommon-keysyms.h).

**OPEN engineering details:** The external path and exact desktop playback mix for monitor shares and normal playback/microphone hardware qualification remain to be established. The audio package baseline is in §3.1, and default playback bindings are now approved. A working video stream does not establish working audio capture or app-specific isolation. Microphone capture remains a separate source; do not automatically mix it into shared playback audio. Isolated application sharing is explicitly deferred, rather than an unresolved v1 release requirement.

**Audio process boundary:** Keep playback, microphone streams, device routing and volume policy in the existing OS audio services. PipeWire with WirePlumber is the engineering baseline; `pipewire-pulse` supplies PulseAudio compatibility for applications that need it ([PipeWire integration](https://github.com/PipeWire/pipewire/blob/master/INSTALL.md)). External `wpctl` implements the approved default playback shortcuts and user-authored audio commands through the existing generic `exec` action ([WirePlumber tool reference](https://pipewire.pages.freedesktop.org/wireplumber/man/wpctl.html)). These roles do not require a Seaside audio engine. No new audio IPC, mixer UI, audio configuration table or additional mandatory startup check is approved by this boundary.

**Target and failure contract for future isolation:** The external capture/routing implementation must establish which audio streams belong to the selected application. Window identity or an app-name string alone is not proof of that association. If isolated capture is unavailable, report the limitation through the external application's supported path; never silently substitute desktop-wide audio for a selected-app share. This no-substitution rule also applies when documenting v1's lack of isolated application audio. Do not advertise untested OBS/Discord audio-sharing compatibility. No custom fallback UI, routing daemon, audio IPC or audio configuration table is approved.

**Feasibility review — 2026-11-19 (research, not implementation proof):** PipeWire exposes application/process metadata and separate media nodes ([upstream keys](https://raw.githubusercontent.com/PipeWire/pipewire/master/src/pipewire/keys.h)) and can capture sink monitor ports or route streams through virtual sources/sinks ([loopback documentation](https://docs.pipewire.org/page_module_loopback.html)). These mechanisms make selected-application capture and desktop-mix capture possible with suitable external routing. They do not establish a universal one-to-one mapping between a Wayland window and its audio: multiple windows or subprocesses can belong to one application, and per-window attribution needs application/client support or a verified association. A monitor is a video source; its selection does not by itself define which audio output devices are included.

**Current external-client limit:** In its **2026-11-12** [Linux engineering update](https://discord.com/blog/discord-linux-a-year-of-upgrades), Discord describes Wayland per-app audio sharing through desktop portals as work in progress, with desktop-wide audio or no audio as the current choice. Do not promise automatic selected-app audio in Discord from compositor capture support alone. Qualify exact installed client/backend versions and any external routing workflow. No custom audio service, portal implementation, hidden desktop-audio fallback or additional mandatory dependency is authorized by this research. Application-level granularity is **APPROVED**, and its implementation is **DEFERRED beyond v1** by owner decision (G15).

**Desktop-audio qualification:** A monitor share includes the defined desktop playback mix. Check microphone separation, audio-device changes, capture cancellation and audio/video synchronization using real receiving clients/recordings; record exact external package versions. This qualification is separate from deferred application-audio isolation.

**Future application-audio acceptance cases — not v1 release gates:** Play distinguishable sound in the selected application and an unrelated application simultaneously. A window share includes the selected application and excludes the unrelated one. Exercise multiple windows of the same application, native/satellite applications, stream creation/destruction, capture cancellation and audio-device changes. Check microphone separation, target-loss handling and audio/video synchronization. Run these when the external path is available and the deferred work is scheduled; do not claim them passed from video-only tests.

## 8. Configuration system: normative semantics

**Canonical file:** `~/.config/seaside/config.toml`. Seaside owns this file; Conch never does. The internal last-known-good snapshot under `$XDG_STATE_HOME/seaside/last-good-config.toml` is **recovery state**, not another user-editable configuration. The TOML layout in §9 is the **v1 schema contract**; only the specifically marked open defaults await owner confirmation. This section fixes *syntax for already-approved capabilities*, not additional features.

### 8.1 Loading, validation, reload and recovery

1. **Missing config:** On first TTY launch, atomically create the fully documented §9 template with all actual v1 keys, supported enum values, ranges, default values, and actions. Never overwrite a file that appeared concurrently. If generation fails, log it and run with compiled defaults.
2. **Present, valid config:** Parse strict UTF-8 TOML 1.0, rejecting unknown keys, bad types, duplicate entries, conflicting keybindings and invalid ranges. Fill unspecified settings from compiled defaults, not from a prior user config, with the binding-map replacement rule in §8.3: a present `[keybinds]` table defines the complete active binding map. Read all file contents before parsing; do not accept partly written files.
3. **Valid live edit:** Parse/validate into a *new* config object, check affected available outputs, apply all live changes atomically or reject the candidate without changing any active live setting. Preserve runtime night-light enable state when its configured enable value is unchanged (§6.3). Update the last-known-good snapshot atomically after a successful commit; clear the top-edge error banner. The snapshot stores the validated requested TOML configuration, not transient shortcut/IPC night-light state.
4. **Invalid live edit:** Keep all active settings and the previous snapshot. Display the **persistent nondismissible** compositor-owned banner with the path, line if known, failing field and error. Expose the same diagnostic to stderr/log/IPC; don't automatically repair or overwrite user TOML.
5. **Invalid at startup:** Load the last-known-good snapshot if present and valid, otherwise compiled defaults. Keep the user config unchanged and show the persistent banner until a valid edit applies.
6. **Restart-only setting:** `workspaces.mode` is the *sole* approved restart-only key. A valid live edit may have a **requested** mode different from the **active** mode; all other valid values apply. Persist the entire validated requested configuration in the snapshot, expose pending restart, and don't change workspace ownership until a new Seaside process starts. At next launch the requested mode becomes active.
7. **Autostart:** `autostart.commands` executes only once after the strict graphical-session preflight at launch; a reload never starts or stops those commands. All other exposed settings are live unless a future owner-approved exception is recorded.
8. **Override reset:** Only a *committed change* to `window.tiling` or `window.layout` clears temporary per-window tiling overrides across all workspaces and rebuilds the active layout as already specified. Invalid reloads, monitor updates, and unrelated settings don't reset them.

**ENGINEERING CHOICES:** Watch the user file and its containing directory for editor atomic-save/rename patterns; debounce partial writes and recheck the replacement path. Limit configuration size to 1 MiB and reject unsafe/nonfinite numeric input. Save snapshots with a temporary file, `fsync`, rename and restrictive permissions; don't block the display event loop on disk operations. No extra user-editable config, no automatic recovery UI beyond the already approved error banner.

**Transaction and persistence details (ENGINEERING CHOICE):** Decode a changed wallpaper and validate affected output states before committing the logical configuration. Use a generation number so an older parse/decode/write job cannot overwrite a newer accepted candidate. Atomic reload refers to the compositor's logical configuration; do not claim one physical atomic commit across independent DRM devices. Pre-test output changes, retain rollback state, and report actual output state if an unavoidable hardware failure prevents rollback, using the existing output failure/migration rules. A failed snapshot write after a valid live commit keeps the active settings and the previous durable snapshot, reports the persistence failure through logs/status, and does not overwrite the valid user file or call it a syntax error. A new process uses the newest successfully persisted recovery snapshot only when normal user-config loading fails. If `XDG_STATE_HOME` is unset, use the standard `~/.local/state` base; an absent or unsafe `XDG_RUNTIME_DIR` is a startup error, not permission to place session sockets in a shared directory.

### 8.2 Full settings registry and validation

**Unit convention (ENGINEERING CHOICE; owner review if physical-pixel interpretation is important):** Geometry gaps, borders, manual monitor positions, and keyboard resize distances use **logical pixels**; at scale `1.0` these match physical pixels. At scale `1.25`, 10 logical px maps to about 12.5 physical px, rounded consistently at rasterization. Floating geometry and per-output work areas use the same logical coordinate space. Refresh rates use **Hz**, not game FPS. This is a necessary fractional-scaling convention, not a new setting.

| TOML key | Accepted values (exact v1 parser) | Default | Apply |
|---|---|---|---|
| `autostart.commands` | array of 0–128 nonempty shell-command strings; individual command max 4096 UTF-8 bytes | `[]` | startup only |
| `wallpaper.path` | absolute path to a readable/decodable local image, max 4096 bytes | `/usr/share/seaside/wallpapers/black.png` | live |
| `window.tiling` | boolean | `true` | live + override reset if changed |
| `window.layout` | `"dwindle"`, `"scrolling"` | `"dwindle"` | live + rebuild/reset if changed |
| `window.gaps_outer` | integer 0–512 logical px | `10` | live |
| `window.gaps_inner` | integer 0–512 logical px | `5` | live |
| `window.border_width` | integer 0–64 logical px | `2` | live |
| `workspaces.mode` | `"global"`, `"per-monitor"` | `"global"` | **restart only** |
| `monitors.primary` | `"auto"` or output selector, defined below | `"auto"` | live |
| `monitors.resolution` | `"auto"` or `"WIDTHxHEIGHT"`, positive integers | `"auto"` | live |
| `monitors.refresh_rate` | `"auto"` or positive finite number in Hz | `"auto"` | live |
| `monitors.scale` | positive finite number from 0.5–4.0 | `1.0` | live |
| `monitors.rotation` | integer `0`, `90`, `180`, `270` | `0` | live |
| `monitors.position` | `"auto"` or `[x, y]` signed logical integer coordinates | `"auto"` | live |
| `[[monitors.output]].match` | exact output selector, required in each override entry | none by default | live |
| `[[monitors.output]].resolution` | same as `monitors.resolution` | inherits global | live |
| `[[monitors.output]].refresh_rate` | same as `monitors.refresh_rate` | inherits global | live |
| `[[monitors.output]].scale` | same as `monitors.scale` | inherits global | live |
| `[[monitors.output]].rotation` | same as `monitors.rotation` | inherits global | live |
| `[[monitors.output]].position` | same as `monitors.position` | inherits global | live |
| `dwindle.insertion` | `"focused"`, `"largest"` | `"focused"` | live |
| `dwindle.split_ratio` | finite fraction 0.05–0.95 | `0.5` | live |
| `scrolling.default_column_width` | finite fraction 0.10–4.0 of usable output logical width; a client's minimum can override | `0.5` | live |
| `input.mouse.profile` | `"flat"` **only**; there is no other mouse-acceleration mode in v1 | `"flat"` | live |
| `input.mouse.sensitivity` | finite libinput speed from -1.0 to 1.0 | `0.0` | live |
| `idle.display_off_after_minutes` | integer 0–1440; `0` disables idle display power-off | `15` | live |
| `render.direct_scanout` | boolean (attempt whenever eligible, correct composition fallback) | `true` | live |
| `render.adaptive_sync` | `"off"`, `"fullscreen"`, `"always"` | `"off"` | live |
| `render.allow_tearing` | boolean; only client-requested eligible fullscreen presentation | `false` | live |
| `render.fullscreen_aspect` | `"preserve"`, `"stretch"` | `"preserve"` | live |
| `night_light.enabled` | boolean; global hardware colour control, no GPU fallback | `false` | live when configured value changes; unrelated edits preserve runtime override (§6.3) |
| `night_light.temperature_kelvin` | integer warmth target; **4500 K approved default**; engineering validation range 1000–10000 (pending API qualification) | `4500` | live |
| `[keybinds]` entries | normalized chord/gesture -> exactly one approved action string per §8.3 | only confirmed defaults | live |

**Important exclusions:** No `wallpaper.fit` selector in v1: use a fixed, aspect-preserving **cover** fit with cropping as needed; this is a rendering implementation of the approved changeable wallpaper, not a new option. No `render.explicit_sync` switch: explicit synchronization is always supported when applicable and correctly falls back for incompatible clients. No cursor-disable key, rounded corners, animation switches, separate focus modes, window rules, HDR, shell styling, power controls, night-light schedule or per-monitor night light.

**Capability validation:** Syntax/type errors reject the entire TOML reload. For a **connected** display, an explicit unsupported resolution/refresh pair rejects a new configuration and keeps the prior mode. An override referring to a currently disconnected display is valid but dormant until that display is available. A valid feature setting on hardware that doesn't support it (e.g. VRR or night light) is not a syntax error: preserve output operation and report the actual unsupported capability through status/events. Mandatory hardware cursor failure instead disables that output per §5.2.

#### Monitor selector and override contract (ENGINEERING CHOICE)

- Output selectors are strings of either `"identity:<opaque>"` (stable physical identifier, where sufficiently unique) or `"connector:<output-name>"` (e.g. `connector:DP-1`). The exact current values are **returned by `seasidectl outputs`**; users should copy those rather than construct them by hand. No wildcards, regex or implicit case-folding.
- The `identity` value is derived from available physical metadata and reconnection history. It is **not guaranteed unique if monitors supply identical/missing identifiers**; Seaside best-effort disambiguates physical outputs using connectors and history as previously approved. Ambiguous configured selectors report a useful error rather than applying the same override to two outputs. Workspace identity must never be conflated with monitor identity.
- `[monitors]` supplies defaults. Each `[[monitors.output]]` overrides **only** fields present for its matched output; unspecified fields inherit from `[monitors]`. Duplicate overrides resolving to the same live physical output are invalid. The `primary` setting uses the same selector grammar or `auto`.
- The `auto` mode selection policy stays as already approved: preferred/native resolution, **highest** supported refresh rate at that resolution. If an explicit resolution has `refresh_rate="auto"`, use the highest rate at that resolution. If `resolution="auto"` and refresh is explicit, choose the preferred resolution if it supports the requested rate; otherwise return a clear unsupported-mode error rather than silently reducing resolution.
- Manual position is `[x, y]`, the logical coordinate of the output's top-left after scale and rotation. Automatic positions arrange outputs from the primary left-to-right, after any explicitly positioned output rectangles are accounted for. The detailed placement tie-break and overlap diagnostics are deterministic layout-engine details, not new user settings.

### 8.3 Binding grammar and action catalog (ENGINEERING CHOICE)

Every Seaside compositor key/mouse gesture is configurable in the **same `[keybinds]` table**. No hidden bindings or keyboard-shortcut inhibit mode. Each key is quoted in TOML; one value is one action. Supported grammar:

```text
Keyboard chord:    [Modifier+...]XKB_KEYSYM
Mouse gesture:     Modifier+MouseLeftDrag | Modifier+MouseRightDrag
Modifiers:         Super, Ctrl, Alt, Shift (zero or more; any order; each at most once)
Examples:          Super+D, Super+Left, Alt+Tab, Super+Ctrl+Left, XF86AudioMute
Action:            command_name [space required_argument ...]
External command:  exec <nonempty shell command string>
```

The parser normalizes modifier ordering and keysym case only where XKB explicitly defines equivalence. Keys are named using XKB keysym names (e.g. `D`, `Tab`, `Return`, `Left`, `F12`); no new keyboard-layout or shortcut editor UI. Modifiers are not inferred from keysyms. Missing/unknown actions, duplicate modifier tokens, a missing primary key, two bindings normalizing to the same chord, or pairing a pointer-only action with a keyboard binding are config errors. On a successful live reload, replace the binding map atomically.

**APPROVED binding removal:** When `[keybinds]` is present, its entries are the complete active binding map, replacing rather than merging with compiled defaults or the previous map. Deleting or commenting out a binding disables that shortcut after a successful reload, including a shipped default. A present but empty `[keybinds]` table disables all compositor bindings. If the entire table is absent, the ordinary omitted-setting rule supplies the compiled default binding map; keep an empty table to explicitly request no bindings. Invalid edits keep the previous active map per §8.1. First-use generation still writes all confirmed default bindings explicitly.

**Approved actions (same action dispatcher as IPC; names are engineering interface spelling):**

| Action value in TOML | Input/context | Effect / restrictions |
|---|---|---|
| `toggle_window_tiling` | key; focused ordinary window | temporary tiling override |
| `focus left\|right\|up\|down` | key | spatial focus with within-workspace wrapping |
| `cycle_focus_mru` | key | current workspace Alt+Tab-style MRU cycle |
| `move_window` | configured mouse left drag | floating move, or tiled detach/reinsert |
| `resize_window` | configured mouse right drag | floating resize, or tiled split/column resize |
| `move_window DIR` | key | directionally move/reorder currently focused window |
| `swap_window DIR` | key | swap focused tiled window with eligible neighbor |
| `resize_window DIR [DISTANCE]` | key | resize toward specified direction, with optional positive integer logical-pixel step; default step **engineering value 32** |
| `workspace ID` | key | select/create globally numbered workspace ID |
| `move_to_workspace ID` | key | move focused ordinary window, follow destination workspace/output, and focus moved window (§5.1) |
| `transfer_workspace SELECTOR` | key | explicitly transfer focused workspace to selected output |
| `toggle_window_pin` | key | sticky across workspaces on current output; temporarily floats tiled window |
| `toggle_window_maximize` | key | standard maximization, restore prior state |
| `toggle_window_fullscreen` | key | fullscreen/restore prior state |
| `close_window` | key | request focused client to close, not force-kill it |
| `night_light on\|off\|toggle` | key | set global manual night-light mode |
| `exit` | key | stop compositor, return to launching TTY |
| `exec SHELL_COMMAND` | key | launch configured external command using `/bin/sh -c`, no IPC counterpart |

`DIR` means `left`, `right`, `up`, or `down`; `ID` means a positive integer workspace ID; `SELECTOR` is the exact current monitor selector from §8.2. When a command has no eligible target, it is a no-op for a keybinding and a machine-readable error for an IPC call. Floating-only or tiled-only operations follow the already-approved window rules; don't silently introduce minimization, always-on-top or new movement modes.

**Confirmed default bindings:** `Super+D` tiling toggle; `Super+Left/Right/Up/Down` directional focus; `Alt+Tab` current-workspace MRU; `Super+MouseLeftDrag` move/detach-reinsert; `Super+MouseRightDrag` resize; **`Super+Return` = `exec foot`** (approved default terminal); **`XF86AudioLowerVolume`, `XF86AudioRaiseVolume`, `XF86AudioMute`** = the external `wpctl` commands in §9 (approved playback controls). Keyboard move/swap/resize, pinning, maximize/fullscreen, other external-command bindings, workspace selection/move, night-light, close and exit remain **unbound by default unless the owner separately approves a specific default chord**. The template may contain commented examples but must not silently activate them. Users can remove or rebind every default.

**`exec` and autostart:** Treat the command suffix and autostart array elements as user-authored shell commands; execute asynchronously via `/bin/sh -c`. There is no `eval`/script interpreter inside Seaside. Do not offer `exec` through IPC, and do not run an `exec` string supplied by an untrusted Wayland client. Log spawn failures without blocking input or output presentation.

## 9. Canonical TOML template — v1 configuration surface

This is the **one** TOML file generated on first run. Every *approved, exposed* setting has a default and its valid values/range documented inline. Entries under `[[monitors.output]]` and optional keybindings are intentionally commented until a user chooses a selector or chord. **Night-light warmth is approved at 4500 K by default**, with night light itself disabled until manually enabled. Bounds in §8.2 are defensive engineering validation limits and require confirmation against the target implementation.

```toml
# Seaside v1 — ~/.config/seaside/config.toml
# Every setting is documented here. Edit and save to live-reload unless noted.
# Invalid edits preserve the last valid config and display a persistent error.

[autostart]
# List of shell commands; [] = start nothing (default). Commands run ONCE
# after session preflight. Changing this list needs a Seaside restart.
commands = []

[wallpaper]
# Absolute path to an image. Default is the bundled solid-black image.
# Uses fixed aspect-preserving cover/crop; no wallpaper fit option in v1.
path = "/usr/share/seaside/wallpapers/black.png"

[window]
# true = globally tile ordinary windows; false = globally float.
# Changing tiling or layout clears temporary per-window tiling overrides.
tiling = true
# Layout options: "dwindle" (default), "scrolling".
layout = "dwindle"
# User-configurable defaults in logical pixels (see spec for scale behavior):
# outer/inner 0..512; border width 0..64. Borders are square.
gaps_outer = 10
gaps_inner = 5
border_width = 2

[workspaces]
# "global" (default) or "per-monitor". THIS KEY NEEDS A RESTART.
mode = "global"

[monitors]
# Primary: "auto" (highest supported Hz, then resolution, then connector)
# or a selector: "identity:<as-reported>" / "connector:<output-name>".
# Use `seasidectl outputs` to discover valid monitor selectors.
primary = "auto"
# Mode: "auto" = preferred/native resolution + highest Hz at that res.
# Resolution override: "WIDTHxHEIGHT", e.g. "2560x1440".
# Refresh rate override: positive Hz number, e.g. 165.0 or 59.94.
resolution = "auto"
refresh_rate = "auto"
# Fractional scale: finite number 0.5..4.0; 1.0 means 100% (default).
scale = 1.0
# Degrees clockwise: 0 (default), 90, 180, 270.
rotation = 0
# "auto" = auto horizontal layout, or [x, y] logical coords, e.g. [0,0].
position = "auto"

# Optional per-monitor overrides. Repeat this [[monitors.output]] table.
# Exact match options: identity:<as-reported> or connector:<output-name>.
# Omitted fields inherit the corresponding [monitors] defaults.
# [[monitors.output]]
# match = "connector:DP-1"
# resolution = "2560x1440"  # "auto" or WIDTHxHEIGHT
# refresh_rate = 165.0       # "auto" or positive Hz number
# scale = 1.25               # 0.5..4.0, fractions allowed
# rotation = 90              # 0, 90, 180, 270
# position = [1920, 0]       # "auto" or [x, y] logical px

[dwindle]
# New-window target: "focused" (default) or "largest".
insertion = "focused"
# Initial new split allocation ratio: finite 0.05..0.95.
# Existing split orientations recalculate automatically; new side right/bottom.
split_ratio = 0.5

[scrolling]
# New column width as fraction of usable output logical width: 0.10..4.0.
# 0.5 = 50% by default. Per-window minimum widths still take precedence.
default_column_width = 0.5

[input.mouse]
# Pointer profile: "flat" only in v1 (no pointer acceleration).
profile = "flat"
# libinput normalized speed: -1.0..1.0; 0.0 = neutral default.
sensitivity = 0.0

[idle]
# Minutes with no activity before display power-off: 0..1440; 0 disables.
# Visible fullscreen apps and client idle inhibition prevent display-off.
display_off_after_minutes = 15

[render]
# true = attempt direct scanout for eligible frames; false = compose.
# Explicit synchronization is automatically supported as appropriate;
# it is NOT user-configurable. Hardware cursors are mandatory, no option.
direct_scanout = true
# "off" (default), "fullscreen", "always".
adaptive_sync = "off"
# false (default) or true: allow client-requested tearing in fullscreen only.
allow_tearing = false
# "preserve" (default) for aspect ratio / bars, or "stretch" to fill.
fullscreen_aspect = "preserve"

[night_light]
# false (default) or true; manual global hardware-only night light.
# No schedule, per-monitor setting or GPU post-processing fallback.
# Shortcut/IPC changes survive unrelated config edits for this session.
# Changing this enabled value applies it; a new session uses the config value.
enabled = false
# Kelvin 1000..10000 (engineering validation range); default 4500 K APPROVED.
# Disabled by default, so this has no effect until explicitly enabled.
temperature_kelvin = 4500

[keybinds]
# A binding is "Modifier+XKB_Key" = "action [args]"; modifiers are optional.
# Modifiers: Super, Ctrl, Alt, Shift. Examples: Left, Tab, D, Return, F12.
# Drag gestures: Super+MouseLeftDrag and Super+MouseRightDrag.
# All default bindings can be removed or remapped.
# This table is the complete active map: delete/comment an entry to disable it.
# An empty [keybinds] table disables all bindings; omitting the table uses defaults.
"Super+D" = "toggle_window_tiling"
"Super+Left" = "focus left"
"Super+Right" = "focus right"
"Super+Up" = "focus up"
"Super+Down" = "focus down"
"Alt+Tab" = "cycle_focus_mru"
"Super+MouseLeftDrag" = "move_window"
"Super+MouseRightDrag" = "resize_window"
"Super+Return" = "exec foot"  # Default terminal; installed through Seaside package dependencies.
# Standard media events, including those produced by Fn on supported keyboards.
# Fn/Fn-Lock and physical key positions are keyboard-specific; Fn is not a modifier here.
# External wpctl controls the current default playback device; 5% steps, max 100%.
"XF86AudioLowerVolume" = "exec wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"
"XF86AudioRaiseVolume" = "exec wpctl set-volume --limit 1 @DEFAULT_AUDIO_SINK@ 5%+"
"XF86AudioMute" = "exec wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"

# ALL OTHER APPROVED ACTIONS ARE UNBOUND BY DEFAULT.
# Available action forms (choose your own chords; no hidden defaults):
#   toggle_window_tiling; focus DIR; cycle_focus_mru
#   move_window (mouse drag only); resize_window (mouse drag only)
#   move_window DIR; swap_window DIR; resize_window DIR [DISTANCE]
#   workspace ID; move_to_workspace ID; transfer_workspace SELECTOR
# Moving a window to another workspace follows it and focuses that window.
#   toggle_window_pin; toggle_window_maximize; toggle_window_fullscreen
#   close_window; night_light on; night_light off; night_light toggle
#   exit; exec SHELL_COMMAND
# DIR = left/right/up/down; ID = positive integer, globally unique.
# SELECTOR = identity:<as-reported> or connector:<output-name>.
# DISTANCE = optional positive logical pixels, default 32.
# Optional examples (uncomment ONLY those you want):
# "Super+Q" = "close_window"
# "Super+1" = "workspace 1"
# "Super+Shift+1" = "move_to_workspace 1"
# "Super+Alt+1" = "transfer_workspace connector:DP-1"
# "Super+Shift+Left" = "move_window left"
# "Super+Alt+Left" = "swap_window left"
# "Super+Ctrl+Left" = "resize_window left 32"
# "Super+P" = "toggle_window_pin"
# "Super+M" = "toggle_window_maximize"
# "Super+F" = "toggle_window_fullscreen"
# "Super+N" = "night_light toggle"
# "Super+Shift+E" = "exit"
# System actions are EXTERNAL commands, e.g.:
# "Super+Shift+R" = "exec systemctl reboot"
# No dedicated power command, confirmation UI or automatic boot into Seaside.
```

**Template acceptance contract:** A real TOML parser must parse the full generated template; the parsed effective values must exactly match the compiled default instance. Generator, validation logic and reference docs should share one internal definition to prevent drift. The template's comments must cover *every accepted enum value and numeric range* and link free-form XKB keysyms to their standard naming rules. Unknown keys produce an actionable error rather than being ignored. A physical monitor's selector string is intentionally copied from runtime discovery, not guessed from machine-specific data at build time.

**Binding-map acceptance cases:** With a present `[keybinds]` table, delete or comment out `Super+Return` and verify that the terminal shortcut is absent after a valid reload and subsequent startup. Remapping it must activate only the new chord. An empty table yields zero bindings; an absent table yields compiled defaults. An invalid candidate preserves the previous map. These cases must also hold when a last-known-good snapshot is loaded.

## 10. IPC, CLI, command and event protocol — v1

### 10.1 Transport and security (ENGINEERING CHOICE)

- The IPC server is **inside Seaside**, not a daemon or network service. Use an `AF_UNIX` **stream** socket under `$XDG_RUNTIME_DIR/seaside/`, directory mode `0700`, socket mode `0600`, and enforce the connecting peer's user ID via `SO_PEERCRED`. Socket name contains the runtime Wayland socket name, e.g. `.../seaside/wayland-0.sock`, to avoid collisions when multiple sessions exist. Export `SEASIDE_SOCKET` into the Seaside session environment so `seasidectl` and Conch know the correct instance. With no variable, a CLI may derive the path from `WAYLAND_DISPLAY` but must not guess another user's session.
- Use UTF-8 **newline-delimited JSON** (`JSONL`), one JSON object per line. An input request has a hard limit of **64 KiB** including the newline; output responses/events are capped to **1 MiB** each. Invalid JSON, excessive size, unsupported protocol version, duplicate JSON object fields, or unknown methods must return a structured error (and close the connection if framing cannot be recovered). No unbounded JSON parsing or recursion.
- Version field is integer `1`. Each request has an unsigned integer `id` echoed in its response, a dotted `method` name and a `params` object (required even when empty). Responses are correlated by `id`; event subscriptions send unsolicited records with `event` and global monotonic `seq`. Every response also includes `seq`, the event watermark at the point its result was captured. The protocol version is not the compositor package version. Do not accept methods outside the explicit allowlist below.
- Processing, event distribution and client reads/writes are nonblocking. Each connection is bounded to **256 queued records or 1 MiB**, whichever comes first, and at most **32 in-flight requests**. Disconnect slow readers; they can resubscribe and re-query state. No replay of missed events; bounded trace export uses a separate file path rather than trying to fit all frames into one JSON response.
- Window IDs, output IDs and workspace IDs in IPC are identifiers, **not memory addresses or arbitrary client-controlled pointers**. Window IDs are unique within a Seaside instance lifetime and never intentionally reused in the same lifetime; all go stale when the server exits. All client titles/app IDs are untrusted, UTF-8 bounded and escaped.

### 10.2 JSONL envelope, response and event examples

```json
{"v":1,"id":10,"method":"query.outputs","params":{}}
```

Successful response (illustrative output facts, not actual measured hardware values):

```json
{"v":1,"id":10,"seq":41,"ok":true,"result":{"outputs":[{"id":"out-1","selector":"connector:DP-1","identity_selector":null,"connector":"DP-1","connected":true,"enabled":true,"focused":true,"disabled_reason":null,"position":{"x":0,"y":0},"scale":1.0,"rotation":0,"mode":{"width":2560,"height":1440,"refresh_millihz":240000},"hardware_cursor":true,"adaptive_sync":{"supported":null,"applied":false,"reason":null},"tearing":{"supported":null,"applied":false,"reason":null},"night_light":{"supported":null,"applied":false,"reason":null}}]}}
```

Error response:

```json
{"v":1,"id":10,"seq":41,"ok":false,"error":{"code":"invalid_args","message":"Expected a positive workspace ID"}}
```

Subscription request and notification (on a subscribed connection):

```json
{"v":1,"id":11,"method":"events.subscribe","params":{}}
{"v":1,"id":11,"seq":41,"ok":true,"result":{"subscribed":true}}
{"v":1,"event":"window.mapped","seq":42,"data":{"window_id":17,"workspace_id":3,"app_id":"example.app","title":"Example"}}
```

The line above is a **sequence of three separate JSONL messages**, not one JSON document. `window.attention` represents an observed request for attention (e.g. denied focus activation), **not** a new notification subsystem or an authorization for the compositor to focus it.

### 10.3 Query and action allowlist (same state machine as keybindings)

Every mutating operation executes the exact already-approved compositor action; it must not be a second implementation with different focus/tiling semantics. Client-provided window ID is optional for most window methods, defaulting to the focused eligible window; a missing focused target returns `no_target` instead of silently acting on the wrong window.

| JSON `method` | Parameters | Return / effect |
|---|---|---|
| `query.status` | `{}` | readiness and active/recovered config status; last error and pending restart |
| `query.outputs` | `{}` | connected/disabled outputs, selectors, applied modes, cursor, VRR/tearing and color status |
| `query.workspaces` | `{}` | IDs, ownership, output association, visibility and focus |
| `query.windows` | `{}` | ephemeral window ID, workspace, app ID/title, mapped/floating/fullscreen/maximized/pinned flags |
| `query.focus` | `{}` | currently focused output/workspace/window IDs if any |
| `query.config` | `{}` | active vs requested restart-only mode, TOML parse errors, last-good recovery status |
| `query.diagnostics` | `{}` | per-output frame/presentation and direct-scanout results **where measurable** |
| `query.night_light` | `{}` | configured enable value, effective global runtime enable state, temperature, and actual per-output support/applied state |
| `action.focus` | `{"direction":"left"}` or other direction | focus neighbor in same workspace; wrap at workspace edge |
| `action.cycle_focus_mru` | `{}` | MRU window on current workspace |
| `action.workspace_select` | `{"workspace_id":3}` | select/create workspace by approved global/per-monitor rules |
| `action.window_to_workspace` | `{"workspace_id":3,"window_id":17}` | relocate eligible window, select destination, and focus moved window (§5.1); target defaults to focused if ID omitted |
| `action.workspace_transfer` | `{"selector":"connector:DP-1","workspace_id":3}` | transfer workspace to output; ID defaults to focused workspace |
| `action.window_toggle_tiling` | optional `{"window_id":17}` | temporary tile/float override |
| `action.window_toggle_pin` | optional `{"window_id":17}` | sticky; floats when formerly tiled |
| `action.window_toggle_maximize` | optional `{"window_id":17}` | standard maximize/restore |
| `action.window_toggle_fullscreen` | optional `{"window_id":17}` | fullscreen/restore |
| `action.window_close` | optional `{"window_id":17}` | ask focused client to close |
| `action.window_move` | `{"direction":"left","window_id":17}` | approved keyboard directional reorder/move |
| `action.window_swap` | `{"direction":"left","window_id":17}` | approved keyboard swap with neighboring tile |
| `action.window_resize` | `{"direction":"right","delta":32,"window_id":17}` | keyboard resize; positive logical-pixel delta, default 32 |
| `action.night_light_set` | `{"enabled":true}` | set manual global night light; report per-output capability |
| `action.exit` | `{}` | clean exit to logged-in TTY; no power action |
| `trace.start` | `{}` | start explicitly requested bounded, temporary trace |
| `trace.stop` | `{}` | stop trace |
| `trace.export` | `{}` | asynchronously export captured trace into private state dir; return generated path |
| `events.subscribe` | `{}` | events on same open connection, until disconnect |

**Explicitly forbidden via IPC:** arbitrary shell `exec`, custom power/reboot/suspend commands, application-specific rules, synthetic input/remote-desktop injection, direct arbitrary geometry modifications, and dynamic code evaluation. `action.exit` is not a host power action: it just closes this Seaside instance. A shell that wants to execute `systemctl reboot` does so separately under normal system permissions.

### 10.4 Event names and payload guarantees

| Event | Minimum data | When generated |
|---|---|---|
| `window.mapped` | `window_id`, `workspace_id` | new ordinary toplevel becomes mapped |
| `window.unmapped` | `window_id`, `workspace_id` | mapped toplevel becomes unmapped |
| `window.destroyed` | `window_id` | Wayland toplevel permanently destroyed |
| `window.focused` | `window_id` (nullable), `workspace_id`, `output_id` | keyboard focus changes |
| `window.attention` | `window_id`, `workspace_id` | application on hidden/unfocused workspace requests attention; **never** steals focus |
| `workspace.changed` | `workspace_id`, `output_id`, `visible` | workspace display/assignment changes |
| `output.changed` | `output_id`, `connected`, `enabled` | display hotplug/disable/config changes |
| `config.error` | `path`, `message`, `line` (nullable) | invalid config detected |
| `config.recovered` | `source` | valid user config successfully restored/applied |
| `integration.failed` | `name`, `message` | required external integration fails **after** startup |
| `integration.recovered` | `name` | recovered external integration becomes available |

**Event semantics and startup ordering:** Notifications reflect committed state, not speculative layout calculations. Subscribe **first**, receive the subscription acknowledgement, then query current state on the same connection. The acknowledgement is queued before subsequent notifications for that subscription. Buffer interleaved events while awaiting a query; its response's `seq` is the snapshot watermark, so discard buffered events at or below it for that queried view, then handle newer events. This closes the query-before-subscribe gap without replay or a new snapshot method. Queries are individually coherent; multiple queries are not one atomic combined snapshot. A connection loss or queue overflow requires subscribing and querying again. Events remain advisory invalidations for the states they describe, not a complete replication log of every metadata property. With no subscribers, keep only the sequence counter, not unbounded event history. All other events require explicit approval, unless indispensable to express an already-approved state query.

### 10.5 Error codes, CLI and tracing behavior

**Error codes:** `invalid_request`, `invalid_args`, `unsupported_version`, `unknown_method`, `not_ready`, `no_target`, `not_found`, `invalid_state`, `ambiguous_output`, `unsupported_capability`, `permission_denied`, `trace_not_running`, `trace_already_running`, `response_too_large`, `internal_error`. Responses have `ok:false` and `error.code` plus an actionable `error.message`. Pending restart is **reported in `query.config`**, not an error that rolls back otherwise valid live settings.

The `seasidectl` CLI is a thin client for exactly these same methods. Its v1 command mapping is an engineering interface spelling, not another server-side API:

```text
seasidectl status                  -> query.status
seasidectl outputs                 -> query.outputs
seasidectl workspaces              -> query.workspaces
seasidectl windows                  -> query.windows
seasidectl focus                    -> query.focus
seasidectl focus left               -> action.focus
seasidectl workspace 3             -> action.workspace_select
seasidectl workspace transfer 3 connector:DP-1 -> action.workspace_transfer
seasidectl window toggle-tiling    -> action.window_toggle_tiling
seasidectl window toggle-pin       -> action.window_toggle_pin
seasidectl window toggle-maximize  -> action.window_toggle_maximize
seasidectl window toggle-fullscreen -> action.window_toggle_fullscreen
seasidectl window close            -> action.window_close
seasidectl window move left        -> action.window_move
seasidectl window swap left        -> action.window_swap
seasidectl window resize right 32  -> action.window_resize
seasidectl window to-workspace 3   -> action.window_to_workspace
seasidectl night-light status      -> query.night_light
seasidectl night-light on          -> action.night_light_set
seasidectl night-light off         -> action.night_light_set
seasidectl debug render            -> query.diagnostics
seasidectl trace start|stop|export -> trace methods
seasidectl events                  -> events.subscribe
seasidectl exit                    -> action.exit
```

**Tracing:** Only `trace.start` enables detailed per-frame capture. The exporter writes a **bounded** JSONL trace file under `$XDG_STATE_HOME/seaside/traces/` (directory `0700`, file `0600`) asynchronously. The engineering cap is **60 seconds or 20,000 entries or 16 MiB**, whichever first; automatically stop at a cap and report it in diagnostics. `trace.start` while running returns `trace_already_running`; otherwise it begins a new bounded session, replacing any previous in-memory completed trace. `trace.stop` without a running session returns `trace_not_running`; stopping preserves the recorded samples. Export requires a completed trace (`invalid_state` if still running, `trace_not_running` if none exists), and returns an actual completed path after successful asynchronous writing. A failed export preserves the samples for retry. Retain at most one in-memory trace; user-requested exported files are not an always-on history. No frames are captured/retained continuously when tracing is off.

**Contract acceptance tests:** Valid queries and edits, stale IDs, malformed JSONL/unknown fields, duplicate request IDs, version mismatch, oversized packets, event subscription ordering, a consumer that stops reading, empty outputs, output disconnection/recovery, attempted IPC `exec`, trace caps, and parsing the CLI's own JSON. None may block presentation or crash Seaside.

### 10.6 Typed result shapes and request validation (ENGINEERING CHOICE)

All fields shown below are required; `?` means the field's value may be JSON `null`, not that its key may be omitted. Numbers must be finite. Request IDs, window IDs, counters and event sequence numbers are integers in `0..9007199254740991`; window IDs start at 1 and are not reused in the instance. Workspace IDs are integers in `1..2147483647`. Output IDs are opaque strings, stable for remembered outputs within an instance, distinct from connector/physical selectors. Client metadata strings are escaped UTF-8, bounded to 4096 bytes per title and 1024 per app ID; truncate only at valid character boundaries for IPC and never use display strings as authority.

```text
Mode = {width: positive integer, height: positive integer, refresh_millihz: positive integer}
Capability = {supported: boolean?, applied: boolean?, reason: string?}
Diagnostic = {component: string, message: string, path: string?, line: positive integer?, field: string?}
ConfigStatus = {path: string, source: "user"|"last_good"|"defaults",
                active_workspace_mode: "global"|"per-monitor",
                requested_workspace_mode: "global"|"per-monitor",
                pending_restart: boolean, recovery_persisted: boolean, error: Diagnostic?}
Output = {id: string, selector: string, identity_selector: string?, connector: string,
          connected: boolean, enabled: boolean, focused: boolean, disabled_reason: string?,
          position: {x: integer, y: integer}, scale: number, rotation: 0|90|180|270,
          mode: Mode?, hardware_cursor: boolean?,
          adaptive_sync: Capability, tearing: Capability, night_light: Capability}
Workspace = {id: workspace ID, owner_output_id: string?, output_id: string?,
             visible: boolean, focused: boolean}
Window = {id: window ID, workspace_id: workspace ID?, app_id: string, title: string,
          mapped: boolean, floating: boolean, fullscreen: boolean,
          maximized: boolean, pinned: boolean}
Focus = {output_id: string?, workspace_id: workspace ID?, window_id: window ID?}
TraceStatus = {state: "empty"|"running"|"stopped", entries: integer, bytes: integer,
               dropped_samples: integer, stop_reason: "requested"|"time"|"entries"|"bytes"?}
OutputDiagnostic = {output_id: string, backend: string?, renderer: string?,
                    frame_state: "idle"|"scheduled"|"waiting_for_present"|"inactive",
                    presentation_mode: "scanout"|"composed"|"unknown",
                    fallback_reasons: array of strings,
                    committed_frames: integer?, presented_frames: integer?,
                    discarded_frames: integer?, last_presentation_ns: decimal string?}
```

An output record's geometry/config values describe its last applied/remembered state; `mode:null` means none is known. Unsupported or unobservable facts use the capability reason/nullable fields, not invented measurements. Workspace `owner_output_id` describes the remembered owner/last assignment, including a disconnected output; `output_id` is the current association, including temporary migration. A hidden workspace can retain that association without being visible. A window may have `workspace_id:null` before initial placement; focus IDs may be null when there is no eligible object. Nanosecond timestamps use decimal strings to preserve precision.

| Method | Exact `result` object |
|---|---|
| `query.status` | `{ready: boolean, phase: "starting"\|"ready"\|"stopping", config: ConfigStatus, last_error: Diagnostic?}` |
| `query.outputs` | `{outputs: array of Output}` |
| `query.workspaces` | `{workspaces: array of Workspace}` |
| `query.windows` | `{windows: array of Window}` |
| `query.focus` | `Focus` |
| `query.config` | `ConfigStatus` |
| `query.diagnostics` | `{outputs: array of OutputDiagnostic, trace: TraceStatus}` |
| `query.night_light` | `{configured_enabled: boolean, enabled: boolean, temperature_kelvin: integer, outputs: array of {output_id: string, supported: boolean?, applied: boolean?, reason: string?}}` |
| `action.*` | `{accepted: true, changed: boolean}`; `accepted` means the action was dispatched, not that a client honored a close request; `changed` reports a committed compositor state change |
| `events.subscribe` | `{subscribed: true}`; a repeated subscription on the same connection is a successful no-op |
| `trace.start`, `trace.stop` | `{trace: TraceStatus}` |
| `trace.export` | `{path: absolute string}`; only return after a successful completed write |

**Envelope/error rules:** Reject unknown request/parameter fields, non-object `params`, invalid numeric types (including booleans as integers), invalid enums and duplicate JSON fields. Maximum JSON nesting depth is 16. A duplicate still-in-flight request ID on the same connection is `invalid_request`; IDs can be reused after their response. For invalid input without a recoverable valid request ID, return `id:null` in the error response, then close if safe framing cannot be recovered. Other responses echo the valid ID. Subscription/query responses include the captured watermark; events keep their strictly increasing `seq`. Error codes remain the §10.5 allowlist. Before readiness, allow queries/subscription for diagnosis and clean exit; reject other mutations as `not_ready`. A successful exit acknowledgement is queued before teardown and must not wait indefinitely for a slow client. `seasidectl` writes machine-readable JSON to stdout, diagnostics to stderr, and exits 0 on success or nonzero on transport/server errors.

**Event field types:** Event IDs use the same ranges and opaque output-ID rules as query results. On `window.focused`, each absent focused object is represented by its corresponding ID field being null; `workspace.changed.output_id` is nullable while no output association is available. Visibility/connectivity/enabled fields are booleans. Config-error line numbers are positive integers or null. Event records do not expose raw pointers, arbitrary client JSON or unescaped metadata.

## 11. Wayland and external protocol coverage

The following is a **requirements-to-protocol matrix**, not an assertion that a given protocol has already been implemented. Pin exact versions to the chosen, tested wlroots/wayland-protocols releases during development. Do not advertise a protocol unless its implementation is functional and secure.

| Approved user capability | Protocol/API family to evaluate and pin | Release posture |
|---|---|---|
| Basic native clients | Wayland core `wl_compositor`, `wl_shm`, seats/outputs, `xdg-shell` | Required |
| Buffers/presentation | `linux-dmabuf`, presentation-time, explicit `linux-drm-syncobj` | Required where available, honest capability fallback |
| Mouse locking and gaming | relative-pointer, pointer-constraints; pointer/keyboard input through seat | Required |
| Ordinary app activation and transient UI | xdg-activation, xdg-shell toplevels/popups | Needed for normal approved app behavior; policy must respect no focus stealing |
| Clipboard and DnD | core `wl_data_device`; relevant primary-selection/data-control interfaces | Required for approved workflow, scope/security to pin |
| Fractional display scaling | fractional-scale and viewport interfaces | Required for per-output fractional scaling |
| Independent shell | `wlr-layer-shell` | Required, no bundled client |
| Capture / existing tools | `ext-image-copy-capture-v1`, output and foreign-toplevel capture sources, `ext-foreign-toplevel-list-v1`; compatible `wlr-screencopy` for output tools | Output and user-selected single-window video capture required; monitor desktop-audio path needs qualification (§7.5); isolated application audio DEFERRED beyond v1 (G15) |
| Idle inhibition / monitors | idle-inhibit, DRM/KMS output power interfaces | Required for approved display sleep |
| Night light | DRM hardware gamma/CTM/LUT control where available | Required if supported; do not alter output when unsupported |
| Legacy games | external `xwayland-satellite` + X11 socket activation | Required, lazily launched |
| Text input and IMEs | XKB, key repeat; text-input/input-method only as necessary for compatible ordinary input | **Compatibility review**; additional user-facing IME features require owner approval |
| Security / locking | `ext-session-lock-v1` | **DEFERRED**; do not claim secure locking |
| Fullscreen keyboard inhibition | keyboard-shortcuts-inhibit | **Not supported in v1**; compositor bindings always win |
| HDR and extended color | modern color-management/HDR protocols | **Future**; SDR v1 |

**Security/privacy checks:** capture permissions, portal user-selection path, privileged clipboard/data-control access, socket ownership, client-controlled metadata, and memory safety of destroyed surfaces. A required external portal may require a chooser UI supplied *by that external service*; do not invent a Seaside chooser.

### 11.1 External package/release baseline

- Required developer/runtime components to pin after an actual Arch build: external `foot` terminal (required, launched only by default `Super+Return`), working Linux kernel `amdgpu` + Mesa stack, a chosen compatible version of `wlroots`, `wayland`/`wayland-protocols`, `libinput`, `libxkbcommon`, seat/session dependencies, TOML parser, a small chosen image decoder, `xwayland-satellite`, `xorg-xwayland`, `pipewire`, `pipewire-audio`, `pipewire-pulse`, `wireplumber` (including `wpctl`), `xdg-desktop-portal` and a **verified compatible** portal backend (plus whatever existing services it requires).
- **Do not fake exact version pins or pretend the portal backend has been tested.** Record `pacman -Qi` versions, dependency manifests, protocol XML revisions, compile flags and the Arch snapshot **first inside the Arch VM** for reproducible early builds, then repeat on the native Framework Desktop for hardware qualification. An optional portal compatibility package is not automatically a correct mandatory backend.
- Packaging: PKGBUILD declares **all required packages** as dependencies, including `foot`, rather than relying on users to follow a separate manual command. No separate display manager. `seaside` is invoked from a logged-in TTY. The default install does not auto-run Conch, Quickshell, foot, or any other app; `Super+Return` starts foot on demand.
- External source references for validation: [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots), [Wayland protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols), [libinput device config](https://wayland.freedesktop.org/libinput/doc/latest/api/group__config.html), [xwayland-satellite](https://github.com/Supreeeme/xwayland-satellite), [Niri XWayland integration](https://github.com/niri-wm/niri/blob/main/docs/wiki/Xwayland.md), [Arch portal setup](https://wiki.archlinux.org/title/Xdg-desktop-portal), [Linux DRM/KMS](https://docs.kernel.org/gpu/drm-kms.html).

## 12. Testing, observability and release acceptance

### 12.1 Performance diagnostics (approved v1)

- `seasidectl` can query each output's actual mode/refresh, renderer/backend, scanout-versus-composition status and reasons for fallback where observable, hardware-cursor capability, VRR and tearing *applied* state, and relevant frame/presentation counters. Never manufacture unavailable measurements.
- Detailed frame/presentation tracing is **opt-in and temporary**. Start/stop/export trace sessions through the approved local control path. No always-on frame-history buffers, FPS overlay, separate monitoring daemon or continuous logging of every rendered frame.
- Use bounded buffers and nonblocking collection. A trace must never block input/presentation; report overflow/dropped samples honestly. External measurement is needed for true input-to-photon latency.

### 12.2 Required test suite

| Test area | Minimum test set / expected property |
|---|---|
| Audio dependencies + default keys | Clean Arch installation supplies the declared audio packages and `wpctl`; standard user services activate without Seaside-owned audio daemons/autostart commands; media volume-down/up keys control the default playback device in 5-point steps with increases capped at 100%, mute toggles playback; test actual emitted media events/Fn behavior on the chosen keyboard, while ordinary F10/F11/F12 stay available to applications; deletion/remapping in TOML works; unavailable playback device/service causes a logged external-command failure without blocking/crashing Seaside; normal playback and microphone compatibility qualification recorded separately |
| Build + config | Clean Arch build; default `foot` dependency installed and `Super+Return` launches it from a shell-less session; generated TOML parses and mirrors defaults; all documented enums/ranges valid; invalid syntax, unknown keys and duplicate conflicting chords rejected; reload atomic; restore last-good snapshot across reboot; invalid first-ever TOML uses defaults + persistent banner; restart-only workspace mode reported |
| Window lifecycle | Rapid map/unmap/destroy; close focused/unfocused windows; client crash mid-configure; no stale pointers/focus; fullscreen/maximize enter/leave restores; no minimization; transient dialog ownership and workspace migration |
| Dwindle | Focused/largest insertion, dynamic longest-axis split, right/bottom new child, sibling promotion, min-size constraints, tiled pointer resize, detach/reinsert drag, temporary overrides and reload reset |
| Scrolling | One window per column, 50% default or client min, new column right, left-aligned initial strip, independent resizing, instant minimal viewport movement, wrap at logical strip edge, right-neighbor-first close, focus persistence and oldest-to-newest rebuild after global layout changes |
| Workspaces | Global and per-monitor unique IDs; never mirrored; focused output invariants; MRU restore; click focus and hovered scroll; `Alt+Tab` current-workspace; moving a window follows and focuses it for new, hidden and already-visible destination workspaces, including destinations on another monitor, without cursor warp; explicit transfer with MRU source replacement and focus follows |
| Multi-output | Primary ranking max Hz -> resolution -> connector; configured primary; physical ID matching/ambiguous best effort; auto migration, manually moved workspace not restored; unplug all, reconnect; scale 1.0/fractional; rotations; manual positions; output mode override |
| Cursor + input | Native hardware cursor on reference AMD output; unsupported output disabled, others alive; flat/neutral mouse input; relative pointer/constraints; fullscreen shortcuts always active; keyboard focus separate from wheel/pointer focus |
| Render + gaming | Single fullscreen eligible surface attempts direct scanout; compare output commits and frame pacing vs composition fallback; check presentation timing during capture, overlays, scaling, gamma and cursor; VRR off/fullscreen/always; tearing false/client-requested fullscreen; no compositor FPS cap |
| External apps | Native Wayland clients; Steam, Proton/Wine and X11 applications through lazy satellite; X11 first-launch and restart behavior; XWayland clipboard interoperability; X11 input lock/focus; mixed scaling |
| Capture + portals | `grim` and compatible recording tool; active portal/PipeWire output and chosen-window sharing with a real installed backend/chooser and actual OBS/Discord versions; chosen-window content excludes unrelated overlapping applications and is not retargeted on move/resize/destroy; changing workspaces preserves the selected window's live stream without stealing focus or revealing the newly visible workspace; native and satellite-backed X11 windows; record only on request; no capture work when idle; strict startup fails if required service broken; runtime portal/PipeWire crash does **not** terminate games or Seaside |
| Idle + night light | 15-minute/off=0 timeout, per-output fullscreen suppression of sleep, explicit inhibitors, wake input; night light defaults off and targets **4500 K** when enabled; shortcut/IPC enable state survives unrelated and temperature-only reloads, a changed configured enable value takes precedence, invalid edits preserve state, and a new process uses the loaded config value; global hardware-only warmth; unsupported output stays enabled and unchanged; no forced GPU color-pass fallback |
| Shell boundary + IPC | No shell/default autostart required; external layer-shell app works; fullscreen hides nonessential overlays; same-user IPC actions/status/events; malicious/slow subscriber bounded/disconnected; trace start-stop-export |
| Session lifecycle | Start from logged-in TTY, VT switch there/back; suspend/resume; monitor changes during resume; clean exit returns to same logged-in TTY; no special power manager, no auto-restart |

**Pre-release stability tests:** repeated output hotplug and mode changes, long fullscreen and mixed desktop sessions, repeated config-edit failures/recovery, memory/resource leak checks, interrupting satellite/portal services, and nonblocking IPC under load. Store test hardware, kernel, Mesa, wlroots, satellite and protocol versions with results. An isolated native AMD DRM/KMS run is required: nested/headless tests alone cannot prove direct scanout, hardware cursor planes, VRR, tearing or real display latency.

### 12.2a Test-environment strategy: Arch VM versus native AMD (engineering validation; no feature changes)

**Conclusion:** The **approved primary environment for all early Seaside tests is an Arch Linux VM hosted on Windows**. It is not a substitute for bare-metal release qualification on the Framework Desktop. This section specifies testing practice only; it does not add VM-specific product features, weaken the mandatory hardware-cursor rule, or expand the supported distribution/GPU list.

| Environment | Useful for | What it cannot certify |
|---|---|---|
| C unit tests / headless wlroots backend (no physical output) | TOML and IPC parsing, window-layout math, focus/history, workspace state, state-machine transitions and failure paths | Physical input, DRM output/cursor behavior, GPU presentation or latency |
| **Arch Linux guest on Windows host (primary early environment; VM platform/virtual GPU TBD)** | Clean Arch builds, guest TTY startup, real Wayland clients, input, layout and workspace behavior, config/IPC, error paths, clipboard/XWayland, and session/portal integration *when the chosen VM supports them* | Framework `amdgpu` cursor planes, true direct scanout to a physical display, hardware VRR/tearing, real gaming latency or native AMD colour controls |
| Nested wlroots instance under an existing Wayland host | Fast crash-isolated development of window focus/layout and client protocols when the selected backend supports it | TTY ownership, native KMS timing, hardware cursor-plane enforcement, VRR, physical scanout |
| Native Framework Desktop Arch session using `amdgpu`/DRM/KMS | Final reference for cursor-plane enforcement, native scanout, VRR/tearing, fractional/rotated outputs, night-light hardware LUT, Steam/Proton, real frame timings and VT/suspend | Cannot be replaced by guest-only results |

**Hardware cursor constraint (APPROVED; unchanged):** Some virtual DRM drivers offer a *virtual* cursor plane (for example, VirtIO-GPU uses cursor-plane/hotspot semantics). This may allow a guest Seaside session to satisfy its cursor requirement *within the virtual output*, but success is not guaranteed by selecting `virtio-gpu` or enabling VirGL. Verify actual exposed DRM cursor-plane capabilities and a working cursor in the guest before counting an interactive VM test as passed. If absent/unsupported, disable that output per the already approved rule; do **not** invent a software cursor, loosen enforcement, or mark a resulting black screen as a compositor logic regression. Headless tests have no physical/virtual output requiring a cursor. A nested Wayland backend should not be claimed as a hardware-cursor qualification test.

**Windows-hosted VM setup (engineering choice, not a Seaside product feature):** Select a Windows-capable VM platform and virtual GPU that actually exposes the Wayland/DRM capabilities needed for interactive guest testing; the hypervisor and graphics configuration are **not yet chosen**. Linux **KVM is not available as the native accelerator on a Windows host**: do not carry over QEMU/KVM/virt-manager-on-Linux setup instructions to this test plan. If using QEMU on Windows, investigate Windows Hypervisor Platform (WHPX); alternative Windows VM products can be evaluated separately. Neither VirtIO-GPU nor any other virtual GPU is assumed to satisfy the hardware-cursor requirement until tested. Keep a clean, disposable Arch guest, take snapshots before destructive tests, and record Windows host, hypervisor, virtual GPU, guest kernel/Mesa/wlroots and configuration versions. Slow or software guest rendering can validate functional correctness but **cannot** establish Seaside gaming performance.

**Release-test rule:** **Run all early unit, integration, session and VM-compatible graphical tests in the Windows-hosted Arch guest.** All VM/headless/nested test results must identify their environment. G7 (real hardware cursor), G9 (hardware night light), actual physical scanout, VRR/tearing and end-to-end gaming input/presentation performance remain **native Framework/AMD tests**. VM success alone never closes these gates. References: [QEMU VirtIO-GPU](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html), [Linux DRM cursor-plane and hotspot requirements](https://www.kernel.org/doc/html/latest/gpu/drm-kms.html), [wlroots backend architecture](https://gitlab.freedesktop.org/wlroots/wlroots/-/blob/master/docs/architecture.md).

### 12.2b Incremental test harness and minimum human involvement

**APPROVED development workflow:** Build the harness alongside the compositor from the first development part. Every behavior added to a part must have an automated acceptance check where practical; reserve human involvement for observations or interactions the available automation cannot establish. Do not defer the harness to final qualification, claim an arbitrary percentage of automation, or add product features solely to make tests easier.

**Harness structure (ENGINEERING CHOICE):**

| Development component | Responsibility | Boundary |
|---|---|---|
| `tests/unit/` | Exercise production layout, focus/workspace, config, binding and state-transition code directly; check expected examples and independent invariants | No physical output or second implementation of the window manager |
| `tests/clients/` | Small real Wayland clients with predictable buffers, size constraints and controllable map/unmap/destroy/input behavior; layer/capture clients where required | Test programs, never runtime shell components |
| `tests/integration/` | Start an isolated headless fixture using the production modules, connect real clients, exercise actions and verify client-observed events, buffers and state | Offscreen output and test input do not qualify a physical GPU/cursor or real TTY preflight |
| `tests/session/` | Exercise the production TTY entrypoint and real systemd-user/D-Bus/PipeWire/portal activation in the dedicated Arch test guest/session | Service fakes are useful for failure paths but cannot pass G2 or real portal compatibility |
| `tests/hardware/` | Run scripted checks and collect evidence on native Framework/AMD outputs; identify the exact remaining human observations | VM/simulated output results never become native-hardware passes |
| `tools/run-tests` | Development-only runner coordinating build targets, processes, deadlines, fixtures, cleanup and a consolidated result report | Test-only dependencies and tools are not installed as compositor services or added to runtime IPC |

Use C for fixture clients and tests linked to the compositor's C modules, with a small Python-standard-library process runner where useful. These are build/check dependencies, not a Python runtime inside Seaside. Register tests with Meson so the normal test command remains the entrypoint. wlroots provides a headless backend and offscreen outputs ([API reference](https://wlroots.pages.freedesktop.org/wlroots/wlr/backend/headless.h.html)); check its exact pinned API before writing the fixture. Meson supplies bounded test execution and result logs ([test reference](https://mesonbuild.com/Unit-tests.html)).

**Testability without product bypasses:** Inject a clock, filesystem locations, backend events and service/spawn collaborators through internal module interfaces used by the test fixture. The installed `seaside` binary still uses the real monotonic clock, normal paths and approved session/backend policy. No shipped test mode, `skip_preflight`, public input-injection API, synthetic-input protocol, hidden configuration setting or software cursor fallback is authorized. A test fixture may simulate capabilities/failures, but its results must be labeled simulated. Real graphical-session qualification runs the actual entrypoint and services.

**Deterministic checks:** Advance the fixture clock to test idle timeouts without waiting 15 minutes. Wait for protocol/state conditions with explicit deadlines instead of sleep-based timing guesses. Use fixed/recorded seeds for generated action sequences and test ownership, geometry, focus and lifetime invariants after each sequence. Exercise actual configure/ack/commit and client input where the protocol matters; checking only the compositor's own status response is insufficient. Use predictable client imagery for capture/geometry assertions rather than fragile screenshots of arbitrary application themes or fonts. Capture must remain tied to the selected window when other fixture windows overlap it or its workspace becomes hidden.

**Failures and isolation:** Cover client destruction mid-operation, stale IDs, malformed/slow IPC, config replacement/write failures, output loss, service/satellite failure and recovery. Give every test a private runtime/state/config fixture and track the processes it owns; clean up on success, failure and timeout without touching normal user files or killing unrelated processes. Tests requiring real user-manager/D-Bus names or seat resources run serially in a dedicated test account/session or disposable guest. Run sanitizer-enabled builds for relevant C lifetime/parser/state tests when supported by the pinned toolchain. Write tests for observable requirements and regression risks; avoid test-per-function quotas, copied implementation algorithms or tests that merely restate assignments.

**Results and evidence:** The runner reports each test as `pass`, `fail`, `skip` or `blocked`, with its requirement section/test ID, part, actual environment, relevant versions, command, seed where applicable, and links to bounded logs/capture artifacts. Include compile/runtime failures and cleanup failures. A skipped hardware/interactive check is not a pass. Emit a machine-readable summary alongside a human-readable report, and state explicitly which required checks remain. Fixtures may record their own requested test captures/traces; production monitoring remains opt-in and bounded as specified elsewhere.

**Manual checks only where necessary:** Automate output-state, presentation, audio-routing and recovery assertions on real hardware whenever the runner has the needed access. Remaining human work can include a physical cable/key action, actual portal user selection when no supported automation exists, audible/visible correctness on real devices, interactive game feel and externally measured latency. First prepare a narrow checklist with prerequisites, exact actions, expected results and evidence already collected automatically; ask for that observation once the relevant automated checks pass or a concrete environment blocker is identified. Do not ask a human to repeat a scriptable parser/layout/config/IPC test. Simulated consent or a service stub does not replace an actual permission-flow check.

### 12.3 Performance acceptance philosophy

**Do not assert invented absolute latency targets.** Before v1 is declared polished, publish reproducible measurements on the Framework Desktop for:

- idle CPU and GPU work with no damage or animations;
- repeated compositor frame timings and presentation misses under native and XWayland gaming;
- percent of eligible fullscreen frames reaching direct scanout, with actual fallback reasons;
- behavior with configured VRR/tearing, capture and night light active/inactive;
- output recovery and frame pacing after VT/suspend/monitor changes.

Compare like-for-like hardware, kernel, game settings and output modes. The acceptance condition is **correct behavior and demonstrably low overhead without regressions**, not an invented claim that the smallest program is automatically the fastest. Avoid collecting high-volume traces unless explicitly enabled.

### 12.4 Release checklist

Distinguish three milestones; requiring completed code before allowing implementation would be circular:

1. **Ready to start a scoped development task:** The task cites approved requirements, names its dependencies and expected checks, and contains no unresolved product decision that blocks that task. Routine engineering details follow the standard-behavior rule at the top of this document. The current design permits the initial §14 build/prototype tasks; it is not evidence that they have passed.
2. **Prototype-qualified:** A real Arch build records the toolchain/library/protocol baseline and its tests. The config/IPC contracts have implementation validators, and the staged session/portal/capture paths have been exercised in the approved VM where possible. Failed feasibility work blocks only dependent work, unless it exposes a scope conflict requiring the owner.
3. **Release-qualified:** Required functional and hardware tests pass in their specified environments, including native Framework/AMD qualification for cursor planes, scanout, VRR/tearing, night light and presentation performance. The strict startup gate works, claimed external-client compatibility is tested, and no unapproved feature has been added. Deferred isolated application audio is not a v1 blocker.

Never use a design review, syntactically valid example or installed dependency as a substitute for prototype or release evidence. The owner-approved 4500 K night-light default does not establish hardware feasibility.

## 13. Remaining engineering gates (not automatic feature requests)

**Gate scope:** A blocking gate blocks the milestone or claim that depends on it, not every independent task. Native GPU qualification can remain pending while Arch-VM config/layout work proceeds. Routine engineering gaps are resolved by agents under the standard-behavior rule; deferred work is not a release prerequisite.

| ID | Priority | Gap | Owner decision needed? | Resolution/verification |
|---|---|---|---|---|
| G1 | **Blocking** | Build-qualify a compatible wlroots/toolchain/protocol XML baseline | No unless it changes behavior | Candidate: Arch `wlroots0.20` `0.20.2-1`, `wlroots-0.20.pc`; lock versions and hashes only after successful target Arch build and protocol smoke tests |
| G2 | **Blocking** | Portal backend + PipeWire startup gate is activation-dependent and cannot itself prove interactive capture | **Yes if strict startup policy must change** | Prototype §3.1 staged Wayland-ready/portal-ready gate on target Arch; ensure no deadlock and verify actual capture separately; don't weaken Q84 silently |
| G3 | **Specified; implementation/test pending** | Monitor selector/override/position grammar and reliable physical identity | Possibly if output matching changes behavior | §8.2 defines syntax; test ID stability and best-effort reconnection with real/duplicate EDIDs |
| G4 | **Specified; implementation/test pending** | Full action name, arguments, chords, mouse gestures, and `exec` syntax | No unless behavior differs | §§4.5/8.3/9 define standard-behavior policy and parser/default bindings; build strict validator and test documented actions; record routine layout details as engineering choices |
| G5 | **Specified; implementation/test pending** | JSONL framing/version, socket, typed results, command/status/event ordering | No | §10 defines v1 shapes, watermark ordering and validation; implement same-user access and malicious/slow-client regression tests |
| G6 | **Blocking** | Capture protocol matrix must actually work with `grim`, recorder, chosen-window selection and portal sharing | Selected-window sharing and live updates across workspace switches approved; ask if implementation requires additional product behavior | Test ext-image-copy-capture with output and foreign-toplevel sources/list, external chooser and actual OBS/Discord; verify continued live capture of the selected window on a hidden workspace; qualify v0.8.4 backend candidate and legacy output fallback as needed; output-only capture is insufficient |
| G7 | **Blocking** | Mandatory hardware cursor behavior after VT/hotplug, nested testing restrictions | **Yes only if reference GPU cannot fulfill mandate** | Native DRM test and fail-safe output disable; never silently use software cursors |
| G8 | **Specified; implementation/test pending** | Logical pixel and numeric-range convention | **Only if owner objects to logical px** | §8.2 defines it; verify physical output rounding at 1.25/1.5 scale |
| G9 | **Owner-approved; hardware validation pending** | Night-light hardware colour adjustment on pinned wlroots and AMD, including output colour-state interaction | No, unless implementation cannot honor hardware-only/no-fallback behavior | **4500 K default approved**; validate proposed range 1000–10000 K, capability check, on/off state restoration, and direct-scanout impact; unsupported outputs remain on and unmodified |
| G10 | **Engineering choices; implementation/test pending** | Image decoder details, deterministic ties, invalid drag handling and floating size clamping | No unless a real conflict changes scope | Wallpaper fit fixed to cover; use §§4.5/8.1 and standard client constraints; select a small maintained decoder during the build baseline and test failure/resource limits; no additional settings |
| G11 | **Specified; implementation/test pending** | Active vs requested restart-only config, last-good persistence | No | §8.1 fixes snapshot semantics; test pending restart and recovery |
| G12 | Medium | Actual current `xwayland-satellite` compatibility with Steam/Proton and first client connection | **Yes if limits force a different architecture** | On-demand spawn/socket contract, process death recovery; real game tests |
| G13 | **Specified; source/test pending** | IPC event fields and exact source of attention events | No unless new behavior | §10.4 defines event names; restrict attention emission to genuine supported requests and test clients |
| G14 | **Resolved; verification pending** | Shell-less first launch has approved terminal launch (`Super+Return` → `foot`); exit deliberately remains unbound by default | No further decision needed for this scope | Arch `foot` is a required package; verify terminal launch from a clean session and configurable exit; do not invent an exit binding |
| G15 | **DEFERRED — not a v1 blocker** | Isolated application audio accompanying a chosen-window share depends on a verified external integration | Deferral and application-level granularity approved; no need to re-ask | Preserve §7.5's future requirement and acceptance cases; revisit when external client/portal support is available; no custom audio workaround or speculative infrastructure in v1; monitor desktop-audio qualification remains separate |

**No need to re-ask** already resolved choices (mouse flat speed 0, AMD-only initial validation, no animations, TTY-only, global workspace default, two layouts, off-by-default VRR/tearing, hardware cursor mandate, no built-in lock/power actions, Conch separation). If a future engineering discovery conflicts with one, raise the actual conflict instead of modifying the requirement.

### 13.1 Validation order for the remaining release gates — engineering plan, not features

The following sequence verifies the **existing approved** contract. It does not authorize new protocols, runtime behavior or UI. Failure of any check must be reported rather than disguised with a fallback that violates an owner decision.

| Order | Gates | Verification needed before declaring the design fully qualified |
|---|---|---|
| 1 | **G1, G3–G5, G11** | In the Windows-hosted Arch guest first, record `pacman -Q` dependency versions and `pkgconf --modversion wlroots-0.20`; compile a small compositor/API smoke build; validate that the canonical generated TOML, standalone example, keyboard actions, monitor selectors, JSONL IPC framing and restart-only reload semantics agree. The **approved** `Super+Return` → `foot` binding must work in a shell-less session. Repeat the baseline on native Arch for final hardware qualification. |
| 2 | **G2** | Launch from a real TTY, initialize the Wayland event loop and export the session environment, then run bounded, noninteractive readiness probes against the real systemd-user/D-Bus, PipeWire and selected portal frontend/backend. Confirm the probes accept on-demand activation and **do not block the Wayland event loop**. Demonstrate the required fail-to-TTY behavior with a deliberately unavailable required integration. |
| 3 | **G6** | While the graphical session is active, successfully capture a frame with `grim` and record/share both a real display and one chosen window through the selected portal/PipeWire backend and external chooser; test native/satellite windows, target movement/resize/destruction, continued live selected-window updates while switching to another workspace, exclusion of unrelated overlapping windows, repeated starts/stops and no capture processing when idle. Merely observing a D-Bus service or PipeWire socket is insufficient evidence. |
| 4 | **G7, G9** | On the Framework Desktop's **native DRM/KMS** backend, verify actual hardware cursor-plane use across supported transforms, scale factors, VT switches and resume. Verify night light is **off by default**, uses the approved **4500 K** target when manually enabled on supported outputs, and never invokes a GPU colour pass; unsupported night-light outputs remain enabled and unchanged. Measure direct-scanout eligibility with night light both off and on. |
| 5 | **G12 and remaining gaming gates** | Exercise the approved lazy satellite startup with an X11 program, then real Steam/Proton games, fullscreen presentation and pointer constraints; test restarts of the external satellite and report any compatibility limits without silently substituting another X11 architecture. Capture presentation traces only on request. |

**Evidence convention:** For each verification, record the tested date, Arch kernel, Mesa, wlroots, PipeWire, portal/satellite versions, monitor identifiers and observed result. Mark a gate **passed** only when tested, not when the dependency exists or source documentation suggests it should work. Release qualification is distinct from approval of new features.

## 14. Ten development parts and coding-agent handoff

Use **10 development parts**, each ending in concrete test evidence. A part may contain several small implementation tasks/changes; it is not a requirement to produce one large diff or stop for approval after every part. The harness starts in part 1 and grows with the code under §12.2b. These are incremental engineering milestones, not authorization to invent features. The current workspace contains design/reference material, not a built compositor; no compile, runtime or GPU result is implied by this handoff.

**Build convention (ENGINEERING CHOICE):** Use Meson/Ninja, a pinned Arch C toolchain targeting C17 with the Linux/POSIX interfaces required by wlroots, and separate compositor/CLI/test targets. Choose the TOML 1.0 parser and small image decoder as build dependencies after checking their real APIs and licences. Keep layout/focus/config state tests independent of physical DRM outputs. Standard build commands, to be provided by the initial implementation, are:

```sh
meson setup build --buildtype=debug
meson compile -C build
meson test -C build
```

These commands are a handoff contract, **not commands successfully run against this workspace**. [Meson build/test reference](https://mesonbuild.com/Running-Meson.html).

| Part | Dependencies / modules | Concrete completion evidence |
|---|---|---|
| 1. Build, feasibility and harness foundation | G1; Windows-hosted Arch VM; toolchain, wlroots/protocol APIs, dependency manifest, test runner | A minimal API build and headless fixture start/stop, recorded exact versions/XML revisions, runner timeout/cleanup/result reporting, and an actual guest DRM cursor-capability probe. Add a real acceptance check as soon as the first production behavior exists; an empty test suite is not evidence. If interactive output is unavailable, record that and continue independent headless work; no software cursor fallback. |
| 2. Config and state foundations | Part 1 toolchain/harness; `config`, window/workspace model | Parse generated/reference TOML, strict validation, complete-map binding semantics, recovery/restart-mode behavior and object-lifetime invariants; automated positive/failure cases run in the Arch guest. |
| 3. Session and core loop | Parts 1/2; `server`, `session`, `output`, `input`, minimal XDG/scene/capture integration | Guest TTY/seat startup, live Wayland dispatch, hardware-cursor enforcement, one `foot` window, black wallpaper and clean exit; prototype G2's asynchronous strict readiness gate now. Automate lifecycle/service checks in the dedicated guest; report any actual seat/display access still needed. |
| 4. Windows and workspaces | Part 3; `window`, `workspace`, basic focus/geometry | Real test clients plus state assertions for map/unmap/destroy, click focus/background scroll, MRU, workspace selection/move with follow, maximize/fullscreen/pin restoration and zero-output survival. |
| 5. Dwindle and scrolling layouts | Part 4; `layout/dwindle`, `layout/scrolling` | Implement each layout in small tasks with its own acceptance cases: geometry, insertion/removal/resize, drag cancellation/reinsert, viewport persistence/rebuild and global override resets. Use deterministic examples and generated invariant checks; confirm client-observed size/configure behavior. |
| 6. Live config, IPC and bindings | Parts 2/4/5; `config`, `ipc`, `seasidectl` | Automated file-replacement/reload/recovery, persistent config-error rendering, exact action dispatch, typed results/event ordering, malformed/slow-client limits and default binding tests. Physical Fn/key mapping is a narrow hardware check; no new IPC methods or native audio/power handlers. |
| 7. Compatibility, audio and sharing | Part 3 plus relevant state/control; `satellite`, `capture`, data-device integration | Lazy first-X11 connection, child/client failure handling, clipboard/DnD, normal external audio controls, whole-output and original-window capture including hidden-workspace live updates. Use scripted clients and real portal/receiver checks; list only unavoidable chooser/client interactions for a human. Isolated application audio remains deferred. |
| 8. Display and GPU behavior | Working core; `render`, `output`, session recovery; native Framework/AMD for qualification | Automate mode/scale/rotation/hotplug-state/fallback checks and native measurements where possible. Qualify actual cursor planes, scanout, explicit sync, VRR/tearing, fullscreen aspect and hardware night light. Record physical/visual checks separately; VM results do not close native gates. |
| 9. Idle, layer-shell, diagnostics and recovery | Relevant prior modules; `idle`, layer-shell, `diagnostic`, session | Fixture-clock idle/inhibitor tests, test layer clients/fullscreen policy, bounded trace tests and scripted VT/suspend/recovery checks where session access permits. Physical wake/resume/display observations remain explicit. No bundled or required shell client. |
| 10. Packaging and release qualification | All required v1 parts/gates | Clean-install PKGBUILD/dependency checks, first-use configuration/terminal/media keys, repeatable real startup/failure-to-TTY, full automated regression/stability report and remaining native/manual evidence. Release only when required checks pass and support limits are accurate. |

**Prototype boundary:** Early API/unit/integration fixtures may exercise incomplete components, but must be clearly identified as test harnesses. Do not introduce a shipped `skip_preflight`, software-cursor switch or weakened production launch path to make a milestone pass. The release entrypoint must satisfy the strict approved contract. A stage is complete only when its relevant evidence exists; coding a placeholder protocol advertisement or returning a fake service-ready result does not count.

### 14.1 Completion criteria for every part

- Implement only the cited approved behavior and keep the production/test interfaces small. Add or extend the relevant harness checks alongside it; do not leave testing as an unspecified future task.
- Run a clean compile, the part's acceptance cases and relevant earlier regression cases in the approved environment. Run sanitizer/resource/failure-path checks when the changed C behavior warrants them. Keep tests meaningful rather than adding low-value tests to satisfy a quota.
- Record the implementation status separately from verification: `implemented`, `automated_verified`, and `qualification_pending` where hardware/interactive evidence is still unavailable. A part is fully complete only after its required checks pass; a pending check must name the exact missing environment/action.
- Produce a short report of requirements covered, engineering decisions, actual commands/results, artifacts and remaining manual/native checks. Keep a runnable scenario for reproducing failures. A screenshot alone is not proof of input, ownership, memory safety or driver behavior.
- Continue to the next independent authorized task after successful checks. Lack of native hardware must not stop unrelated headless work, and skipped tests must not be relabeled passed. Ask the owner only for a real product conflict or a necessary concrete observation/access requirement.

### 14.2 Code comments and maintainability

**APPROVED comment standard:** Comments explain reasoning where it is non-obvious: why a choice exists, which invariant or protocol ordering it preserves, what ownership/lifetime constraints matter, or which documented hardware/API quirk requires it. Explain difficult layout math or synchronization at the point a maintainer needs that context. Prefer clear names and small functions for ordinary code; do not narrate C syntax or restate obvious statements.

Document interface contracts when useful: borrowed versus owned references, callback reentrancy/destruction hazards, event-loop/thread requirements and failure semantics. Link a relevant requirement section or upstream issue/version for an unusual workaround, while including enough local explanation to understand why it is needed. Keep comments correct when code changes. Do not add boilerplate above every function, commented-out alternatives, conversational history or speculative TODO features. Test comments should identify the behavior/regression being protected, not describe assertion syntax.

**Per-agent task contract:** cite approved requirements/sections; name the stage, dependencies and touched modules; implement the smallest coherent task; document routine engineering choices under the standard-behavior rule; compile and run relevant tests in the approved environment. Report checks actually run separately from checks still requiring hardware/access. Never claim GPU behavior measured elsewhere was qualified on the Framework Desktop. Record newly needed functionality as an unapproved proposal. If a gate blocks one task, report that scoped blocker and continue independent authorized work; defer G15 rather than treating it as a v1 prerequisite. Ask the owner only about an actual scope/interaction/requirement conflict, not routine technical details.

## 15. Record of decisions and external research

The supplied draft referenced `SEASIDE_DECISION_ARCHIVE.md` for a full chronological record and `SEASIDE_REPO_AUDIT.md` for Hyprland/Niri research. Neither file is present in this workspace, so that historical material has not been verified here. Do not reconstruct it as though it were verbatim history. Current requirements are in this document; the dated notes below preserve decisions recorded here. The standalone `SEASIDE_CONFIG_EXAMPLE.toml` is a development reference extracted from §9, not a second runtime settings file.

**Research-only 2026-11-19 update:** Arch package and portal/session integration feasibility notes are in §3.1. They do not authorize additional Seaside functionality, and do not close G1/G2/G6 pending native tests.

**Engineering-contract revision 2026-11-19:** §§8–10 now specify the exact TOML tables/types/key syntax and a versioned JSONL Unix-socket IPC allowlist with events, limits, and error handling. Source examples were syntax-checked; no runtime behavior was implemented, and no new feature was approved. `render.explicit_sync` remains a mandatory capability, not a user switch. The previously proposed 4500 K night-light default was subsequently approved in the owner decision below.

**Owner decision — 2026-11-19:** `foot` approved as Seaside’s default terminal, installed through the Arch package dependencies; `Super+Return` launches `foot` on demand. No integrated terminal, persistent terminal process, or other default application launcher is added.


**Owner decision — 2026-11-19 (night light):** Approved **4500 K** as the default `night_light.temperature_kelvin`. `night_light.enabled = false` remains the default. Night light remains global, manually controlled, hardware-only and inactive on unsupported outputs without disabling them. This closes the product-choice portion of G9 but does **not** assert that the target AMD hardware or pinned wlroots supports the needed output colour controls. No additional features were authorized.

**Engineering feasibility review — 2026-11-19 (no scope changes):** The current [Arch `wlroots0.20` package](https://archlinux.org/packages/extra/x86_64/wlroots0.20/) is a viable **candidate dependency**, not a successfully built Seaside baseline. Public wlroots output documentation exposes gamma-LUT state APIs, but this must be checked against the exact installed `wlroots-0.20.pc` headers and DRM capabilities. The [xdg-desktop-portal-wlr integration notes](https://github.com/emersion/xdg-desktop-portal-wlr) require Wayland desktop environment variables in D-Bus activation; Seaside's strict noninteractive portal/PipeWire startup gate remains unproven on the target Framework Desktop. Do not claim G1/G2/G6/G7/G9 complete until the corresponding build, functional and hardware tests pass. No new v1 functionality or owner decision has been inferred from these references.

**Configuration consistency check — 2026-11-19:** The embedded canonical TOML and separately distributed `SEASIDE_CONFIG_EXAMPLE.toml` must be byte-for-byte equivalent (ignoring outer Markdown fences and trailing newlines) before publishing. This review corrected the embedded template so the already-approved `Super+Return` → `foot` shortcut is active, rather than mistakenly appearing as a commented optional example. No new shortcut was approved.

**Current confirmed late-stage revisions:**

- `xwayland-satellite` on demand supersedes integrated wlroots XWayland/XWM.
- TTY graphical-session integration is automatic, and all declared portal/PipeWire requirements pass a strict startup gate; later external service failures are recoverable without shutting down Seaside.
- VT switching and suspend/resume preserve state; exiting Seaside returns to the same logged-in TTY.
- Systemd already supplies suspend/reboot/poweroff. No Seaside-specific power handlers, IPC or confirmation UI; optional user-defined `exec systemctl ...` only.
- Window pinning = sticky per monitor, temporarily floating; not always-on-top.
- Night light = manual, global, hardware output adjustment only; **off by default, 4500 K when enabled**; no GPU fallback or schedule.
- Diagnostics = on-demand status + temporary bounded traces; no continuous monitoring.
- Keyboard move/swap/resize supported, deliberately **without default shortcuts**.
- **Approved default terminal:** install external `foot` as a required Arch package; bind `Super+Return` to `exec foot`. It launches only on request, with no shell or terminal daemon.

**Development readiness and evidence:** §12.4 defines readiness to start development separately from prototype and release qualification. G1/G2/G6/G7 and night-light hardware feasibility (G9) require actual implementation/runtime evidence; they are not prerequisites for writing the code that will test them. The default terminal and night-light temperature are already approved; exit remains unbound. The embedded TOML reference and JSON message examples have been checked with Python's standard parsers, **not** a built Seaside implementation. Update this canonical document when the owner changes a requirement, and preserve a short dated revision note without fabricating missing historical records.

**Engineering test-plan note — 2026-11-19 (no scope changes):** All early testing will be in an **Arch Linux guest VM hosted on Windows** (hypervisor and virtual GPU not yet chosen). Verify the guest cursor plane before interactive testing; otherwise use headless tests without weakening hardware-cursor enforcement. Guest cursor/virtual presentation do not qualify physical cursor planes, scanout, VRR, tearing or latency. Later hardware-sensitive release gates remain native Arch on the Framework Desktop.

**Owner-approved testing workflow — 2026-11-19:** All **early testing** happens **inside an Arch Linux VM running on Windows**. This is solely the test host arrangement; Seaside continues to target Arch Linux, and the native AMD Framework Desktop remains mandatory for final GPU, hardware-cursor, direct-scanout, VRR/tearing, night-light and latency validation. The VM product and virtual GPU are not yet decided. No Windows build target, software cursor fallback, VM-specific Seaside features or new functionality is authorized.

**Owner clarification — 2026-11-19 (appearance):** Outer gaps of 10 px, inner gaps of 5 px, and border width of 2 px are defaults users can change in the TOML configuration. This resolves the former contradiction between the fixed-value wording in §1 and the editable settings in §§8–9. Square borders and the existing exclusions of decorative effects and compositor animations remain in force.

**Owner decision — 2026-11-19 (binding removal):** Deleting or commenting out a binding in the `[keybinds]` table disables it. A present table is authoritative; omitted entries must not be restored from compiled defaults. This applies equally to shipped defaults and user-added shortcuts. The generated first-use template retains the approved default shortcuts.

**Owner decision — 2026-11-19 (window workspace move):** Moving a window to another workspace automatically follows the window. The destination workspace is selected using the existing output/ownership rules, and the moved window receives keyboard focus. Keybindings and IPC use the same behavior; existing no-cursor-warp and no-workspace-mirroring rules still apply.

**Owner decision — 2026-11-19 (night-light reload):** Night light enabled through a shortcut stays enabled when unrelated TOML settings are edited. Shortcut/IPC runtime control is preserved across reloads unless the configured `night_light.enabled` value actually changes. Configuration and effective runtime state are distinct; restarting Seaside initializes from the loaded configuration rather than persisting runtime toggles.

**Owner decision — 2026-11-19 (chosen-window sharing):** Sharing one chosen window is required in v1; whole-monitor sharing alone is insufficient. Extend the capture protocol/backend qualification to selected-window sources and external selection. Audio was raised for clarification and remains OPEN in §7.5; no audio feature or audio default has been inferred from this decision.

**Owner decision — 2026-11-19 (shared-audio granularity):** Application-level audio isolation is acceptable when sound cannot be separated between individual windows of the same application. The intended mapping is selected-application playback audio for a chosen-window share and desktop playback audio for a monitor share, when shared sound is enabled externally. This confirms the intended behavior; a working external implementation and its requirement for the v1 release remain unresolved in G15. No custom audio engine, new default shortcut or automatic microphone mixing is approved.

**Owner decision — 2026-11-19 (isolated audio deferred):** Defer isolated application audio for chosen-window screen sharing beyond v1 and retain it as a future requirement. Application-level granularity remains acceptable. This resolves G15's release-timing question: isolated audio is not a v1 implementation task or release blocker. Keep single-window video sharing in v1 and desktop playback audio for monitor shares as a separate integration; do not silently replace isolated app audio with the whole desktop mix.

**Owner decision — 2026-11-19 (default media keys):** Volume-up, volume-down and playback mute should work by default, including the media events produced by Fn/function-row combinations on keyboards that provide them. Bind standard XKB media symbols, not hard-coded physical F10/F11/F12 positions or a fabricated Fn modifier. The engineering implementation uses external `wpctl` commands, with editable 5-point steps and a 100% increase cap; the standard Arch audio stack is installed through package dependencies under the existing required-dependency policy. No embedded PipeWire, audio settings UI, microphone-mute default or play/pause default is added.

**Owner decision — 2026-11-19 (window sharing across workspaces):** A chosen-window share continues showing the selected window when the user switches to another workspace. Preserve the same capture target and live updates while its workspace is hidden; do not switch the stream to the new workspace or change local focus to maintain capture. Isolated application audio remains deferred independently.

**Owner direction — 2026-11-19 (standard interactions):** Keep chosen-window video tied to the original window and use generic protocol behavior for ordinary surface interactions. Resolve routine engineering details without repeatedly asking the owner about edge cases. Only substantive scope, interaction or requirement conflicts need another product decision; this is not authorization for additional features.

**Engineering handoff cleanup — 2026-11-19:** Separated scoped development readiness from prototype/release qualification; documented the absence of historical references; extracted the standalone TOML reference; added ordinary state/ownership/transaction rules, typed IPC result shapes, subscription watermarks and dependency-ordered milestones. These are implementation contracts for existing scope. Static documentation checks do not establish an Arch build, running compositor, portal compatibility or hardware qualification.

**Owner-directed development workflow — 2026-11-19:** Develop in testable parts with a harness built alongside the implementation, minimizing human involvement to checks that genuinely need it. Code comments explain decisions, non-obvious behavior and difficult reasoning rather than pure syntax. The engineering plan uses 10 parts (§14), each with concrete evidence and explicit remaining verification; this adds development tooling and standards, not runtime product functionality.

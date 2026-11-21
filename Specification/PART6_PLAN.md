# Part 6 plan for live config, IPC, and bindings

Part 6 makes edits to the user's TOML take effect in a running Seaside session
and adds the `seasidectl` client for local status, actions, and events. Part 2
already supplies `ConfigStore`, `ConfigTransaction`, and `AsyncConfigIo`.
Production startup uses the loader and writes a recovery snapshot after
readiness. Part 4 supplies the desktop action paths, and Part 5 supplies layout
updates and override reset behavior. Reuse these foundations in the running
session. [Part 2 handoff](PART2.md) · [Part 4 handoff](PART4.md) · [Part 5 handoff](PART5.md)

The contracts are in §§2, 5.2, 6, 8–10, 12.2b, and 14 of the design. The TOML
schema, default chords, and IPC method and event names are already specified.
[Seaside design](SEASIDE_SPEC.md)

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 6A. Watch and recover the config | Attach the Linux file and directory watches and a debounce timer to `wl_event_loop`. Route reloads and recovery writes through the bounded worker queue and use `ConfigTransaction` for active and requested settings. | Test in-place writes, atomic rename, deletion and recreation, directory replacement, watch overflow and rescan, and overlapping edits and worker completions. Verify invalid startup recovery, successful repair, and persistence failure. |
| 6B. Apply prepared settings | Prepare changed wallpaper and output state before committing. Update desktop geometry and layout, bindings, input settings, and live output policy through their owners. Preserve the distinction between requested settings and actual device state. | Prove a rejected candidate leaves the running settings and recovery snapshot intact. Check layout override resets, output validation and rollback, and the startup and restart semantics described below. |
| 6C. Render config errors | Render a compositor-owned top-edge banner using PangoCairo and the wlroots scene. Reuse its image until the diagnostic or relevant output geometry or scale changes. | Capture the banner at startup and after a bad edit, including with a fullscreen client. Check Unicode paths, clipping, scale, output loss and return, and removal after valid recovery. |
| 6D. Share typed action dispatch | Route keyboard and IPC actions through existing desktop and session policy. Replace the keyboard and pointer binding map together. Resolve optional explicit window and workspace targets through the existing state model. | Exercise focused, explicit, missing, stale, and ineligible targets. Compare shortcut and IPC outcomes for the same action. Test the default chords, map replacement and events already in flight during a reload. |
| 6E. Serve IPC and build `seasidectl` | Add the same-user nonblocking Unix socket, strict JSONL envelopes, typed queries and responses, and committed-state events. Build `seasidectl` as a separate Meson target using the documented command mapping. | Test discovery, readiness and shutdown, response shapes, subscription ordering, malformed input, slow readers, and the CLI's JSON output and exit status with real sockets. |
| 6F. Verify and hand off | Add a `part6` suite to Meson and `tools/run-tests`. Extend the real Wayland fixtures and the automated Arch VM trial. Record commands, revisions, dependencies, and evidence in the Part 6 handoff. | Pass a clean build, relevant earlier suites, ASan/UBSan, and the guest TTY scenario. Publish verified commits to `origin/main` and verify the usual guest checkout and rebuilt `build/seaside` at the handoff revision. |

## Reload and rendering acceptance

- Read and parse a complete file after debouncing, retaining the 1 MiB input
  limit. Invalidate superseded parse/decode work when a newer edit arrives.
  Guard recovery writes by accepted generation so their completion order
  cannot restore an older snapshot. Verify first launch creates the canonical
  template without replacing a concurrently created file. On invalid startup,
  try a valid last-known-good snapshot and fall back to compiled defaults if
  recovery fails. Exercise shutdown with work outstanding.
- Validate explicit connected-output modes and ambiguous selectors before any
  live changes. Keep disconnected overrides dormant. Pre-test affected output
  states and retain rollback state. If a hardware failure prevents rollback,
  report the actual output state and use the existing migration and hardware
  cursor policy. Verify failure paths with injected output results and test
  the supported mode, scale, or rotation changes in the guest.
- Decode changed static PNG, JPEG, and WebP wallpapers on the worker queue
  using libpng, TurboJPEG, and libwebp. Check bounded dimensions and allocation,
  fixed aspect-preserving cover/crop, the black default, texture reuse, and
  retention of the current wallpaper when decode or output preparation fails.
- Commit the live settings together on the event loop. Only a committed change
  to `window.tiling` or `window.layout` resets temporary overrides. Keep the
  active `workspaces.mode` until restart, expose the requested mode and pending
  restart, and persist the requested TOML. Prove a valid edit applies its
  other settings and the next process starts in the requested mode. Autostart
  executes once after preflight. A temperature-only or unrelated edit preserves
  the effective night-light enable state. A committed change to the configured
  enable value replaces it.
- Keep the user TOML and last-known-good snapshot after a rejected edit. Show
  the persistent, nondismissible diagnostic with path, line when available,
  field, and message, and report it through logs and IPC. Verify valid repair
  clears the banner and updates the snapshot. A failed snapshot write after a
  successful commit leaves the previous durable snapshot available and reports
  the storage failure through logs and status.

## Bindings and IPC acceptance

- A present `[keybinds]` table replaces the complete map, including when it is
  empty. An absent table restores compiled defaults. Test removal and remapping
  of `Super+Return`, default `foot` launch, navigation and both mouse drags,
  plus preservation of the current map on rejection. Check consumed input
  pairs across reload and drag cancellation. Synthetic standard XF86 media
  keysyms should invoke the configured external `wpctl` commands. Physical Fn
  mapping remains a keyboard check.
- Bind a Wayland-instance-specific socket under `$XDG_RUNTIME_DIR/seaside/`
  with `0700` directory and `0600` socket permissions. Verify `SO_PEERCRED`,
  `SEASIDE_SOCKET` in the session environment, CLI discovery via
  `WAYLAND_DISPLAY`, concurrent-instance isolation, and cleanup on startup
  failure and exit. Exercise queries, subscription and clean exit before
  readiness, and verify other mutations return `not_ready` during preflight.
- Use nlohmann/json with the §10 framing and limits. Cover fragmented and
  coalesced records, 64 KiB requests, 1 MiB output records, nesting depth 16,
  unknown and duplicate fields, unsupported versions, bad numeric types and
  ranges, and duplicate in-flight IDs. Enforce 32 in-flight requests and the
  per-connection queue cap of 256 records or 1 MiB. Verify a slow or malformed
  peer leaves a real client and another IPC peer responsive. Arbitrary shell
  execution remains a TOML binding action and is rejected over IPC.
- Project typed snapshots from authoritative state. Check stable instance IDs,
  stale window targets, bounded UTF-8 metadata, and null or actual capability
  values for unobservable or unsupported output facts. Include a zero-output
  round trip. Verify `accepted` and `changed` separately, especially for a
  close request whose client has yet to act, and deliver the exit reply with
  bounded time for teardown.
- Emit §10.4 events after state commits and derive attention and integration
  events from genuine transitions. Test subscribe acknowledgement before
  notifications, interleaved queries and events, and response `seq` as the
  snapshot watermark. Reconnect after backpressure and resubscribe and query to
  recover current state. Verify sequence behavior with zero subscribers.

## Qualification

- Record the exact Arch dependency versions for nlohmann/json, PangoCairo,
  fonts, and the wallpaper codecs. Use deterministic fixtures for interleaved
  edits, output rollback, and protocol limits. Run the guest scenario with a
  temporary config and real clients. Verify the banner visually and through
  capture, pair config and workspace transitions with CLI queries and events,
  and check session startup, restart and clean return to the TTY.
- Part 8 qualifies the native output and rendering paths, including scanout,
  VRR, tearing and hardware night light. Part 6 checks settings and status
  against the available backend and fixtures, with explicit capability
  reporting. Part 9 adds idle timer behavior and the bounded trace recorder
  and exporter. Keep the §10 contract and document when the trace commands
  become available.

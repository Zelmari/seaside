# Part 5 plan for Dwindle and scrolling layouts

Part 5 gives each workspace a working tiling layout. Dwindle is the default,
and scrolling is selectable in the startup TOML. Both use Part 4's window
lifecycle, focus, and workspace policy. The requirements are in §§4–5, 8–9,
12.2b, and 14 of the design. [Seaside design](../../SEASIDE_SPEC.md) · [Part 4 handoff](../Part-04/HANDOFF.md)

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 5A. Connect layouts to window policy | Keep `State` authoritative for window identity, home workspace, and first-map order. Let `WindowManager` own workspace-local layout data and feed effective geometry through `Desktop` and the existing scene bridge. | Retain Part 4 lifecycle and workspace coverage. As each layout lands, exercise startup selection in both workspace modes and verify that each eligible, attached window occupies exactly one layout position in its home workspace. |
| 5B. Implement Dwindle | Insert into the eligible focused tile or the largest tile according to the configured policy. Recalculate split axes from the current region, put new tiles on the right or bottom, and promote the sibling subtree on removal. Adjust affected split ratios for resize. | Check focused and largest insertion, ties, nested removal, axis changes after work-area resize, and client minima in feasible and constrained layouts. On focused close, prefer a spatial sibling or nearby eligible window, then workspace MRU. |
| 5C. Implement scrolling | Maintain an ordered strip with one window per column. Start at the left usable edge, insert to the right of the focused column, and resize columns independently. Reveal focus instantly by moving the viewport only as far as needed. | Check the configured default width (50% by default), client minima and wider-than-viewport columns, focus and viewport staying in place at either strip end, right-then-left focus on focused close, and workspace round trips that preserve order, custom widths, viewport, and eligible focus. |
| 5D. Route tile movement and resize | Use the existing mouse drag and typed keyboard actions. Detach a tile during move drag, then reinsert it on the pointer-selected side of a valid tile or column. Restore valid membership on cancellation or invalid drop. Route resize to a split boundary or column width. | Exercise drops on both sides of targets, including left/top Dwindle drops, and cancellation during window, output, or input loss. Check configured keyboard move, swap, and resize, and preserve the default bindings. |
| 5E. Apply layout changes and restore window state | Connect `Super+D` and the existing configuration transaction effect to layout policy. Preserve floating restore geometry, keep dialogs floating, and handle pin, maximize, and fullscreen transitions. Rebuild on a committed global layout change. | Prove global layout and tiling changes clear temporary overrides across all workspaces, including for hidden and unmapped windows. Check effective geometry and focus after restoration, and verify the next layout rebuild uses original opening order. |
| 5F. Verify and hand off | Register a `part5` suite in Meson and `tools/run-tests`. Extend the real Wayland client fixture and the automated Arch VM trial. | Collect clean-build, regression, sanitizer, client-protocol, and TTY evidence with the exact revisions tested. Publish verified commits to `origin/main` and verify the normal guest checkout and `build/seaside` at the handoff revision. |

## Scope and acceptance details

- **Geometry and membership.** Use the logical work area, configured outer and
  inner tiling gaps, square borders, and committed client size limits. Check
  odd dimensions, small work areas, and large accepted settings for
  deterministic rounding and safe geometry. Keep floating restore geometry,
  tile allocations, and client commits distinct. Keep a maximized or
  fullscreen tile's ordinary membership for restoration. Pinning detaches a
  tile and frees its space until unpinning, when the saved eligibility and
  intervening global settings determine its return. Check workspace transfer
  and loss and return of all usable outputs.
- **Dwindle rules.** Equal dimensions split left/right. Use the largest
  eligible tile if the focused tile is unavailable. Break equal-area ties by
  first-map order and then ID. Check that a resize respects feasible subtree
  minimum sizes and that cancellation leaves the dragged window in a valid
  tree. For generated sequences, assert unique leaves, complete splits, and
  nonoverlapping allocations within the tiling area after each operation.
- **Scrolling rules.** Preserve neighboring widths on resize and close gaps
  after removal. Closing an unfocused column preserves focus. Focus navigation
  and MRU can reveal off-screen columns. Check that a fully visible focus
  target leaves the viewport stationary and a wider column reveals the
  relevant edge. Clip presentation and pointer hit testing to the owning
  viewport, including with an adjacent headless output. Check that focus never
  warps the cursor. In generated sequences, assert unique columns, monotonic
  column positions, valid viewport offsets, and stable widths for columns that
  the operation leaves alone.
- **Action and drag lifetimes.** Use stable window IDs and end a drag cleanly
  if its source disappears, the workspace or output changes, or the input
  device is removed. Cover invalid drops, target removal, and consumed button
  pairs. Directional move, swap, and resize stay on the current workspace and
  output. Scrolling uses horizontal operations, and a vertical operation with
  no eligible target is a no-op for a binding. Preserve Part 4's pointer grabs,
  constraints, floating movement, and hover scroll.
- **Configuration effects.** Exercise startup settings and pass validated
  configuration changes through the runtime policy in tests. A global
  layout change discards the old layout history and rebuilds eligible windows
  oldest first. Scrolling starts at the left with configured default widths
  subject to client constraints. Dwindle uses normal insertion policy. Check
  that unchanged, unrelated, and rejected config changes preserve temporary
  overrides. Part 6 supplies file watching, full live reload, and public IPC.
- **Protocol and guest evidence.** Extend the headless fixture to assert XDG
  configure sizes, state, ack and commit sequencing, including delayed commits
  and a client disappearing mid-operation. Confirm presented bounds and hit
  targets during a viewport shift and after restoration. Run deterministic
  examples and seeded operation sequences through the shared harness. In the
  Arch VM, use a temporary config to exercise both layouts with multiple
  terminals and record observed size changes, focus, drag and resize outcomes,
  VT return, and clean shutdown. Retain commands, versions, seeds, reports, and
  artifacts. Record any unavailable case with its specific limitation. Native
  connector and GPU qualification is scheduled for Part 8.

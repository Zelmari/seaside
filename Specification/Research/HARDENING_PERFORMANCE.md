# Scene lookup and floating resize measurements

At 128 mapped windows, the complete floating resize dispatch took 42.36 µs at the median, down from 78.53 µs. The complete window query took 95.88 µs, down from 169.05 µs. An eight-window full projection took 1.46 µs, up from 1.39 µs.

## Method

The comparison starts at `bfe4bda`, which includes the optimized-build fix, the opening and focus fixes, and the constrained-resize changes. Both sides used GCC 16.2.1, Meson `debugoptimized` (`-O2 -g`), wlroots 0.20.2 and Pixman 0.46.4 on Arch WSL2. The CPU was an AMD Ryzen AI Max+ 395. The benchmark pinned the fixture to logical CPU 16.

The Part 4 fixture opened one real XDG client per 40 × 30 floating window on a single 160 × 90 headless output. Each client had a distinct title and app ID. The timed operations were a one-pixel alternating left/right resize through `Desktop::dispatch`, a full `Desktop::project` with no resize, and `SessionControl::query("query.windows")` through construction and destruction of its JSON result. The query timing excludes socket transport and JSON text serialization. Clients held their existing buffers during the timed resize loop.

Each process warmed up each operation ten times, then measured 800 individual calls with `CLOCK_THREAD_CPUTIME_ID`. For each window count, seven rounds ran the variants in randomized order. Each cell gives the median of the seven run medians and the median of their 95th percentiles, in microseconds. Link-time wrappers counted scene placements and calls to `wlr_xdg_toplevel_set_size`.

| Windows | Resize before | Resize after | Full projection before | Full projection after | Window query before | Window query after |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 4.95 / 5.18 | 4.15 / 4.36 | 1.39 / 1.42 | 1.46 / 1.49 | 9.69 / 10.22 | 5.60 / 5.95 |
| 32 | 17.62 / 23.50 | 13.22 / 17.22 | 5.08 / 5.29 | 4.88 / 5.51 | 38.34 / 55.40 | 23.61 / 33.98 |
| 64 | 30.16 / 39.66 | 19.43 / 24.92 | 11.19 / 13.90 | 9.56 / 12.04 | 77.46 / 102.89 | 47.21 / 68.51 |
| 96 | 55.56 / 77.06 | 34.82 / 42.02 | 21.60 / 29.91 | 14.43 / 18.63 | 119.08 / 145.85 | 71.09 / 96.39 |
| 128 | 78.53 / 104.59 | 42.36 / 51.02 | 36.00 / 53.40 | 19.57 / 30.52 | 169.05 / 205.51 | 95.88 / 124.61 |

An independent series rebuilt the reviewed `6777bd5` sources with only the cursor-coordinate initialization required by GCC. At 128 windows the reviewed-to-final median/p95 pairs were 79.70/112.65 to 42.62/54.73 µs for resize, 36.47/54.56 to 19.60/31.88 µs for projection, and 171.28/209.78 to 96.63/125.55 µs for the query. At eight windows, full projection increased from 1.38/1.41 to 1.46/1.50 µs.

## Placement and lookup costs

At 8, 32, 64, 96 and 128 windows, the 800 resizes originally performed 6,400, 25,600, 51,200, 76,800 and 102,400 scene placements. Each final run performed 800 placements and all variants issued 800 size requests. A normal full projection still places every window. The earlier [review](HARDENING_REVIEW.md) counted 36, 528, 2,080 and 8,256 ID-list visits for metadata at 8, 32, 64 and 128 windows. The new query performs one hash lookup per live ID and constructs each row in its result array.

A separate candidate compared placement fields by value and skipped content and border updates on equality. It still visited every window. With the same index and JSON construction, its median/p95 resize times were 4.73/5.07, 16.15/20.06, 25.86/35.11, 44.71/59.35 and 55.97/75.47 µs at those five window counts. The shipped resize path limits placement to the floating window and its descendant dialogs. Full projection continues to handle tiled layouts, pending opening completion, focus, output geometry and drag cancellation. Surface commits continue to process changed buffers, capture constraints and outstanding configures.

## Behavioral checks

- The benchmark validated ordered metadata for every live client. After destroying a middle client and switching workspaces, it checked all nine JSON fields of every remaining hidden window.
- The headless desktop suite exercised sparse 64-bit IDs with identical low bits, destruction, unmap/remap and new metadata. A separate case resized an unfocused parent beneath two nested dialogs, checked the dialogs' new pixels and pointer hit target, retained keyboard focus, and committed the parent's requested replacement buffer.
- The adjacent fixtures exercised floating borders, tiled neighbor configures, fullscreen fitting, selected-window capture through resizes and subsurfaces, hidden-window updates, independent output capture and layer-shell input. These are compositor CPU measurements. Physical display timing requires native hardware.

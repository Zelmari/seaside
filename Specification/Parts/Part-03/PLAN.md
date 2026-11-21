# Part 3: headless groundwork and first VM boundary

Scope: implement the independent, headless-testable foundation of specification
§§2.1–2.2, 3–3.1 and 14, then stop before extending the graphical session beyond
its first required Arch-VM qualification. This is a partial Part 3 milestone,
not completion of Part 3 or gates G1/G2/G7.

Status: implemented and software-verified in Arch WSL2. The [handoff](HANDOFF.md)
records the final normal/sanitizer results, review fixes and remaining VM work.

| Block | Implementation | Evidence |
|---|---|---|
| Live core and scene | Own a Wayland socket, dispatch and shutdown sources; add minimal XDG configure/map/unmap ownership and wlroots scene integration | Real isolated Wayland client, configure/ack/SHM commit/frame lifecycle, offscreen black/client pixels, cleanup and existing lifecycle regressions |
| D-Bus adapter | Direct sd-bus connection and asynchronous calls integrated with libwayland FD interests, monotonic timers and bounded dispatch | Private bus roundtrip/error/timeout/cancel/disconnect tests, main-loop progress and callback teardown |
| PipeWire adapter | Official libpipewire connection check using a correlated core sync/done exchange | Isolated real server connection where available, absent-server/error/cancellation/deadline cases and safe teardown; no playback/capture claims |
| Readiness coordination | Fixed mandatory-service results, overall deadline, failure categories and deferred completion | Explicitly simulated service reports proving all requirements must pass, failures cannot become readiness, and cancellation/stale results cannot invoke completion |
| Integration and handoff | Register cases by part, record newly used API/library versions, run regressions and relevant sanitizers | Actual test reports, clear fixture/real-service boundaries and a narrow first-VM checklist |

The startup coordinator does not implement or certify the real systemd-user,
activation-environment or portal checks. Real adapters are separate from service
policy: their success proves only the operation tested. Private service fixtures
do not satisfy G2 and must not touch the normal user's activation environment or
services. The selected portal backend and readiness exchange still need the
dedicated graphical session.

The minimal scene is a protocol/lifetime foundation. It does not add window
management, layout/focus policy, input injection, capture protocols or a shell.
The offscreen fixture explicitly selects headless/Pixman; it does not implement
production renderer selection or relax the hardware-cursor requirement.

No incomplete executable is installed as `seaside`, and no preflight bypass,
software-cursor fallback, setting or binding is introduced for tests. The actual
TTY entrypoint, seat/DRM resources, physical output/input handling and real
service activation stay pending rather than receiving fake successful results.

## Stop and test the VM

Before building further on the graphical session, choose the Windows-hosted
Arch VM and run the existing `seaside-probe-drm` there. Record its DRM/output/
cursor capabilities and exact environment, including explicit failures/skips.
Then qualify the real TTY/backend, renderer/output/input and one on-demand
`foot` window, together with strict asynchronous service activation and clean
failure/exit to the logged-in TTY. This later integration is not silently claimed
by headless success; native AMD cursor/latency qualification remains separate.

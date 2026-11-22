# Part 7 plan for compatibility, audio, and sharing

Part 7 adds lazy X11 compatibility, clipboard and drag-and-drop, and live
output and selected-window sharing. It also qualifies the external audio
stack and the default media keys from Part 6. Reuse Part 3's strict session
preflight and monitor-capture path, Part 4's window and workspace lifetimes,
and Part 6's bindings and session status. The current portal smoke test
verifies a screenshot. The Part 7 streaming checks must observe frames from
the selected source in a receiver. [Part 3 handoff](../Part-03/HANDOFF.md) · [Part 4 handoff](../Part-04/HANDOFF.md) · [Part 6 handoff](../Part-06/HANDOFF.md)

The requirements are in §§3, 7.2–7.3, 7.5, 11–12, and 14. G6 covers
end-to-end capture, and Part 7 supplies the X11 session and desktop evidence
for G12. [Seaside design](../../SEASIDE_SPEC.md)

## Delivery steps

| Step | Implementation | Acceptance evidence |
|---|---|---|
| 7A. Check the external stack | Pin the guest's wlroots, protocol XML, satellite/Xwayland, portal, chooser, audio, and capture-tool versions. Start the portal compatibility check alongside the independent X11 and clipboard work. | Record the supported globals and portal source types, the chooser's source-selection behavior, and the buffer path used by the installed tools. Identify any blocker specific to the guest, chooser, or receiver. |
| 7B. Activate X11 on demand | Reserve a local X11 display and pass its listeners to the external `xwayland-satellite` child on the first connection. Publish `DISPLAY` through the existing session environment and supervise the owned child on the event loop. | Prove a native-only session keeps satellite stopped. Map a real X11 client on first connection, handle concurrent clients with one child, and verify a later connection resumes X11 service after child failure. |
| 7C. Wire the seat data devices | Use wlroots seat selection, primary selection, and drag-and-drop with keyboard and pointer focus from the current input path. Evaluate external clipboard-manager access only for the approved workflow. | Transfer known content between real Wayland clients and in both directions between Wayland and X11. Exercise drag/drop, cancellation, and source or target exit. |
| 7D. Complete output and toplevel sources | Extend the existing image-copy manager to serve connected outputs and a stable source for each eligible toplevel. Add `ext-foreign-toplevel-list-v1` and the foreign-toplevel capture source. Support legacy `wlr-screencopy` where a qualified output tool requires it. | Verify source identity, frame pixels, resize, overlap, workspace switches, and target loss. Exercise `grim` and a compatible recorder such as `wf-recorder` against the pinned versions. |
| 7E. Qualify the portal path | Use the installed portal frontend/backend, real chooser, and a PipeWire receiver. Update the existing noninteractive checks to match the qualified backend and its advertised capabilities. | Start and receive both output and chosen-window streams. Verify selection and cancellation, stream termination on target loss, and continued compositor operation after a portal or PipeWire disconnect. |
| 7F. Qualify external audio | Exercise the current `wpctl` bindings with PipeWire, `pipewire-pulse`, and WirePlumber. Check playback, microphone input, and monitor-share desktop playback in the external sharing client. | Observe volume/mute state on the default sink, collect actual playback and input evidence, and verify audio at the receiving client with sharing sound enabled. |
| 7G. Verify and hand off | Register a `part7` suite in Meson and extend the automated Arch TTY trial. Record how to reproduce each remaining interactive check in the Part 7 handoff. | Pass the build, relevant earlier suites, sanitizer checks, and applicable Arch VM cases. Record the exact tested revision. |

## X11 and clipboard acceptance

- Confirm the pinned satellite socket and readiness contract. Allocate a
  collision-safe display number and exercise an occupied display, concurrent
  Seaside sessions where supported, missing or failed child startup, and
  shutdown with a connection or child exit pending. Verify the waiting first
  client can create a mapped window. Check that a child crash leaves native
  clients and IPC responsive, and that cleanup closes and reaps only owned
  resources. Record any remaining Steam/Proton, X11 pointer lock, focus, and
  mixed-scale qualification on the reference system.
- Validate selection and drag request serials and ownership against the seat
  and source client. Exercise MIME negotiation, clipboard and primary
  selection independently, focus changes, drag icons, successful drops,
  canceled drags, and disconnects mid-transfer. Check interaction with
  compositor-owned window move and resize gestures.
- If an approved external clipboard manager needs a data-control protocol,
  pin the minimum compatible interface and verify its privileged access and
  denied-client cases. Clipboard history and source-exit persistence belong
  to external utilities. [Clipboard requirements](../../SEASIDE_SPEC.md#73-clipboard-and-drag-and-drop)

## Capture and portal acceptance

- Start with `xdg-desktop-portal-wlr` 0.8.4, the Part 3 baseline. Its source
  enables the portal's window source type when the image-copy and
  foreign-toplevel source managers are present. Confirm the installed build
  advertises both `MONITOR` and `WINDOW`, and verify the actual chooser can
  list, select, and cancel a window choice. Use a chooser compatible with the
  protocols available in Part 7. If the backend, chooser, or receiver blocks
  a required path, record the exact combination and continue independent
  checks. G6 stays open until both paths work end to end. [Backend source](https://github.com/emersion/xdg-desktop-portal-wlr/blob/v0.8.4/src/screencast/wlr_screencast.c#L506-L546) · [ScreenCast API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html)
- Bind monitor streams to the requested enabled output. Verify multiple
  outputs in a deterministic fixture, including capture of a non-primary
  output and loss of the selected one. Exercise mode or scale changes and
  cursor-included and cursor-excluded frames. Maintain the hardware cursor on
  the display while serving capture buffers.
- Render the selected toplevel's surface tree into its capture buffer. Use
  changing test patterns to verify native and satellite-backed X11 sources
  through movement, resize, overlap by another client, and workspace switches.
  Check that hidden-window commits continue to reach the receiver with
  keyboard focus and workspace visibility unchanged. Separate dialogs follow
  the protocol's surface semantics. If a target is lost, stop its stream and
  reject stale handles. Verify a newly mapped window requires its own selection.
- Provide frame callbacks and damage scheduling for hidden content only
  while an active capture needs it. Exercise slow or disconnected receivers,
  overlapping sessions on one source, and repeated start/stop. Verify that
  pending requests and buffers are released and that capture copies and
  hidden-window wakeups cease after the last session closes. Check protocol
  permissions for toplevel metadata, capture sources, and trusted portal
  clients, including denial where required. Keep source selection and consent
  in the external portal and chooser.
- Run the real ScreenCast session from source selection through delivery of
  PipeWire frames to a scripted receiver. Verify both installed OBS and
  Discord paths where available and record exact versions and observed
  results. Repeat the strict fail-to-TTY check with a required service
  unavailable at launch. After readiness, induce a scoped portal or PipeWire
  loss in the dedicated test environment and check that existing clients and
  IPC survive. Full external-service recovery is scheduled for Part 9.

## Audio and qualification

- Use a controlled default sink and the production media bindings to check
  `wpctl` volume changes in five-percentage-point steps, the 100% cap, mute
  toggle, and the current default device after a sink change. Check remapping
  and removal, ordinary function keys, and logged command failure with no
  playback device or service. Reserve physical Fn mapping and audible checks
  on real devices for the observations the guest cannot establish.
- Exercise normal playback and microphone input through the external audio
  stack. For a monitor share with sound enabled, check the agreed desktop
  playback mix at the receiver with distinguishable signals. Verify microphone
  separation, audio/video sync, cancellation, and device changes. Establish
  which playback devices the external path actually includes before claiming
  support for a multi-device mix. Isolated application audio remains deferred
  beyond v1. [Audio requirements](../../SEASIDE_SPEC.md#75-audio-boundary-desktop-sharing-and-deferred-application-isolation)
- Run `tools/run-tests --suite part7`, relevant earlier suites, and ASan/UBSan
  where the changed C/C++ lifetimes warrant it. Extend the guest runner with
  per-trial config and artifacts. Record the exact tested revision, package
  and protocol versions, renderer, source types, receiver results, and logs.
  In the [Part 7 handoff](HANDOFF.md), give the smallest reproducible checklist for chooser steps,
  receiving-client interactions, and physical observations that still need a
  person. Native display and GPU qualification is scheduled for Part 8.

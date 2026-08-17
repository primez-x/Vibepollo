# Automatic Client HDR Calibration Inheritance

**Date:** 2026-08-16
**Status:** Approved design revised after implementation-plan review

## Problem

Vibepollo currently exposes a per-client HDR color-profile selector, but the
selector is host-local. The host enumerates `.icc` and `.icm` files from its
own Windows color-profile directory, reads the selected profile's MHC2 peak,
and uses that value for the virtual-display EDID, RTX HDR target, and HDR10
stream metadata.

Windows Moonlight currently reports HDR decoder support through `hdrMode=1`,
but it does not report the active client display's calibrated HDR values. The
existing HDR capability fields are sent with zeroed display data. A Windows
HDR Calibration profile created on the client therefore cannot affect the
host unless the same profile is separately installed on the host.

The current UI and getting-started documentation consequently describe a
workflow that is misleading for a remote Windows client: calibrating the
client does not make that profile available to Vibepollo.

## Goal

When a supported Windows Desktop Moonlight client starts an HDR stream,
Vibepollo should receive the client display's calibrated HDR peak before host
display preparation begins. The host should then feed that value through the
same effective peak-brightness path already used by manual HDR profile and
numeric overrides.

Automatic selection must prefer the calibrated values reported by the active
client ICC profile, use the active display's Windows/DXGI-reported values when
calibrated values are unavailable, and retain the existing global fallback for
clients that report neither. The fallback is EDID-derived when Windows reports
it from the display descriptor, but v1 does not claim that
IDXGIOutput6::GetDesc1() is a raw EDID read.

The existing manual host profile and explicit numeric peak override remain
available for handheld, tablet, older, non-Windows, or otherwise incapable
clients.

## Non-goals

- Do not transmit or install the client's ICC file on the host.
- Do not apply a client ICC transform to a host physical display.
- Do not infer client platform or display capability from `clientName`, paired
  device name, or `hdrMode` alone.
- Do not persist client display values in the paired-client record; the values
  are a per-session snapshot and may change when the client changes displays.
- Do not renegotiate HDR capabilities after the stream starts or after the
  Moonlight window moves to another display in v1.
- Do not rewrite native HDR content/mastering metadata based on a target-display
  peak unless an existing encoder/display seam explicitly supports that
  operation.
- Do not change the existing manual `.icc`/`.icm` profile-management workflow
  for clients that need it.

## Product behavior

The effective HDR target is selected in this order:

1. An explicit per-client or per-application numeric peak override.
2. An explicitly selected host-side HDR profile and its readable MHC2 peak.
3. The active Windows client's calibrated ICC/MHC2 luminance values.
4. The active Windows client's EDID/DXGI luminance values.
5. The global `rtx_hdr_peak_brightness` value and existing defaults.

Within automatic client data, a valid calibrated ICC/MHC2 peak wins over the
EDID peak. A non-empty manual host-profile selection suppresses all automatic
client data, even when that host profile has no readable MHC2 peak; in that
case the existing global fallback is retained while the selected host profile
continues through the existing association path.

Client-derived values are eligible only when the host's final effective HDR
request for that launch or resume is enabled. This is a host-side policy
check, not merely a sender-side omission rule; a valid capability on an SDR
request must not replace the host's process-global HDR target.

The client reports values, not a profile path or file. The calibrated source
is the active extended-color display profile associated with the display where
Moonlight will place the stream window. The wire object's `edid` member is
retained for v1 compatibility, but its `source` is `dxgi-output`: the value is
the selected Windows/DXGI display descriptor and may reflect EDID, driver, or
Windows-rationalized data. The client also reads that same descriptor so the
host can distinguish calibrated values from the display-reported fallback.

For the current Vibepollo pipeline, the peak luminance value is the only
client field that changes behavior in v1. It is passed through the existing
runtime override so it controls the virtual-display HDR target wherever the
selected virtual-display backend accepts it and controls the RTX HDR target.
Native HDR content metadata remains source-driven unless an existing encoder
seam explicitly supports a target-display override. Minimum luminance,
full-frame luminance, primaries, and white point are intentionally out of the
v1 wire contract because the existing MHC2 parser has only a verified peak
field and the units/semantics of the other fields have not been established
in this codebase.

Automatic client data must never cause a host physical ICC association. A
manual host profile continues to use the existing association and restoration
logic. For a virtual display with no manual host profile, existing stale
profile-clearing behavior remains in force.

## Client-to-host contract

MoonlightQt adds an optional `clientDisplayCapabilities` launch query
parameter. The value is URL-safe base64 encoding of a compact JSON object.
The explicit parameter avoids changing the undocumented meaning of the
legacy `clientHdrCap*` fields and is ignored by older hosts that do not know
it.

The decoded v1 shape is:

```json
{
  "version": 1,
  "platform": "windows-desktop",
  "calibrated": {
    "source": "windows-icc-mhc2",
    "peak_luminance_nits": 1000
  },
  "edid": {
    "source": "dxgi-output",
    "peak_luminance_nits": 1000
  }
}
```

`calibrated` and `edid` are independently optional. A source is usable for
the peak only when its `source` is one of the v1 source identifiers and its
`peak_luminance_nits` is a finite JSON number in the inclusive wire range
`1..100000` nits. The host rounds accepted fractional values to the nearest
whole nit before applying the existing `400..2000` effective target clamp.
Unknown fields are ignored so the contract can grow without breaking older
Vibepollo builds.

The host and client share these independent transport limits: a maximum of
4096 bytes for the URL-decoded base64 value (including optional terminal
padding), a maximum of 3072 decoded JSON bytes before JSON parsing, and a
maximum of 12288 raw URL-encoded value bytes before query decoding. The
host's common HTTP request adapter creates a `request_query_view` before any
handler runs. The view is a closed, value-only forwarding surface containing
method, path, headers, content/body, local and remote endpoints, precomputed
transport/TLS identity, the sanitized query map/string, and a bounded
capability preflight result. It contains no raw `Request*`, raw query string,
query parser, or conversion back to `Request`. Every `nvhttp.cpp` route
callback, including `/serverinfo`, authorization/logging, `/launch`, and
`/resume`, receives this view. `print_req()` logs only the view's sanitized
query, and launch/resume read the capability only from the preflight result. A
static source check rejects direct `request->parse_query_string()` calls,
raw-request handler parameters, and raw request/query access outside the
adapter, and verifies every route is wrapped by the adapter.

The same sanitizer/redaction helper is used by the separate configuration HTTP
surface (`confighttp.cpp`, `confighttp_rtss.cpp`, and
`confighttp_playnite.cpp`). It adapts that surface's `req_https_t` query source
into the same value-only view; its web-UI launch path receives an explicitly
empty client-capability snapshot and cannot opt into the Moonlight capability
protocol, while its request logger and generic query parser consume the same
sanitized view. The route-inventory check covers both HTTP surfaces, so a
capability-shaped secret cannot appear in either logger or parser.

The initial configuration-surface inventory is `confighttp.cpp:5701-5857` for
all default/resource registrations, `confighttp.cpp:580-595,1037,1072,1336,
3307,3334,3911,3956,4323,5076,5416,5656` for logger/parser and web-UI launch
seams, `confighttp_rtss.cpp:38,95,233-235`, and
`confighttp_playnite.cpp:62,130,228,258,288,320,372,411,425,468,1349,2137,
2182,2228-2259`. Implementation must enumerate every route and every raw
parser/logger access in all three files and leave zero unclassified accesses.

The adapter scans the corresponding raw request query before copying or
percent-decoding. It removes every case-insensitive
`clientDisplayCapabilities` field from the query view while retaining all
other query fields unchanged. A value over 12288 bytes, a repeated capability
field, or malformed percent encoding marks only the capability as invalid and
leaves the rest of the request launchable. A valid field is percent-decoded
into a bounded buffer, then the decoded-base64 and decoded-JSON limits are
enforced before their respective operations. The original raw value is never
passed to a logger or generic query map. The sender rejects the first limit
before request construction. The encoded value is unpadded
URL-safe base64 (`A-Z`, `a-z`, `0-9`, `-`, `_`); optional terminal `=` padding
is accepted, but whitespace, invalid alphabet characters, and excess padding
are rejected. The host also rejects unknown versions without failing launch,
rejects non-finite values, and validates strict JSON types before using any
field. A malformed or unsupported payload is equivalent to no client
capability data and must not prevent the stream from starting.

The host advertises support with `ClientDisplayCapabilitiesVersion=1` in
`/serverinfo`. The client sends this parameter only when that capability is
advertised, for an HDR-capable Windows Desktop session, and for both `launch`
and `resume` requests. GFE and servers that do not advertise the capability
receive no new parameter. If the output cannot be resolved, it sends no
capability payload rather than risking data from the wrong monitor.

## Windows client collection

MoonlightQt adds a Windows-only capability collector used before the
asynchronous launch request is started:

1. On the GUI thread, resolve the selected `QQuickWindow` screen to exactly one
   SDL display using the existing display geometry path, move the hidden test
   window, and verify that `SDL_GetWindowDisplayIndex()` reports the same
   display. A null, unmatched, duplicated, or changed mapping is unresolved:
   keep the existing rendering fallback if needed, but send no capability
   payload. The real SDL stream window does not exist until after the host
   launch request, so it must not be queried from the launch worker.
2. Map that display's GDI/DXGI identity to one active `DISPLAYCONFIG` path,
   reusing the existing mapping approach in `d3d11va.cpp`. Retain the complete
   identity tuple: SDL display index and DXGI adapter/output ordinal, GDI
   source name, source adapter LUID/source ID, target adapter LUID/target ID,
   active-path flags, and target availability. The GDI source lookup uses
   `path.sourceInfo.adapterId` and `path.sourceInfo.id`, while the ColorProfile APIs receive
   `path.targetInfo.adapterId` as `targetAdapterID` plus
   `path.sourceInfo.id` as `sourceID`. Dynamically resolve the Windows
   ColorProfile APIs from `mscms.dll` so older supported Windows versions
   remain loadable when those exports are unavailable.
3. Query `ColorProfileGetDisplayUserScope()` and
   `ColorProfileGetDisplayDefault()` with `CPT_ICC` and
   `CPST_EXTENDED_DISPLAY_COLOR_MODE`. The returned profile name must be one
   basename component: reject empty names, separators, drive/device/UNC
   prefixes, `:`, and `.`/`..` components before joining it to the color
   directory. Open the canonical color directory with
   `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT`, explicitly
   using share modes that prevent delete/rename while the authority handle is
   retained. Reject a reparse-tagged directory and retain that handle plus its
   volume/file ID for the entire candidate open. Enumerate the exact direct
   child entry through the retained directory handle and record its volume/file
   ID. Open the candidate read-only with `FILE_FLAG_OPEN_REPARSE_POINT |
   OPEN_EXISTING` and the same no-delete/rename share policy. Query reparse
   attributes before reading and require the candidate volume/file ID to match
   the retained directory's direct-child entry. Compare the candidate handle's
   `GetFinalPathNameByHandleW()` against the retained directory's final path
   using a single normalization routine: normalize `\\?\UNC\` and ordinary
   UNC forms to the same comparison form, strip trailing separators except for
   a root, and use `CompareStringOrdinal(..., TRUE)` with an exact separator
   boundary after the directory prefix. Reopening the directory after the
   candidate open is diagnostic only; the retained-directory child entry plus
   candidate file-ID match is the containment authority. A replacement,
   junction, or swap-back race or identity mismatch omits the profile. Reject
   missing/unreadable files and alternate-stream or UNC/device inputs, then
   perform one bounded read. The ICC read has a 250 ms wall-clock budget;
   implement it with a cancellable/overlapped read where required so a slow
   local file cannot hold the GUI path indefinitely. On timeout, cancel the
   read and omit only the calibrated value; continue with a valid DXGI
   fallback. Keep all validated handles live through the read. Apply the same
   32 MiB/structural checks as the host and parse MHC2. Free the API-owned name
   with `LocalFree`. Missing
   exports, unsupported OS builds, missing profiles, `STANDARD`-only profiles,
   and parse failures produce no calibrated value.
4. Query the selected DXGI output through
   `IDXGIOutput6::GetDesc1()`. Populate the wire object's `edid` member with
   `source: "dxgi-output"` only when the descriptor is current, uniquely
   mapped, attached to the desktop, reports the v1 HDR color space
   `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`, and has a finite positive
   `MaxLuminance`; reject SDR, scRGB/other color spaces, clone/default,
   detached, ambiguous, or stale output data. This conservative allowlist is
   a Windows/DXGI display-reported fallback, not a claim of raw EDID
   provenance.
5. Serialize only the normalized peak values; never send the client file path,
   profile filename, monitor serial, adapter identifiers, or raw ICC/EDID
   bytes.

Capture the selected-display identity before profile/DXGI reads and revalidate
the QScreen-to-SDL index plus every captured DISPLAYCONFIG/DXGI identity field
afterward: adapter/output ordinal, GDI name, source/target LUIDs and IDs,
active flags, target availability, output device name, desktop coordinates,
`AttachedToDesktop`, `ColorSpace`, and the luminance fields used by the
snapshot. Windows exposes no descriptor generation number here; a successful
requery with exact identity/descriptor equality is the currentness check. A
hotplug, clone transition, window relocation, or output change between those
phases invalidates the snapshot and sends no capability.

The collector must tolerate missing profiles, displays without HDR support,
profile parse failures, and Windows API races. It should log the local source
chosen for the payload without logging the full serialized value. It records
collection latency and treats an ICC read exceeding 250 ms as a calibrated
source failure while preserving any valid DXGI fallback.

The immutable capability snapshot is passed into `NvHTTP::startApp()` before
the asynchronous connection thread begins. The host receives the data before
virtual-display creation and capture preparation. A later display move is
outside v1 and leaves the launch snapshot unchanged.

`Session` owns a GUI-thread-only RAII probe-window lease from initialization
through `Session::start()` and stores it in an explicit preparation state
(`unprepared`, `prepared`, `invalidated`, `handed-off`, or `completed`). Each
preparation has a monotonically increasing generation. Initialization failure,
cancellation, or display invalidation increments the generation, destroys or
clears the probe window/snapshot, and prevents handoff. The collector produces
a value-only
`client_display_preparation_t` containing the selected-screen identity,
SDL/DISPLAYCONFIG/DXGI identity tuple, normalized source values, and encoded
capability string; it contains no `QScreen*`, `SDL_Window*`, or mutable API
handle that a worker may access. `Session::start()` is GUI-thread-only and,
immediately before constructing `AsyncConnectionStartThread`, calls the
GUI-thread
`finalize_client_display_preparation()` while retaining or recreating that
probe window. It rechecks the selected screen, calls
`SDL_GetWindowDisplayIndex()`, re-collects the complete identity/descriptor
when the mapping changed, and clears the value if validation fails. Only then
does it atomically move the value-only launch snapshot and generation into the
worker constructor, destroy the probe window on the GUI thread, and mark the
state `handed-off`. Repeated `start()` is rejected or idempotently returns
without creating a second worker. Replace the current worker's direct
`Session*`, `m_AsyncConnectionSuccess` mutation, and direct success/error side
effects with a value-only `connection_start_result_t { generation, success,
error }` signal. A queued GUI-thread slot alone validates generation and active
state before mutating `Session`, emitting UI signals, or entering `exec`;
cancellation invalidates the generation before signaling the worker. Worker
completion is accepted only when its generation equals the handed-off
generation and the session remains active; late completion after cancellation,
stop, destruction, or a newer start is discarded. The worker performs no
QScreen, SDL, DISPLAYCONFIG, DXGI, or Windows ColorProfile calls and cannot
mutate `Session`. The remaining race after this handoff is a documented v1
limitation; no later renegotiation is attempted.

## Vibepollo host integration

New Vibepollo builds advertise `ClientDisplayCapabilitiesVersion=1` in the
existing `/serverinfo` XML. Old Moonlight clients ignore this additional
element. The client uses this explicit advertisement to decide whether to
send `clientDisplayCapabilities`; it does not infer support from a generic
non-GFE classification, `appVersion`, or a guessed server family.

Moonlight stores the parsed advertisement as the runtime-only
`NvComputer::clientDisplayCapabilitiesVersion` field. Each server-info parse
starts from zero, accepts only an integer version of exactly `1`, and assigns
the parsed value through the existing `NvComputer` refresh/merge path,
including an explicit zero for absent, malformed, zero, or unknown values.
Same-host copy construction and assignment carry the current runtime value;
default construction, persisted reload, and construction for a replacement
host identity start at zero. The failed-refresh owner in
`computermanager.cpp` explicitly invalidates the field under the host lock
when all-address polling fails, the host goes offline, or an identity
replacement prevents `NvComputer::update()`; a later successful refresh may
restore version 1. The field is excluded from `NvComputer` persistence and
serialized equality and is never reused after an invalidating event.

The host parses `clientDisplayCapabilities` for both launch verbs before
computing runtime overrides and stores the validated result on
`launch_session_t`. It must remain separate from the persistent
`crypto::named_device_t::hdr_profile` field so client data cannot silently
become a saved host configuration. `clone_for_startup()` must copy the new
field explicitly.

Client-derived policy uses one canonical effective-HDR resolver shared by
session construction and peak policy. Before evaluating it, launch/resume
builds the prospective runtime map and applies a candidate map value for
`dd_hdr_request_override` in preference to the previously active global value.
The resolver then produces exactly the existing
`rtsp_stream::effective_hdr_requested(const launch_session_t&)` predicate,
`enable_hdr && !prefer_sdr_10bit && !force_sdr`, after `hdrMode`, per-client
10-bit-SDR preference, and the Windows force-on/force-off/automatic setting
have been applied. Launch and resume resolve this seed once before peak
selection and pass the same result into the session builder and policy; neither
may independently reinterpret `hdrMode` or read a stale global override.
Backend display mode must not substitute a different HDR predicate.

The effective HDR target helper consumes both the existing explicit settings
and the new session capability object. It returns the selected peak, source,
and fallback reason. Runtime configuration override creation uses the
selected peak in the same `rtx_hdr_peak_brightness` field currently used by
manual MHC2 profile extraction. Existing supported-range clamping is
preserved, but logs must include both the reported and effective values when
clamping occurs.

The selected effective peak must be available before:

- Sunshine temporary-display creation, where the existing
  `hdr_max_luminance_nits` request receives the effective target;
- RTX HDR runtime initialization, where the existing
  `rtx_hdr_peak_brightness` runtime key becomes
  `config::video.rtx_hdr.peak_brightness` and is copied into the RTSP session
  peak; and
- any other backend that explicitly accepts the same target field.

The log source should distinguish at least `explicit-override`,
`host-icc-mhc2`, `client-icc-mhc2`, `client-dxgi-output`, and
`global-default`. v1 does not add a live source-status API; the settings UI
explains the static policy, while redacted runtime logs identify the selected
source and fallback reason.

If the client sends calibrated data but no valid peak, the host may use its
DXGI/global peak fallback. If the client sends no payload, all existing
non-client behavior remains unchanged.

### Backend and shared-session ownership

The Sunshine virtual-display backend already accepts a per-creation maximum
luminance and is the v1 exact-inheritance path. The current SudoVDA wrapper
does not pass a luminance value to its driver and discards `hdr_requested`.
This plan deliberately keeps SudoVDA automatic handling `runtime-only`: the
reported peak may feed RTX HDR, but the host must not claim that SudoVDA's
virtual EDID was matched. Manual host profiles remain the exact fallback for
that backend. A future SudoVDA protocol extension is outside this change.

The host runtime override is process-wide. The existing HTTP route locks
`launch_request_mutex` and `stream_lifecycle_gate` serialize launch/resume
ownership decisions. While holding that lifecycle gate, an idle first session
resolves and publishes the effective target. A joining RTSP/WebRTC request
observes activity and must not mutate the active runtime target; it inherits
the already-published runtime value. A failed first or replacement launch
must restore the previous runtime-override snapshot and owner record.
Final stream teardown must not blanket-clear overrides while an application is
paused; the owner record and retained map remain available for resume. The
next request that observes true lifecycle idleness may transactionally replace
that target, while application termination performs the existing eventual
clear. Per-session capability fields may still be retained for logs, but they
do not retarget global capture/encoder state while another owner is active.

Implement this as a private `hdr_runtime_owner_state`/transaction manager
shared by HTTP, RTSP, WebRTC, stream teardown, and process termination, using
the existing `stream_lifecycle_gate` as its single mutation lock. The manager
is the sole production authority for the complete runtime override map:
low-level whole-map set/clear functions are private to it, and unrelated-key
edits use manager transactions under the same gate. Every transaction advances
a monotonic map revision. A generation-checked lease stores the proposed
owner token, prior map, prior owner, candidate map, owned HDR keys, map
revision, and phase (`provisional`, `awaiting-stream`, `active`, or
`retained-paused`).

An idle launch/resume begins a lease and publishes the candidate map; HTTP
success leaves it awaiting stream ownership rather than committing it. Pending
joins bind participant tokens to that awaiting-stream cohort. The first
RTSP/WebRTC ownership publication from any participant commits the cohort's
matching generation; canceling one participant removes only that participant.
Rollback is permitted only after the cohort has no pending participants and no
active publication. When rollback is permitted, it restores only lease-owned
HDR entries if the generation is current and those entries still equal the
candidate values; it preserves unrelated current keys and newer explicit HDR
edits. Synchronous errors, pending-session cancellation/expiry,
virtual-display failure, and asynchronous stream-start failure roll back the
matching lease under the gate. A stale rollback cannot overwrite a newer
generation. A join copies the active owner's target without opening a separate
lease. Last-stream teardown marks a still-running application
`retained-paused` without clearing the map; a later idle request replaces it
transactionally. `proc_t::terminate()` clears the runtime map and owner state
together.

All runtime-map writes must use this manager. In particular, launch/resume,
WebRTC's first-capture path, application termination, live
`rtx_hdr_peak_brightness` edits, and unrelated runtime-key edits are manager
transactions. An explicit live numeric edit updates owner provenance
atomically. A direct generic whole-map replacement cannot omit or clear the
owned HDR key without going through an owner transition.

The manager exposes explicit lifecycle transitions for pending-participant
cancel/expiry, first RTSP or WebRTC publication, stream teardown, paused
retention, replacement, and process termination. The centralized stream
finalizer in `stream.cpp:2705-2808` and every RTSP/WebRTC caller of it must
invoke the matching transition under `stream_lifecycle_gate`; no backend may
clear or retain the map by changing only its local stream state. This includes
RTSP timeout, RTSP teardown, WebRTC teardown, and application termination.

Cross-domain process edits use one lock order: acquire
`stream_lifecycle_gate` before `_apps_mutex`; no path may acquire the lifecycle
gate while holding `_apps_mutex`. A live app edit snapshots and validates its
app UUID/revision under `_apps_mutex`, releases it, acquires the lifecycle gate,
revalidates the revision, performs the manager transaction, and commits app
state without calling `config::apply_config_now()` until both locks are
released. Process termination retains its existing gate-first entry. Lock-order
assertions and a stress test cover simultaneous app edits, launch/resume,
teardown, and termination.

The initial lifecycle inventory is pinned to `stream.cpp:2705-2808,2993-3005`,
RTSP callers at `rtsp.cpp:672-677,724-756,819-846,1076-1127,1137-1163,1882`,
WebRTC callers at `webrtc_stream.cpp:3014,3456-3462,3491,3500,5419,5518,5544,5641`,
process/runtime writers at `process.cpp:2513-2523,2719,3962-4027,4030-4091`,
HTTP/runtime writers and client-override construction at
`nvhttp.cpp:3217,3300-3340,3786-3795,4168-4170,4709-4722,4913`,
and external termination callers at `confighttp.cpp:2220,2855,5223,5245`,
`main.cpp:528`, `system_tray.cpp:107,113,120,530`, and
`platform/windows/playnite_integration.cpp:947`. Implementation must rerun
the repository inventory and leave zero unclassified lifecycle or map writers.

## User interface and documentation

The new and legacy client settings views should make the policy explicit:

- Rename or clarify the automatic option as “Automatic (client display
  capabilities when available)”.
- Explain that supported Windows Desktop Moonlight clients report their
  active calibrated display values automatically before streaming.
- Explain that manual profiles and peak values are host-side overrides for
  clients that cannot report calibrated data or require custom tuning.
- Remove the instruction that a user must run Windows HDR Calibration while
  streaming and then select the client profile on the host as the normal
  Windows workflow.

Update the getting-started HDR guidance to describe automatic Windows client
reporting and retain manual calibration guidance only as an explicit fallback.
Update the English locale sources for both web clients and preserve existing
translation/fallback conventions rather than replacing unrelated translations.

## Compatibility and safety

- Old Moonlight clients omit the new parameter and continue using the current
  global, manual-profile, or manual-peak behavior.
- Non-Windows clients omit the parameter and continue using the manual profile
  or global fallback. This covers Steam Deck and iPad clients without a
  client-side protocol change.
- GFE launch/resume requests do not receive the custom parameter; the current
  GFE request shape remains unchanged.
- The pinned pre-feature Vibepollo baseline
  `f8c4ac2762b351457ee57aef0863655d18b936e4` parses launch/resume query
  parameters as a map and reads known keys without rejecting unknown keys, but
  it does not advertise `ClientDisplayCapabilitiesVersion`. The new client
  therefore omits the field when talking to that baseline. This exact
  old-host behavior and the current Vibepollo advertisement are the v1
  compatibility guarantee; generic Sunshine/Apollo servers are not assumed to
  accept the field merely because they are non-GFE.
- The sender-side gate is the advertised version, not a heuristic based on
  `isNvidiaServerSoftware`, `appVersion`, or an unknown server family. A host
  must advertise version 1 before the client constructs the new query field.
  A compatibility matrix records the exact server-info advertisement and both
  launch/resume results for each supported host build before the sender gate
  is enabled for that build.
- No client-supplied value is persisted or treated as trusted configuration.
- No client-supplied path is opened by the host.
- Payload size, numeric ranges, and JSON structure are bounded before use.
- HTTP verbose request logging redacts the complete
  `clientDisplayCapabilities` value before parsing, case-insensitively and for
  every route, regardless of validity or size. Runtime logs identify the
  selected source and fallback reason without exposing raw profile data.
- Moonlight's verbose URL logging also removes or replaces the complete
  `clientDisplayCapabilities` query value before logging. The client must not
  log the serialized base64/JSON capability merely because it contains no raw
  ICC bytes.

## Testing and verification

### MoonlightQt

- Unit-test profile/MHC2 extraction for valid, absent, truncated, malformed,
  and out-of-range profiles.
- Unit-test Windows/DXGI peak normalization and omission behavior when the
  output is non-PQ/SDR, cloned, detached, ambiguous, stale, or unavailable.
  The v1 allowlist is exactly
  `DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. The source label is
  `client-dxgi-output`; `edid` is only the v1 wire member name.
- Unit-test profile-name resolution from a bare API-returned filename through
  the Windows color directory, including missing, traversal-shaped, oversized,
  and unreadable files. Test canonical directory/file handle comparison with
  case variants, a prefix-boundary sibling, ADS, UNC/device input, junction
  and symlink/reparse replacement during open, directory rename/replacement,
  and a reparse-tagged candidate; every race must omit the calibrated value.
  Keep the retained directory and candidate handles live through the read and
  assert that an injected slow read exceeding 250 ms omits ICC while retaining
  a valid DXGI fallback.
- Unit-test serialization for calibrated-plus-EDID, calibrated-only,
  EDID-only, invalid-calibrated-plus-valid-EDID, valid-calibrated-plus-invalid-
  EDID, and no-data cases. Verify each independent member can fail without
  suppressing the other valid source.
- Unit-test the 4096-byte URL-decoded base64, 3072-byte decoded JSON, and
  12288-byte raw URL-encoded boundaries, including optional padding and
  percent expansion. Exercise raw values of exactly 12288 and 12289 bytes,
  mixed-case and percent-encoded field names, repeated fields, malformed
  percent sequences, and path-shaped values before generic query parsing.
- Verify the launch request includes the new parameter before both launch and
  resume calls only when the host advertises version 1, omits it for GFE/SDR/
  unadvertised hosts, and retains the existing HDR query fields. Verify the
  host `/serverinfo` advertisement is ignored by old clients. Verify
  `NvComputer` refreshes version 1 to absent, zero, malformed, and unknown
  version without retaining the previous advertisement, and that the field is
  excluded from persistence/serialized equality. Cover copy/assignment,
  failed refresh, host switching, and a persistence round trip with explicit
  zero/nonzero expected values.
- Verify multi-monitor mapping through the pre-launch QQuickWindow/hidden-test
  window path, including negative-coordinate displays, duplicate/mirrored
  geometry, hidden-window placement mismatch, and unresolved-output omission.
- Verify missing modern Windows color APIs and pre-20348 behavior leave
  Moonlight loadable and omit calibrated data while preserving the DXGI/global
  fallback.
- Verify a post-read display-identity mutation, hidden-window relocation, or
  clone/hotplug transition invalidates the snapshot rather than reporting the
  wrong output. Mutate each captured source/target/DXGI identity and descriptor
  field independently.
- Verify the GUI-owned probe-window lease survives initialization until
  `Session::start()`, final validation runs on the GUI thread, the worker
  receives only an immutable value snapshot, and initialization failure,
  cancellation, repeated finalization, probe-window recreation, or a screen
  mutation between initialization and worker creation causes invalidation,
  recollection, or omission.

### Vibepollo

- Unit-test capability parsing, payload limits, unknown versions, malformed
  JSON, non-finite values, strict base64url handling, and range rejection.
- Unit-test effective-source precedence, including explicit numeric override,
  non-empty host ICC profile with valid and invalid MHC2 data, client calibrated
  profile, client EDID, and global default. A selected host profile must never
  fall through to client data.
- Unit-test that client-derived values are ignored for SDR launch and resume
  requests on both idle-owner and active-join paths, while explicit host
  overrides retain their existing semantics.
- Assert the exact `enable_hdr && !prefer_sdr_10bit && !force_sdr` predicate
  for contradictory `hdrMode`, client preference, and force-on/force-off
  combinations on launch and resume.
- Unit-test runtime override creation to ensure the selected peak reaches the
  existing `rtx_hdr_peak_brightness` field.
- Unit-test that automatic client data does not trigger host physical ICC
  association or persistence.
- Unit-test startup cloning and shared-session ownership so a second client
  cannot retarget process-global runtime state.
- Unit-test paused-application ownership: a disconnected owner's retained map
  is restored after a failed replacement, a successful idle resume replaces it,
  and application termination performs the eventual clear. Inject failures
  after policy resolution, runtime publication, virtual-display preparation,
  and process launch; assert both map and owner record rollback.
- Run a repository-wide inventory over `src/**/*.cpp` and `src/**/*.h` for
  runtime-map mutation, classify every result, and fail the build/check when a
  feature-key writer is outside the owner manager or low-level definitions.
  Pair this with stale-generation rejection tests for every inventoried
  launch, resume, WebRTC, teardown, termination, and live-edit path.
- Exercise the centralized `stream.cpp:2705-2808` finalizer and each RTSP/
  WebRTC caller for pending expiry, first publication, timeout, teardown,
  paused retention, replacement, and termination; assert every path invokes a
  manager transition and preserves map-plus-owner consistency.
- Unit-test request logging redaction for the custom capability parameter.
- Unit-test the common raw-query adapter: it removes the capability field
  before every generic parser/logger and every `nvhttp.cpp` or
  `confighttp*.cpp` route callback,
  preserves unrelated fields, bounds raw value scanning without allocating an
  oversized decoded value, and records invalid/repeated/malformed fields
  without rejecting the launch. A source check proves every route is wrapped,
  no callback calls the raw request parser directly, and the configuration
  web-UI path always receives an empty capability snapshot.
- Test Sunshine virtual-display creation with a client calibrated peak and
  verify the requested HDR display-reported peak. Test SudoVDA separately and
  verify the explicit runtime-only diagnostic without claiming exact virtual
  display inheritance. Assert the resolved value reaches the existing
  `rtx_hdr_peak_brightness`/RTSP RTX HDR consumer while the SudoVDA
  `createVirtualDisplay` path receives no new luminance argument and continues
  to discard only its existing `hdr_requested` parameter.

### End-to-end Windows smoke test

With a calibrated HDR Windows display and the modified Windows Moonlight
client, start an HDR stream and verify the host log reports
`client-icc-mhc2` with the expected peak before Sunshine virtual-display
creation. Remove or disable the active calibration profile and verify the same
client falls back to `client-dxgi-output`. Finally, use an explicit host profile or
numeric override and verify it remains authoritative, including when the host
profile's MHC2 peak is unreadable. Run the same matrix against SudoVDA and
record whether the backend provides exact virtual-display inheritance or the
documented runtime-only fallback.
Enable Moonlight verbose logging during one launch/resume and assert the
serialized capability value is absent from the logged URL.

## Expected result

A calibrated Windows laptop can connect to a Sunshine-backed Vibepollo host
without per-device manual tuning. The host receives the calibrated display
peak before it creates the virtual display and uses that target through the
existing HDR override chain. Steam Deck, iPad, older Moonlight, non-Sunshine
virtual-display backends without the new driver field, and unusual
multi-display clients retain the manual host-profile and numeric-override
paths instead of being forced into an unreliable automatic guess.

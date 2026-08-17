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
sender rejects the first limit before request construction; the host enforces
the raw limit at query extraction, the decoded-base64 limit before decoding,
and the decoded-JSON limit before parsing. The encoded value is unpadded
URL-safe base64 (`A-Z`, `a-z`, `0-9`, `-`, `_`); optional terminal `=` padding
is accepted, but whitespace, invalid alphabet characters, and excess padding
are rejected. The host also rejects unknown versions without failing launch,
rejects non-finite values, and validates strict JSON types before using any
field. A malformed or unsupported payload is equivalent to no client
capability data and must not prevent the stream from starting.

The client sends this parameter only for an HDR-capable Windows Desktop
session, only for non-GFE hosts, and for both `launch` and `resume` requests.
If the output cannot be resolved, it sends no capability payload rather than
risking data from the wrong monitor.

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
   `CPST_EXTENDED_DISPLAY_COLOR_MODE`. The returned profile name is resolved
   through the Windows color directory (`GetColorDirectoryW` or an equivalent
   canonical helper), constrained to that directory, and then read only
   locally. Resolve the canonical directory and final file path with
   case-insensitive containment, reject UNC/device/alternate-stream,
   absolute, traversal-shaped, missing, unreadable, or reparse-point escapes,
   then open read-only with `CreateFileW`, verify the handle's
   `GetFinalPathNameByHandleW` remains inside the directory, and perform one
   bounded read. Apply the same 32 MiB/structural checks as the host and parse
   MHC2. Free the API-owned name with `LocalFree`. Missing exports,
   unsupported OS builds, missing profiles, `STANDARD`-only profiles, and
   parse failures produce no calibrated value.
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
chosen for the payload without logging the full serialized value.

The immutable capability snapshot is passed into `NvHTTP::startApp()` before
the asynchronous connection thread begins. The host receives the data before
virtual-display creation and capture preparation. A later display move is
outside v1 and leaves the launch snapshot unchanged.

Because the initialization test window is destroyed before `Session::start()`
starts `AsyncConnectionStartThread`, the client performs one final
GUI-thread display-identity validation immediately before that handoff,
retaining or recreating the hidden test window as needed. A changed screen,
SDL/DXGI binding, or display descriptor clears the snapshot (and may recollect
from the newly selected display) before `NvHTTP::startApp()`. The remaining
race after the worker handoff is a documented v1 limitation; no later
renegotiation is attempted.

## Vibepollo host integration

The host parses `clientDisplayCapabilities` for both launch verbs before
computing runtime overrides and stores the validated result on
`launch_session_t`. It must remain separate from the persistent
`crypto::named_device_t::hdr_profile` field so client data cannot silently
become a saved host configuration. `clone_for_startup()` must copy the new
field explicitly.

Client-derived policy uses one canonical effective-HDR resolver shared by
session construction and peak policy. Its current result is the existing
`rtsp_stream::effective_hdr_requested(const launch_session_t&)` predicate,
`enable_hdr && !prefer_sdr_10bit && !force_sdr`, after `hdrMode`, per-client
10-bit-SDR preference, and the Windows `hdr_request_override` force-on/
force-off/automatic setting have been applied. Launch and resume must resolve
this seed once before peak selection and pass the same result into the session
builder and policy; neither may independently reinterpret `hdrMode`. Backend
display mode must not substitute a different HDR predicate.

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
the existing `stream_lifecycle_gate` as its single mutation lock. A
generation-checked lease stores the proposed owner token, prior map, prior
owner, candidate map, and phase (`provisional`, `awaiting-stream`, `active`,
or `retained-paused`). An idle launch/resume begins a lease and publishes the
candidate map; HTTP success leaves it awaiting stream ownership rather than
committing it. First RTSP/WebRTC ownership publication commits the matching
generation. Synchronous errors, pending-session cancellation/expiry,
virtual-display failure, and asynchronous stream-start failure roll back the
matching lease under the gate. A stale rollback cannot overwrite a newer
generation. A join copies the active owner's target without opening a lease.
Last-stream teardown marks a still-running application `retained-paused`
without clearing the map; a later idle request replaces it transactionally.
`proc_t::terminate()` clears the runtime map and owner state together.

All writes that can change `rtx_hdr_peak_brightness` must use this manager:
launch/resume, WebRTC's first-capture path, application termination, and live
RTX HDR edits. An explicit live numeric edit updates owner provenance
atomically; unrelated runtime keys remain under the existing config API.

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
  parameters as a map and reads known keys without rejecting unknown keys.
  This exact baseline is the v1 compatibility guarantee; the plan does not
  generalize that result to unverified deployed versions. GFE remains
  explicitly excluded.
- No client-supplied value is persisted or treated as trusted configuration.
- No client-supplied path is opened by the host.
- Payload size, numeric ranges, and JSON structure are bounded before use.
- HTTP verbose request logging redacts the complete
  `clientDisplayCapabilities` value before parsing, case-insensitively and for
  every route, regardless of validity or size. Runtime logs identify the
  selected source and fallback reason without exposing raw profile data.

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
  and unreadable files.
- Unit-test serialization for calibrated-plus-EDID, calibrated-only,
  EDID-only, invalid-calibrated-plus-valid-EDID, valid-calibrated-plus-invalid-
  EDID, and no-data cases. Verify each independent member can fail without
  suppressing the other valid source.
- Unit-test the 4096-byte URL-decoded base64, 3072-byte decoded JSON, and
  12288-byte raw URL-encoded boundaries, including optional padding and
  percent expansion.
- Verify the launch request includes the new parameter before both launch and
  resume calls for non-GFE HDR sessions, omits it for GFE/SDR sessions, and
  retains the existing HDR query fields.
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
- Unit-test request logging redaction for the custom capability parameter.
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

## Expected result

A calibrated Windows laptop can connect to a Sunshine-backed Vibepollo host
without per-device manual tuning. The host receives the calibrated display
peak before it creates the virtual display and uses that target through the
existing HDR override chain. Steam Deck, iPad, older Moonlight, non-Sunshine
virtual-display backends without the new driver field, and unusual
multi-display clients retain the manual host-profile and numeric-override
paths instead of being forced into an unreliable automatic guess.

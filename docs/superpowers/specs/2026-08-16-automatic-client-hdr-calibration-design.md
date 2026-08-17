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

The host must impose a small maximum decoded payload size, reject unknown
versions without failing launch, reject non-finite values, and validate strict
JSON types before using any field. The encoded value is unpadded URL-safe
base64 (`A-Z`, `a-z`, `0-9`, `-`, `_`); optional terminal `=` padding is
accepted, but whitespace, invalid alphabet characters, excess padding, and
payloads over 4096 encoded bytes are rejected. A malformed or unsupported
payload is equivalent to no client capability data and must not prevent the
stream from starting.

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
2. Map that display's GDI/DXGI identity to a `DISPLAYCONFIG` source adapter LUID
   and source ID, reusing the existing mapping approach in
   `d3d11va.cpp`. Dynamically resolve the Windows ColorProfile APIs from
   `mscms.dll` so older supported Windows versions remain loadable when those
   exports are unavailable.
3. Query `ColorProfileGetDisplayUserScope()` and
   `ColorProfileGetDisplayDefault()` with `CPT_ICC` and
   `CPST_EXTENDED_DISPLAY_COLOR_MODE`. Read the returned profile only locally,
   bound the file to the same 32 MiB/structural checks as the host, and parse
   its MHC2 peak. Missing exports, unsupported OS builds, missing profiles,
   `STANDARD`-only profiles, and parse failures produce no calibrated value.
4. Query the selected DXGI output through
   `IDXGIOutput6::GetDesc1()`. Populate the wire object's `edid` member with
   `source: "dxgi-output"` only when the descriptor is current, uniquely
   mapped, HDR-capable, and has a finite positive `MaxLuminance`; otherwise
   omit it. This is a Windows/DXGI display-reported fallback, not a claim of
   raw EDID provenance.
5. Serialize only the normalized peak values; never send the client file path,
   profile filename, monitor serial, adapter identifiers, or raw ICC/EDID
   bytes.

The collector must tolerate missing profiles, displays without HDR support,
profile parse failures, and Windows API races. It should log the local source
chosen for the payload without logging the full serialized value.

The immutable capability snapshot is passed into `NvHTTP::startApp()` before
the asynchronous connection thread begins. The host receives the data before
virtual-display creation and capture preparation. A later display move is
outside v1 and leaves the launch snapshot unchanged.

## Vibepollo host integration

The host parses `clientDisplayCapabilities` for both launch verbs before
computing runtime overrides and stores the validated result on
`launch_session_t`. It must remain separate from the persistent
`crypto::named_device_t::hdr_profile` field so client data cannot silently
become a saved host configuration. `clone_for_startup()` must copy the new
field explicitly.

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
- RTX HDR runtime initialization; and
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
the already-published runtime value. A failed first launch must release or
restore the provisional override, and final-session teardown must clear it
through the existing runtime-override lifecycle. Per-session capability fields
may still be retained for logs, but they do not retarget global
capture/encoder state.

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
- Old Vibepollo hosts must ignore the unknown launch parameter from a newer
  client; the implementation plan must source-check the pre-feature query
  parser and add a concrete gate if any supported historical host rejects it.
- No client-supplied value is persisted or treated as trusted configuration.
- No client-supplied path is opened by the host.
- Payload size, numeric ranges, and JSON structure are bounded before use.
- HTTP verbose request logging redacts the complete
  `clientDisplayCapabilities` value. Runtime logs identify the selected source
  and fallback reason without exposing raw profile data.

## Testing and verification

### MoonlightQt

- Unit-test profile/MHC2 extraction for valid, absent, truncated, malformed,
  and out-of-range profiles.
- Unit-test Windows/DXGI peak normalization and omission behavior when the
  output is SDR, cloned, detached, ambiguous, stale, or unavailable. The
  source label is `client-dxgi-output`; `edid` is only the v1 wire member name.
- Unit-test serialization for calibrated-plus-EDID, calibrated-only,
  EDID-only, and no-data cases.
- Verify the launch request includes the new parameter before both launch and
  resume calls for non-GFE HDR sessions, omits it for GFE/SDR sessions, and
  retains the existing HDR query fields.
- Verify multi-monitor mapping through the pre-launch QQuickWindow/hidden-test
  window path, including negative-coordinate displays, duplicate/mirrored
  geometry, hidden-window placement mismatch, and unresolved-output omission.
- Verify missing modern Windows color APIs and pre-20348 behavior leave
  Moonlight loadable and omit calibrated data while preserving the DXGI/global
  fallback.

### Vibepollo

- Unit-test capability parsing, payload limits, unknown versions, malformed
  JSON, non-finite values, strict base64url handling, and range rejection.
- Unit-test effective-source precedence, including explicit numeric override,
  non-empty host ICC profile with valid and invalid MHC2 data, client calibrated
  profile, client EDID, and global default. A selected host profile must never
  fall through to client data.
- Unit-test runtime override creation to ensure the selected peak reaches the
  existing `rtx_hdr_peak_brightness` field.
- Unit-test that automatic client data does not trigger host physical ICC
  association or persistence.
- Unit-test startup cloning and shared-session ownership so a second client
  cannot retarget process-global runtime state.
- Unit-test request logging redaction for the custom capability parameter.
- Test Sunshine virtual-display creation with a client calibrated peak and
  verify the requested HDR display-reported peak. Test SudoVDA separately and
  verify the explicit runtime-only diagnostic without claiming exact virtual
  display inheritance.

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

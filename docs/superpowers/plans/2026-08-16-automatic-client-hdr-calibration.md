# Automatic Client HDR Calibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans.

**Goal:** Have Windows Desktop Moonlight report the already-calibrated HDR peak of the display selected for the stream, and have Vibepollo use that value to configure the host display/runtime when no explicit host override is active, while preserving manual host profile tuning for clients such as Steam Deck and iPad.

**Architecture:** Moonlight collects a bounded, normalized capability snapshot before /launch or /resume from the hidden display-selection window that already exists during session initialization. It sends a URL-safe base64 JSON value containing a calibrated Windows ICC/MHC2 peak and, independently, a Windows/DXGI display-reported peak. The wire field remains named edid for v1 compatibility, but source dxgi-output does not claim raw EDID provenance. Vibepollo parses the value before calculating runtime overrides, applies one deterministic precedence policy, stores the capability snapshot and resolved result on the launch session, and copies both through the existing startup clone contract. Sunshine receives the resolved peak through its existing HDR virtual-display request. SudoVDA remains runtime-only in v1 because its current wrapper/driver contract does not accept HDR luminance; redacted runtime logs and static UI copy state that limitation without adding a live status API.

**Tech Stack:** C++23/nlohmann JSON/CMake/GoogleTest on Vibepollo; C++17/Qt 6/QJsonDocument/QtTest/qmake on MoonlightQt; Windows ColorProfile and DISPLAYCONFIG APIs with optional mscms.dll resolution, MHC2 parsing, DXGI IDXGIOutput6::GetDesc1, SDL display/window mapping, existing Sunshine HDR configuration, and existing HTTP launch/resume query plumbing.

## Global Constraints

- Work in the authoritative repositories:
  - Host: C:\Users\Matt\Documents\Codex\2026-08-15\vibepollo-atmos\Vibepollo
  - Client: C:\Users\Matt\Documents\Codex\2026-08-15\vibepollo-atmos\MoonlightQt
- Preserve the existing HDR query fields and all non-Windows/GFE behavior.
- v1 transports only a normalized peak luminance in nits. Do not transmit raw ICC files, client paths, profile names, primaries, black level, minimum luminance, or guessed values that the current host parser cannot consume.
- The wire edid object is a compatibility name for the dxgi-output fallback. User-facing logs and copy must call it Windows/DXGI-reported unless raw EDID provenance is actually established.
- Use this precedence for the effective peak:
  1. Explicit numeric per-client/per-app override.
  2. A non-empty explicit host HDR profile, using its valid MHC2 peak.
  3. The client’s active Windows ICC/MHC2 calibrated peak.
  4. The client’s DXGI/EDID-derived peak.
  5. The existing global/default behavior.
- A selected but missing, unreadable, malformed, or unsupported host profile suppresses automatic client values and retains the existing global fallback. It must never silently fall through to a client calibration.
- Client-derived values are usable only when the host's final effective HDR request is enabled; host policy must reject them for SDR launch/resume even if the sender supplied a valid payload.
- A first active session owns process-global runtime HDR configuration. Joining launch/resume requests inherit the active target and cannot retarget it with a different client peak.
- Use the existing launch_request_mutex and stream_lifecycle_gate as the owner-selection seam. Do not introduce a second unsynchronized process-global HDR owner.
- Capability collection must fail closed when the selected QScreen cannot be mapped uniquely to one SDL/DXGI output or when the hidden window lands on a different output. The existing display-0 rendering fallback must never become a reported capability.
- The Windows display identity is a complete tuple: source adapter LUID/source ID for GDI lookup, target adapter LUID/source ID for ColorProfile APIs, and the mapped DXGI adapter/output identity.
- Client capability parsing and transport failures are fail-soft: old clients and malformed values must still launch with existing behavior.
- The capability field must be bounded before decoding/parsing, URL-safe, redacted from verbose request logs, and sent on both /launch and /resume only for non-GFE hosts.
- Add focused automated tests for every pure parser/policy/clone contract. Add Windows collection and host-driver behavior to the end-to-end checklist where the environment is required.
- v1 exposes source/fallback information through redacted runtime logs only; settings pages explain policy statically and do not claim live per-session status.
- Do not expand the SudoVDA driver protocol, rewrite source-driven native HDR mastering metadata, or add persistent per-client calibration storage in this change.

---

## 1. Lock the cross-repository wire and policy contracts with failing tests

**Files:**

- Add Vibepollo/tests/unit/test_client_hdr_capabilities.cpp.
- Add Vibepollo/tests/unit/test_hdr_peak_policy.cpp.
- Update Vibepollo/tests/CMakeLists.txt.
- Add MoonlightQt/tests/hdr/hdr.pro.
- Add MoonlightQt/tests/hdr/tst_clientdisplaycapabilities.cpp.
- Update MoonlightQt/tests/tests.pro.

**Work:**

1. Define the v1 payload fixture exactly as the following JSON:

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

2. Add host-side tests for:
   - valid calibrated-only, EDID-only, and both-source payloads;
   - unpadded and padded base64url;
   - wrong version/platform/source/type;
   - missing, zero, negative, non-finite, and out-of-range peak values;
   - finite fractional peaks such as 999.4, 999.5, and 999.6, with the approved nearest-nit rounding rule;
   - malformed base64/JSON and an encoded value over the maximum accepted size;
   - unknown optional fields being ignored;
   - unresolved, duplicated, mirrored, negative-coordinate, and hidden-window-placement-mismatch display mappings producing no capability parameter;
   - no capability payload producing the existing no-override state.
3. Add policy tests covering every precedence row, including explicit numeric override, valid manual profile, selected unreadable profile, calibrated-only, EDID-only, both client values, and no client values.
4. Add a host startup-clone test fixture that proves the capability snapshot survives launch_session_t::clone_for_startup().
5. Add Moonlight QtTest coverage for deterministic JSON serialization, base64url encoding, omission of absent sources, numeric rounding, maximum encoded length, and fail-closed selected-display resolution.
6. Add injectable Windows API tests for unavailable mscms exports, unsupported OS/profile scope, STANDARD-only versus EXTENDED profile selection, bounded profile reads, and malformed MHC2 data.
7. Register the host tests as narrow component targets using the existing sunshine_register_component conventions, and register the Qt test as an opt-in tests/hdr subdirectory.

**Verification:** Run the new tests first and confirm they fail because the production model/parser/policy does not exist yet. Do not weaken assertions to make the empty implementation pass.

**Commit:** test: define automatic client HDR capability contracts

## 2. Implement the host capability model, bounded parser, and deterministic peak policy

**Files:**

- Add Vibepollo/src/client_hdr_capabilities.h.
- Add Vibepollo/src/client_hdr_capabilities.cpp.
- Add Vibepollo/src/hdr_peak_policy.h.
- Add Vibepollo/src/hdr_peak_policy.cpp.
- Update the host test target source lists in Vibepollo/tests/CMakeLists.txt.

**Work:**

1. Add a small host-owned value type with optional calibrated and DXGI display-reported peak fields, source/version metadata, and a parse result that distinguishes absent, invalid, unsupported, and valid input for logging without exposing raw input.
2. Decode base64url with -/_, accept optional terminal padding, reject invalid characters/padding/oversized encoded input, then parse strict JSON types with nlohmann JSON.
3. Accept only version == 1, platform == "windows-desktop", known source labels, finite numeric peaks in 1..100000 nits, and the documented object shape. Accept fractional JSON numbers and round them to the nearest whole nit using the same rule as the client; clients emit normalized integers. Treat invalid values as an ignored capability rather than an HTTP error.
4. Add a policy function with explicit inputs for final effective HDR request, numeric override, manual-profile-selected state, manual-profile peak validity, client capabilities, and the existing global fallback. Return both the selected peak/source and an explanation enum suitable for diagnostics. Client-derived values are ignored whenever final effective HDR is false.
5. Apply the existing runtime range clamp only at the existing runtime boundary (400..2000 nits) so transport validation and host behavior remain independently testable.
6. Ensure the policy does not use client values when any non-empty manual profile is selected, even if profile parsing fails.

**Verification:** Re-run the host parser/policy tests from Task 1. Confirm malformed/unsupported payloads are ignored, manual-profile failure preserves global fallback, and no policy path invents a peak.

**Commit:** feat: add bounded client HDR capability policy

## 3. Thread the capability snapshot through Vibepollo launch state and runtime ownership

**Files:**

- Update Vibepollo/src/rtsp.h.
- Update Vibepollo/src/rtsp.cpp.
- Update Vibepollo/src/nvhttp.cpp.
- Update Vibepollo/tests/unit/test_rtsp_startup_snapshot.cpp.
- Add or update focused host HTTP/session tests if the existing test seams permit them.

**Work:**

1. Add optional client_hdr_capabilities and hdr_peak_resolution values to launch_session_t. The resolution records reported peak, effective clamped peak, source, fallback reason, and whether the value was inherited from an active shared session; it is runtime state only and never enters crypto::named_device_t.
2. Update launch_session_t::clone_for_startup() explicitly so both new fields are copied; do not rely on aggregate/default copying.
3. Extract and validate clientDisplayCapabilities immediately after query extraction for both handlers, before either runtime-override block. Enforce the encoded-size limit before decoding.
4. Pass the same immutable parsed snapshot into every make_launch_session_from_snapshot call path used by launch and resume. Do not let resume's later app-context seeding reconstruct or overwrite the request snapshot.
5. In launch, merge app/client numeric overrides, evaluate the policy, and publish the resolved peak before config::set_runtime_config_overrides. In resume, perform the same parse/policy ordering before the existing lines 3757-3765 profile/runtime block, then construct the session from that result at line 3819.
6. Use the existing launch_request_mutex and stream_lifecycle_gate acquired by the /launch and /resume routes at nvhttp.cpp:4561-4576 as the atomic owner-selection seam. Compute final effective HDR request before policy selection. While that gate observes no activity, the first or replacement request snapshots the prior runtime map and owner record, evaluates, and publishes one target. If activity is present, the request must preserve the existing runtime target and mark its session result shared-active-session; it must not evaluate a client peak into global overrides. has_stream_session_activity() already includes pending RTSP/WebRTC activity and teardown.
7. Define lifecycle cleanup without assuming final stream teardown clears the map: a failed first or replacement request restores the prior runtime map and owner record; a joining client never clears the owner; when the last stream disconnects while the app remains paused, retain the owner/map for resume; the next truly idle request may replace it transactionally; application termination performs the existing eventual clear. Add the owner record separately from the runtime map so stale-map emptiness is not used as an ownership signal.
8. Keep any selected host profile associated with the session even if its MHC2 read fails, suppress client values in that case, and use the existing global fallback.
9. Update verbose query logging to print only that the capability field was present, absent, or invalid plus its parse reason; never log the base64 or decoded JSON.
10. Keep /launch and /resume behavior symmetric and preserve old-client behavior when the field is missing.

**Verification:** Run host unit/component tests, including separate launch and resume ordering, startup cloning, request compatibility, runtime cleanup, and logging-redaction tests. Add deterministic lifecycle tests for A-start/B-join/A-end/C-join, failed first launch, final teardown, and RTSP/WebRTC activity. Assert that joins inherit the published effective value and that only a later idle request can select a new target.

**Commit:** feat: apply client HDR capabilities to launch sessions

## 4. Connect the resolved peak to the supported Windows host consumer and emit truthful runtime logs

**Files:**

- Update Vibepollo/src/nvhttp.cpp for source/fallback logging and resolved-result publication.
- Update Vibepollo/src/platform/windows/virtual_display.cpp only if a shared testable MHC2 helper is required; do not create a second parser.
- Do not change the SudoVDA driver protocol or add a live status endpoint.
- Update the relevant Windows virtual-display unit tests, including Vibepollo/tests/unit/platform/windows/test_virtual_display_sunshine.cpp.

**Work:**

1. Reuse the existing manual MHC2 peak extraction and existing hdr_max_luminance_nits path for Sunshine. Do not create a second luminance conversion.
2. Emit redacted runtime logs containing only reported peak, effective peak, source, fallback reason, and backend scope. Never log profile paths, names, serialized payloads, or raw identifiers.
3. For SudoVDA, retain the runtime peak where existing RTX HDR behavior consumes it, but log that exact virtual-display matching is unavailable in v1 because the current wrapper discards hdr_requested.
4. Do not claim that native HDR streams have been retuned: their content/mastering metadata remains source-driven in this change.
5. Test that Sunshine receives the clamped resolved peak, that manual profiles remain authoritative, that failed first launches do not leave stale overrides, and that SudoVDA does not receive an accidental unsupported request.

**Verification:** Run the focused Windows virtual-display tests and inspect the generated request/log values. If a real driver is available, capture Sunshine virtual-display HDR readback and compare it with the resolved peak; separately record the SudoVDA limitation. Confirm no new UI/API status surface is introduced.

**Commit:** feat: route resolved HDR peak through supported display paths

## 5. Collect calibrated ICC/MHC2 and DXGI values in Moonlight before launch

**Files:**

- Add MoonlightQt/app/backend/clientdisplaycapabilities.h.
- Add MoonlightQt/app/backend/clientdisplaycapabilities.cpp.
- Add MoonlightQt/app/backend/clientdisplaycapabilities_win.h.
- Add MoonlightQt/app/backend/clientdisplaycapabilities_win.cpp.
- Update MoonlightQt/app/app.pro.
- Update MoonlightQt/app/streaming/session.h.
- Update MoonlightQt/app/streaming/session.cpp.

**Work:**

1. Keep the wire model/serializer platform-neutral and put Windows display/profile discovery behind the Windows implementation boundary. Non-Windows builds must compile with no-op collection and no new Windows dependency.
2. Add a selected-display resolver that returns success only when the QScreen geometry maps to one SDL display. Preserve the existing display-0 fallback for rendering if desired, but pass an unresolved result to the capability collector so it emits no data. After SDL_SetWindowPosition(testWindow, x, y), require SDL_GetWindowDisplayIndex(testWindow) to equal the resolved index before collecting.
3. Reuse the DISPLAYCONFIG mapping pattern already present in app/streaming/video/ffmpeg-renderers/d3d11va.cpp: query active paths, match the output GDI name, require one unique path, and retain the complete identity. Use sourceInfo.adapterId/sourceInfo.id for the GDI source lookup, but pass targetInfo.adapterId as ColorProfile `targetAdapterID` and sourceInfo.id as `sourceID`.
4. On Windows, dynamically resolve ColorProfileGetDisplayUserScope and ColorProfileGetDisplayDefault from mscms.dll. Query the selected scope and request CPT_ICC with CPST_EXTENDED_DISPLAY_COLOR_MODE. Free the returned profile name with LocalFree. Resolve a bare returned filename through GetColorDirectoryW (or an equivalent canonical Windows color-directory helper), constrain the result to that directory, reject empty/absolute/traversal-shaped names, then apply the 32 MiB and MHC2 checks. If exports, OS support, scope, subtype, file, or profile parsing are unavailable, omit calibrated and continue to the display-reported fallback.
5. Apply the host's proven MHC2 bounds and fixed-point conversion in a client-local helper: maximum 32 MiB file, valid MHC2 tag structure, finite positive peak in 1..100000, nearest-nit rounding. Do not use WcsGetDefaultColorProfile or GetICMProfile as the HDR authority.
6. Query the mapped DXGI output through IDXGIOutput6::GetDesc1().MaxLuminance and normalize it to an integer nit value only when the output is current, uniquely mapped, `AttachedToDesktop`, finite/positive, and `ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Reject every other color space, including SDR, scRGB, studio-PQ, YCbCr, clone/default, detached, or stale output data. Keep the JSON member named edid for v1 compatibility but label its source dxgi-output.
7. Capture the QScreen/SDL/DISPLAYCONFIG/DXGI identity before profile and output reads, then revalidate the hidden-window display index and complete identity tuple afterward. Any mutation from hotplug, clone transition, relocation, or output replacement omits the capability snapshot.
8. Emit calibrated and display-reported values independently when available. A calibrated value is not replaced by the display-reported fallback, and the fallback remains useful if the ICC/MHC2 association is absent or unreadable.
9. Serialize only the normalized values and source labels. Never send the physical ICC path, profile bytes, monitor name, adapter/source identifiers, EDID blob, or other identifying data.
10. Store only the encoded immutable capability string on the session until the asynchronous launch request consumes it. Ensure collection occurs on the GUI/display thread and does not block the launch worker on repeated profile scans.
11. Add the new sources/headers to app.pro and link the required DXGI/Windows libraries; resolve mscms.dll functions dynamically rather than requiring a new hard link that breaks older supported Windows versions.

**Verification:** Run the Moonlight QtTest suite with synthetic MHC2/DXGI/API fixtures. On Windows, exercise one monitor, a multi-monitor layout with negative coordinates, null/unmatched/duplicate QScreen mapping, hidden-window placement mismatch, ICC missing, STANDARD-only profile, missing mscms exports, pre-20348 behavior, malformed MHC2, DXGI mapping failure, SDR/clone/default output, and a display with both sources. Confirm the payload follows the selected stream display rather than whichever monitor owns the process window, and is omitted for every unresolved/ambiguous case.

**Commit:** feat: collect calibrated client HDR display capabilities

## 6. Send the capability on Moonlight launch and resume without changing legacy hosts

**Files:**

- Update MoonlightQt/app/backend/nvhttp.h.
- Update MoonlightQt/app/backend/nvhttp.cpp.
- Update MoonlightQt/app/streaming/session.cpp call sites.
- Update the Moonlight QtTest fixture if request construction is covered there.

**Work:**

1. Extend NvHTTP::startApp with an optional encoded capability argument whose default preserves existing callers.
2. Append clientDisplayCapabilities=<percent-encoded-base64url> to both /launch and /resume query construction when the request is HDR, the host is not GFE, and the encoded value is non-empty and within the client-side bound.
3. Preserve all existing hdrMode, clientHdrCapVersion, clientHdrCapSupportedFlagsInUint32, clientHdrCapMetaDataId, and clientHdrCapDisplayData parameters unchanged.
4. Omit the custom field for GFE and when collection yields no usable value. The supported pre-feature Vibepollo parser baseline `f8c4ac2762b351457ee57aef0863655d18b936e4` ignores unknown query keys, so v1 does not defer an abstract legacy-host gate or change the request architecture.
5. Add request serialization tests for both verbs, HDR/non-HDR, GFE/non-GFE, empty payload, and percent-encoding.

**Verification:** Build the Moonlight app and opt-in tests, run the request tests, and inspect captured URLs to confirm no raw JSON/path/ICC data is present and no legacy query field changes.

**Commit:** feat: transmit client HDR capabilities on launch and resume

## 7. Update UI, documentation, and localization for the new automatic mode

**Files:**

- Update Vibepollo/src_assets/common/assets/web/components/devices/ClientSettingsEditor.vue.
- Update Vibepollo/src_assets/common/assets/web-legacy/views/ClientManagementView.vue.
- Update the English locale files used by both web clients:
  - Vibepollo/src_assets/common/assets/web/public/assets/locale/en.json
  - Vibepollo/src_assets/common/assets/web/public/assets/locale/en_US.json
  - Vibepollo/src_assets/common/assets/web/public/assets/locale/en_GB.json
  - Vibepollo/src_assets/common/assets/web-legacy/public/assets/locale/en.json
  - Vibepollo/src_assets/common/assets/web-legacy/public/assets/locale/en_US.json
  - Vibepollo/src_assets/common/assets/web-legacy/public/assets/locale/en_GB.json
- Update Vibepollo/docs/getting_started.md.
- Update locale consistency tests only if new keys require an explicit catalog assertion.

**Work:**

1. Rename/reword “Automatic (no override)” so it clearly means “use the client-reported calibrated display when available; otherwise use existing host defaults.”
2. Keep the manual HDR profile selector and explain that it intentionally overrides client-reported calibration for fixed-display/handheld/tablet clients or manual tuning.
3. Explain that client ICC data is normalized to display specifications; Vibepollo never receives or stores the physical ICC file. Runtime source/fallback details are available only in redacted host logs in v1.
4. Replace the getting-started instruction that suggests calibrating the host while the stream is running with the new workflow and preserve the existing manual-profile instructions.
5. Keep all user-facing copy truthful about Sunshine versus SudoVDA, Windows/DXGI-reported fallback values, and native HDR metadata.

**Verification:** Run locale consistency tests, build the web assets through the existing project command, and inspect both current web UIs at the client-settings view for wrapping, disabled/selected states, and manual-profile discoverability.

**Commit:** docs: explain automatic client HDR calibration

## 8. Run cross-layer verification and perform the end-to-end compatibility matrix

**Files:** No new production files; update tests or documentation only when a verification gap is found.

**Work:**

1. Host:
   - configure/build with cmake -S . -B build -G Ninja -DBUILD_TESTS=ON;
   - run the focused capability/policy/startup/virtual-display tests;
   - run the complete configured ctest --test-dir build --output-on-failure;
   - run git diff --check.
2. Moonlight:
   - run powershell .\setup-deps.ps1 only if the existing dependency directory is absent;
   - configure with qmake6 "CONFIG+=tests" moonlight-qt.pro;
   - build with the existing Qt/MSVC build command (nmake from the configured Qt developer prompt);
   - run the HDR QtTest executable and the existing VRR tests;
   - run git diff --check.
3. Compatibility:
   - old Moonlight to new Vibepollo: no custom field, existing behavior;
   - new Moonlight to the pinned pre-feature Vibepollo host baseline `f8c4ac2762b351457ee57aef0863655d18b936e4`: verify the map-based launch/resume parser ignores the unknown field, and freeze that result as the v1 compatibility contract;
   - GFE launch/resume: no custom field;
   - malformed, oversized, unknown-version, wrong-platform, and non-finite payloads: invalid values ignored; in-range fractional values are rounded identically by host and client; valid client values on SDR requests are ignored by host policy;
   - manual valid/missing/malformed profile: manual selection suppresses client values and preserves global fallback on failure;
   - calibrated ICC plus Windows/DXGI display-reported value: ICC wins;
   - display-reported value only: it is used only for a current, unique HDR output;
   - unresolved, mirrored, SDR, detached, or clone/default output: no client capability is sent;
   - multiple viewers: first idle request owns the target, later viewers inherit it, failed first/replacement launches restore the prior owner, paused-app disconnect retains the map, and a later idle request can replace it transactionally;
   - Sunshine: resolved peak reaches virtual-display HDR request;
   - SudoVDA: runtime-only behavior is reported accurately in redacted logs and static documentation;
   - native HDR and RTX HDR: verify only the existing consumers are affected.
4. Capture launch/resume logs with verbose logging enabled and confirm capability payload contents are never logged.
5. Compare the final diff against the committed design spec and this plan. Remove any implementation or test artifacts not required by the feature.

**Verification target:** All focused and full tests pass in the available environments. Each hardware/driver-only row is recorded with one of `observed`, `unavailable`, or `not-applicable`; `unavailable` is an environment limitation, never a passing result.

**Commit:** test: verify automatic client HDR calibration integration

## 9. Final gap review and handoff

1. Check every requirement in docs/superpowers/specs/2026-08-16-automatic-client-hdr-calibration-design.md against the actual diff, tests, UI copy, and build output.
2. Confirm there is no raw ICC/profile/EDID data crossing the wire, no persistence of client display data, no unintended GFE query change, and no SudoVDA protocol claim.
3. Confirm the host and client repositories contain only intentional changes, with separate scoped commits and no credentials/generated local state.
4. Report the effective precedence, supported driver scope, verification commands/results, and any hardware-only limitations.

# Automatic Client HDR Calibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans.

**Goal:** Have Windows Desktop Moonlight report the already-calibrated HDR peak of the display selected for the stream, and have Vibepollo use that value to configure the host display/runtime when no explicit host override is active, while preserving manual host profile tuning for clients such as Steam Deck and iPad.

**Architecture:** Moonlight collects a bounded, normalized capability snapshot before /launch or /resume from the hidden display-selection window that already exists during session initialization. It sends a URL-safe base64 JSON value containing a calibrated Windows ICC/MHC2 peak and, independently, a DXGI/EDID-derived peak. Vibepollo parses the value before calculating runtime overrides, applies one deterministic precedence policy, stores the capability snapshot on the launch session, and copies it through the existing startup clone contract. Sunshine receives the resolved peak through its existing HDR virtual-display request. SudoVDA remains runtime-only in v1 because its current wrapper/driver contract does not accept HDR luminance; the UI and diagnostics state that limitation instead of claiming an exact virtual-display match.

**Tech Stack:** C++23/nlohmann JSON/CMake/GoogleTest on Vibepollo; C++17/Qt 6/QJsonDocument/QtTest/qmake on MoonlightQt; Windows ICC/MHC2 APIs, DXGI IDXGIOutput6::GetDesc1, SDL display/window mapping, existing Sunshine HDR configuration, and existing HTTP launch/resume query plumbing.

## Global Constraints

- Work in the authoritative repositories:
  - Host: C:\Users\Matt\Documents\Codex\2026-08-15\vibepollo-atmos\Vibepollo
  - Client: C:\Users\Matt\Documents\Codex\2026-08-15\vibepollo-atmos\MoonlightQt
- Preserve the existing HDR query fields and all non-Windows/GFE behavior.
- v1 transports only a normalized peak luminance in nits. Do not transmit raw ICC files, client paths, profile names, primaries, black level, minimum luminance, or guessed values that the current host parser cannot consume.
- Use this precedence for the effective peak:
  1. Explicit numeric per-client/per-app override.
  2. A non-empty explicit host HDR profile, using its valid MHC2 peak.
  3. The client’s active Windows ICC/MHC2 calibrated peak.
  4. The client’s DXGI/EDID-derived peak.
  5. The existing global/default behavior.
- A selected but missing, unreadable, malformed, or unsupported host profile suppresses automatic client values and retains the existing global fallback. It must never silently fall through to a client calibration.
- A first active session owns process-global runtime HDR configuration. Joining launch/resume requests inherit the active target and cannot retarget it with a different client peak.
- Client capability parsing and transport failures are fail-soft: old clients and malformed values must still launch with existing behavior.
- The capability field must be bounded before decoding/parsing, URL-safe, redacted from verbose request logs, and sent on both /launch and /resume only for non-GFE hosts.
- Add focused automated tests for every pure parser/policy/clone contract. Add Windows collection and host-driver behavior to the end-to-end checklist where the environment is required.
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
   - missing, zero, negative, fractional, non-finite, and out-of-range peak values;
   - malformed base64/JSON and an encoded value over the maximum accepted size;
   - unknown optional fields being ignored;
   - no capability payload producing the existing no-override state.
3. Add policy tests covering every precedence row, including explicit numeric override, valid manual profile, selected unreadable profile, calibrated-only, EDID-only, both client values, and no client values.
4. Add a host startup-clone test fixture that proves the capability snapshot survives launch_session_t::clone_for_startup().
5. Add Moonlight QtTest coverage for deterministic JSON serialization, base64url encoding, omission of absent sources, numeric rounding, and maximum encoded length.
6. Register the host tests as narrow component targets using the existing sunshine_register_component conventions, and register the Qt test as an opt-in tests/hdr subdirectory.

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

1. Add a small host-owned value type with optional calibrated and EDID peak fields, source/version metadata, and a parse result that distinguishes absent, invalid, unsupported, and valid input for logging without exposing raw input.
2. Decode base64url with -/_, accept optional terminal padding, reject invalid characters/padding/oversized encoded input, then parse strict JSON types with nlohmann JSON.
3. Accept only version == 1, platform == "windows-desktop", known source labels, integer-safe finite peaks in 1..100000 nits, and the documented object shape. Treat invalid values as an ignored capability rather than an HTTP error.
4. Add a policy function with explicit inputs for numeric override, manual-profile-selected state, manual-profile peak validity, client capabilities, and the existing global fallback. Return both the selected peak/source and an explanation enum suitable for diagnostics.
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

1. Add an optional client_hdr_capabilities value to launch_session_t.
2. Update launch_session_t::clone_for_startup() explicitly so the new field is copied; do not rely on aggregate/default copying.
3. Parse clientDisplayCapabilities immediately after request query extraction and before runtime override calculation. Enforce the encoded-size limit before decoding.
4. Pass the parsed immutable snapshot into every make_launch_session_from_snapshot call path used by launch, resume, and other startup entry points.
5. Apply the policy before config::set_runtime_config_overrides. Preserve the current order in which client/application numeric overrides are merged, then let the selected peak affect only the existing HDR runtime field.
6. Implement first-active-session ownership: when no session is active, the first valid launch selects and applies the resolved target; when a session is active, later requests retain the active runtime target and record that their client capability was not allowed to retarget shared state.
7. Keep the selected host profile associated with the session even if its MHC2 read fails, and use the global fallback in that case.
8. Update verbose query logging to print only that the capability field was present, absent, or invalid plus its parse reason; never log the base64 or decoded JSON.
9. Keep /launch and /resume behavior symmetric and preserve old-client behavior when the field is missing.

**Verification:** Run host unit/component tests, including startup cloning and request compatibility tests. Add deterministic tests for two concurrent requests with different peaks: the first active session wins, the second cannot mutate the process-global target.

**Commit:** feat: apply client HDR capabilities to launch sessions

## 4. Connect the resolved peak to the supported Windows host consumer and expose truthful diagnostics

**Files:**

- Update Vibepollo/src/platform/windows/virtual_display.h.
- Update Vibepollo/src/platform/windows/virtual_display.cpp.
- Update Vibepollo/src/platform/windows/virtual_display_sunshine.cpp only if a small diagnostic/contract seam is required.
- Update Vibepollo/src/platform/windows/virtual_display_sudovda.cpp only if a non-invasive diagnostic hook is required; do not add a driver protocol field.
- Update the relevant Windows virtual-display unit tests, including Vibepollo/tests/unit/platform/windows/test_virtual_display_sunshine.cpp.

**Work:**

1. Reuse the existing manual MHC2 peak extraction and existing hdr_max_luminance_nits path for Sunshine. Do not create a second luminance conversion.
2. Add a narrow capability/driver-scope query or diagnostic result so the launch path can state whether the resolved automatic peak is being applied to Sunshine’s virtual EDID or only to runtime consumers.
3. For SudoVDA, retain the runtime peak where existing RTX HDR behavior consumes it, but report that exact virtual-display EDID matching is unavailable in v1 because the current wrapper discards hdr_requested.
4. Do not claim that native HDR streams have been retuned: their content/mastering metadata remains source-driven in this change.
5. Test that Sunshine receives the clamped resolved peak, that manual profiles remain authoritative, and that SudoVDA does not receive an accidental unsupported request.

**Verification:** Run the focused Windows virtual-display tests and inspect the generated request/diagnostic values. If a real driver is available, capture Sunshine virtual-display HDR readback and compare it with the resolved peak; separately record the SudoVDA limitation.

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
2. In Session::initialize, after the existing hidden test window has been moved to the selected QScreen/SDL display and before it is destroyed, resolve an immutable display identity for that window. Do not use the real stream window; it is created after /launch.
3. Read the active Windows display ICC association for that monitor using the Windows display/profile API already available to the client build. Parse only the existing MHC2 fixed-point peak field and round to the nearest whole nit using the same units/offset contract as the host.
4. Query the mapped DXGI output through IDXGIOutput6::GetDesc1().MaxLuminance and normalize it to an integer nit value. Treat output mapping or readback failure as absent, not zero.
5. Emit calibrated and EDID values independently when available. A calibrated value is not replaced by EDID, and EDID remains useful as the fallback if the ICC/MHC2 association is absent or unreadable.
6. Serialize only the normalized values and source labels. Never send the physical ICC path, profile bytes, monitor name, EDID blob, or other identifying data.
7. Store only the encoded immutable capability string on the session until the asynchronous launch request consumes it. Ensure collection occurs on the GUI/display thread and does not block the launch worker on repeated profile scans.
8. Add the new sources/headers to app.pro and link any required Windows libraries already used by the renderer (dxgi, gdi32, user32, ole32) without adding a new third-party dependency.

**Verification:** Run the Moonlight QtTest suite with synthetic MHC2/DXGI fixtures. On Windows, exercise one monitor, a multi-monitor layout with negative coordinates, ICC missing, MHC2 missing, DXGI mapping failure, and a display with both sources. Confirm the payload follows the selected stream display rather than whichever monitor owns the process window.

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
4. Omit the custom field for GFE and when collection yields no usable value, avoiding reliance on unknown-parameter tolerance in legacy servers.
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
3. Add a quiet diagnostic/status description for the source actually selected: explicit numeric, host profile, client ICC/MHC2, client EDID, global default, or SudoVDA runtime-only limitation.
4. Explain that client ICC data is normalized to display specifications; Vibepollo never receives or stores the physical ICC file.
5. Replace the getting-started instruction that suggests calibrating the host while the stream is running with the new workflow and preserve the existing manual-profile instructions.
6. Keep all user-facing copy truthful about Sunshine versus SudoVDA and native HDR metadata.

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
   - new Moonlight to old host: unknown field omitted where required and otherwise ignored without failure;
   - GFE launch/resume: no custom field;
   - malformed, oversized, unknown-version, and wrong-platform payloads: ignored;
   - manual valid/missing/malformed profile: manual selection suppresses client values and preserves global fallback on failure;
   - calibrated ICC plus EDID: ICC wins;
   - EDID only: EDID is used;
   - multiple viewers: first active session owns the target and later viewers inherit it;
   - Sunshine: resolved peak reaches virtual-display HDR request;
   - SudoVDA: runtime-only behavior is reported accurately;
   - native HDR and RTX HDR: verify only the existing consumers are affected.
4. Capture launch/resume logs with verbose logging enabled and confirm capability payload contents are never logged.
5. Compare the final diff against the committed design spec and this plan. Remove any implementation or test artifacts not required by the feature.

**Verification target:** All focused and full tests pass in the available environments; any hardware/driver-only matrix row is explicitly recorded with observed result or environment limitation rather than marked as passed without evidence.

**Commit:** test: verify automatic client HDR calibration integration

## 9. Final gap review and handoff

1. Check every requirement in docs/superpowers/specs/2026-08-16-automatic-client-hdr-calibration-design.md against the actual diff, tests, UI copy, and build output.
2. Confirm there is no raw ICC/profile/EDID data crossing the wire, no persistence of client display data, no unintended GFE query change, and no SudoVDA protocol claim.
3. Confirm the host and client repositories contain only intentional changes, with separate scoped commits and no credentials/generated local state.
4. Report the effective precedence, supported driver scope, verification commands/results, and any hardware-only limitations.

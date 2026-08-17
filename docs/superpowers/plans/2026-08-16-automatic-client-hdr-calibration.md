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
- Use one documented transport-limit contract in both repositories: 4096 bytes for the URL-decoded base64 value including padding, 3072 decoded JSON bytes before parsing, and 12288 raw URL-encoded value bytes before query decoding. Enforce each limit at the sender and the corresponding HTTP/base64/JSON host boundary.
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
- The capability field must be bounded before decoding/parsing, URL-safe, redacted from verbose request logs, and sent on both /launch and /resume only after the host advertises protocol version 1.
- Add focused automated tests for every pure parser/policy/clone contract. Add Windows collection and host-driver behavior to the end-to-end checklist where the environment is required.
- v1 exposes source/fallback information through redacted runtime logs only; settings pages explain policy statically and do not claim live per-session status.
- `clientDisplayCapabilities` is redacted case-insensitively on both the Moonlight HTTP and configuration HTTP surfaces before any handler parsing, including malformed, oversized, mixed-case-key, and path-shaped values.
- A common HTTP request adapter must construct a `request_query_view` before any route callback. The view is a closed value-only forwarding surface containing method, path, headers, content/body, local and remote endpoints, transport/TLS identity, a sanitized query view, and a bounded capability preflight result, but no raw `Request*`, raw query string, query parser, or conversion back to `Request`. Wrap every `nvhttp.cpp` route, including `/serverinfo`, authorization/logging, `/launch`, and `/resume`; `print_req()` and handlers consume only this view. Add a static source check that every route is wrapped, no callback contains a direct `request->parse_query_string()` call, and no raw request crosses the adapter. The adapter scans the raw `Request::query_string` before copying or percent-decoding, removes every case-insensitive capability field from the sanitized view, and preserves unrelated query segments. A malformed, repeated, or oversized capability field invalidates only that field and preserves all unrelated query parameters.
- The new client query is capability-negotiated: Vibepollo advertises `ClientDisplayCapabilitiesVersion=1` in `/serverinfo`, and Moonlight sends the field only when version 1 is advertised. GFE and unadvertised Sunshine/Apollo/old Vibepollo hosts receive no new field.
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
- Update Vibepollo/src/nvhttp.cpp and MoonlightQt/app/backend/nvcomputer.h/.cpp
  for the version-1 server-info advertisement/parse contract.

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
8. Add server-info negotiation fixtures: version 1 is advertised by the new
   Vibepollo host and parsed by the new client; missing, zero, malformed, and
   unknown versions disable transmission without changing legacy fields.

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
2. Decode base64url with -/_, accept optional terminal padding, reject invalid characters/padding, and enforce the shared 4096-byte URL-decoded base64 and 3072-byte decoded-JSON limits before strict JSON parsing with nlohmann JSON. The `request_query_view` adapter is the only component that sees the raw `Request::query_string`; it removes every case-insensitive capability field from the handler/logging query view, counts raw value bytes before copying or percent-decoding, and rejects a capability value over 12288 bytes before allocation. Repeated capability fields and malformed percent encoding invalidate only the capability field; unrelated launch parameters remain available.
3. Accept only version == 1, platform == "windows-desktop", known source labels, finite numeric peaks in 1..100000 nits, and the documented object shape. Accept fractional JSON numbers and round them to the nearest whole nit using the same rule as the client; clients emit normalized integers. Treat invalid values as an ignored capability rather than an HTTP error.
4. Add a policy function with explicit inputs for final effective HDR request, numeric override, manual-profile-selected state, manual-profile peak validity, client capabilities, and the existing global fallback. Define final effective HDR as the existing `rtsp_stream::effective_hdr_requested()` result, exactly `enable_hdr && !prefer_sdr_10bit && !force_sdr`, after `hdrMode`, per-client preference, and Windows `hdr_request_override` force-on/force-off/automatic processing. Return both the selected peak/source and an explanation enum suitable for diagnostics. Client-derived values are ignored whenever final effective HDR is false.
5. Apply the existing runtime range clamp only at the existing runtime boundary (400..2000 nits) so transport validation and host behavior remain independently testable.
6. Ensure the policy does not use client values when any non-empty manual profile is selected, even if profile parsing fails. Add independent invalid/absent calibrated-member and display-member cases so one valid client source survives failure of the other.

**Verification:** Re-run the host parser/policy tests from Task 1. Confirm malformed/unsupported payloads are ignored, manual-profile failure preserves global fallback, and no policy path invents a peak.

**Commit:** feat: add bounded client HDR capability policy

## 3. Thread the capability snapshot through Vibepollo launch state and runtime ownership

**Files:**

- Add Vibepollo/src/hdr_runtime_owner.h.
- Add Vibepollo/src/hdr_runtime_owner.cpp.
- Add Vibepollo/src/http_request_view.h.
- Add Vibepollo/src/http_request_view.cpp.
- Update Vibepollo/src/rtsp.h.
- Update Vibepollo/src/rtsp.cpp.
- Update Vibepollo/src/nvhttp.cpp.
- Update Vibepollo/src/confighttp.cpp, src/confighttp_rtss.cpp, and
  src/confighttp_playnite.cpp.
- Update Vibepollo/src/stream.cpp.
- Update Vibepollo/src/webrtc_stream.cpp.
- Update Vibepollo/src/process.cpp.
- Update Vibepollo/src/config.cpp and src/config.h only where the manager must observe or route the feature key.
- Update Vibepollo/tests/unit/test_rtsp_startup_snapshot.cpp.
- Add or update focused host HTTP/session tests if the existing test seams permit them.

**Work:**

1. Add optional client_hdr_capabilities, hdr_peak_resolution, and a generation-checked hdr-runtime owner token to launch_session_t. The resolution records reported peak, effective clamped peak, source, fallback reason, and whether the value was inherited from an active shared session; it is runtime state only and never enters crypto::named_device_t. Implement `hdr_runtime_owner_state` in the new manager with owner token/generation, phase (`provisional`, `awaiting-stream`, `active`, or `retained-paused`), awaiting-stream cohort participant tokens/count, prior owner, candidate map, owned HDR keys, map revision, and resolved target. The manager is the sole production authority for the complete runtime override map; its transactions preserve unrelated runtime keys and conditionally restore only lease-owned HDR entries.
2. Update launch_session_t::clone_for_startup() explicitly so both new fields are copied; do not rely on aggregate/default copying.
3. Add the shared `request_query_view` adapter in `http_request_view.cpp` and use it before every route callback in `nvhttp.cpp`, `confighttp.cpp`, `confighttp_rtss.cpp`, and `confighttp_playnite.cpp`. The adapter scans the corresponding raw transport request query (`Request::query_string` for Moonlight HTTP and the `req_https_t` equivalent for configuration HTTP), removes every case-insensitive `clientDisplayCapabilities` segment, and provides the callback with method, path, headers, content/body, local/remote endpoints, precomputed transport/TLS identity, a sanitized query view, and a bounded capability preflight result. The raw transport request object, raw query string, parser, and any conversion back to a request remain private to the adapter. `print_req()`, serverinfo, authorization, launch, resume, and configuration web-UI handlers consume this view; configuration web-UI launches receive an explicitly empty capability snapshot and cannot advertise or accept the Moonlight protocol. Preserve unrelated segments byte-for-byte. Count raw value bytes before copying or percent-decoding; over-limit, repeated, and malformed-percent fields become invalid capability status without rejecting the request. Decode and validate the retained field before either runtime-override block; enforce the encoded-size limit before base64 decoding. Add a static source check covering every route registration on both HTTP surfaces, direct parser call, raw request parameter, raw request/query access outside the adapter, and every logger path.
4. Pass the same immutable parsed snapshot into every make_launch_session_from_snapshot call path used by launch and resume. Do not let resume's later app-context seeding reconstruct or overwrite the request snapshot.
5. Resolve one canonical effective-HDR seed before peak policy and session construction. First build the prospective runtime map, including any launch/resume candidate override. A pure resolver must apply the candidate map's `dd_hdr_request_override` in preference to the previously active global value, then apply `hdrMode`, per-client preference, and force-on/force-off/automatic processing to produce exactly the existing `rtsp_stream::effective_hdr_requested()` result, `enable_hdr && !prefer_sdr_10bit && !force_sdr`. Pass that one result to both the session builder and peak policy; neither may read the old global override or independently reinterpret `hdrMode`. Test candidate force-off over global automatic and candidate force-on over global force-off.
6. Use the existing launch_request_mutex and stream_lifecycle_gate acquired by the /launch and /resume routes at nvhttp.cpp:4561-4576 as the atomic owner-selection seam. While that gate observes no activity, `begin_candidate` snapshots the prior runtime map/owner, evaluates, and publishes one target under a generation lease. If activity is present, the request must preserve the existing runtime target and mark its session result shared-active-session; it must not evaluate a client peak into global overrides. has_stream_session_activity() already includes pending RTSP/WebRTC activity and teardown.
7. Keep the lease beyond the HTTP handler: HTTP success moves it to `awaiting-stream` and creates a participant in an awaiting-stream cohort. A pending join binds to that same cohort and receives its own participant token. The first RTSP/WebRTC ownership publication from any participant commits the matching generation for the entire cohort; cancellation removes only that participant. Rollback is allowed only when the cohort has no pending participants and no active publication. Thus A-cancel/B-start, B-start/A-cancel, B-cancel/A-start, both-cancel, and mixed RTSP/WebRTC ordering cannot let a stale participant undo a committed or still-pending cohort. Synchronous request failure, pending-session expiry, virtual-display failure, and asynchronous stream-start failure roll back the matching prior map and owner only after the cohort is empty. A stale rollback cannot overwrite a newer generation. Add explicit manager callbacks at `stream.cpp:2705-2808` and every RTSP/WebRTC caller for participant cancel/expiry, first publication, stream teardown, paused retention, replacement, and termination; no local backend cleanup may clear or retain the map independently. Last-stream teardown while the app remains paused changes the owner to `retained-paused` without clearing the map; a later idle request may replace it transactionally; application termination clears map and owner together.
8. Promote the existing `stream_lifecycle_gate`/scoped access into the owner-manager API rather than adding a second lock. Make the manager the sole production authority for the complete runtime override map: `config::set_runtime_config_overrides()` and `clear_runtime_config_overrides()` become manager-private low-level operations, and all unrelated-key edits use a manager transaction that reads, modifies, publishes, and increments a monotonic map revision under the same gate. Each lease records its generation, map revision, owned keys, and candidate values. Rollback conditionally restores only lease-owned HDR entries when the generation is current and those entries still equal the lease's candidate; it must preserve unrelated keys and a newer explicit HDR edit. A generic whole-map replacement that omits `rtx_hdr_peak_brightness` cannot bypass owner invalidation or silently clear an owned target. Route WebRTC's current first-capture runtime-map writer (`webrtc_stream.cpp:3088-3105`), application termination, live `rtx_hdr_peak_brightness` edits, and every existing whole-map caller through the manager. Before implementation, run a repository-wide inventory over `src/**/*.cpp` and `src/**/*.h` for every call to `set_runtime_config_overrides`, `clear_runtime_config_overrides`, direct runtime-map assignment/clear, and every construction of a map containing `rtx_hdr_peak_brightness`; record the exact file/line and classify each as manager-owned, low-level definition, or unrelated-key transaction. The known baseline includes `nvhttp.cpp:3300,3333,3340,3786,3795`, `webrtc_stream.cpp:3094,3104`, `process.cpp:2719,4025,4089`, and the `config.cpp:3186,3220` low-level definitions. After the refactor, no production caller outside `hdr_runtime_owner.cpp` may call the low-level set/clear API. Add a repository test/static check that fails on any unclassified or non-manager writer, and add generation/map-revision rejection tests for every inventoried failure, teardown, termination, unrelated-key edit, and live-edit path.
   Enforce one lock order for the cross-domain process paths: acquire
   `stream_lifecycle_gate` before `_apps_mutex`; no code may acquire the gate
   while holding `_apps_mutex`. Live app-edit code must snapshot and validate
   app state under `_apps_mutex`, release it, acquire the lifecycle gate, then
   revalidate an app revision/UUID before the manager transaction and any
   committed app-state update. `config::apply_config_now()` runs after both
   locks are released. `proc_t::terminate()` retains its existing gate-first
   entry. Add lock-order assertions and a stress test for simultaneous app
   edits, launch/resume, teardown, and termination.
   The mandatory current-source lifecycle inventory before implementation is:
   `src/stream.cpp:2705-2808,2993-3005`; RTSP finalizer callers at
   `src/rtsp.cpp:672-677,724-756,819-846,1076-1127,1137-1163,1882`;
   WebRTC finalizer/lock callers at
   `src/webrtc_stream.cpp:3014,3456-3462,3491,3500,5419,5518,5544,5641`;
   process termination/runtime writers at
   `src/process.cpp:2513-2523,2719,3962-4027,4030-4091`; HTTP/runtime
   writers and client-override construction at
   `src/nvhttp.cpp:3217,3300-3340,3786-3795,4168-4170,4709-4722,4913`;
   configuration and external termination callers at
   `src/confighttp.cpp:2220,2855,5223,5245`, `src/main.cpp:528`,
   `src/system_tray.cpp:107,113,120,530`, and
   `src/platform/windows/playnite_integration.cpp:947`. The inventory must
   be rerun and must finish with zero unclassified lifecycle or map writers.
9. Keep any selected host profile associated with the session even if its MHC2 read fails, suppress client values in that case, and use the existing global fallback.
10. Keep every `print_req()` downstream of the common raw-query adapter so it receives only `request_query_view`. Also make its redaction helper case-insensitive for defense in depth; for every route and regardless of validity/size it may log only presence/status/reason and never the base64, raw query segment, or decoded JSON. The route-inventory test must enumerate every `http_server.resource`, `http_server.default_resource`, and HTTPS equivalent registration in `nvhttp.cpp` plus every `confighttp*.cpp` route, assert that its callback is wrapped, and exercise each wrapper with a secret capability value to prove no logger/parser path receives raw bytes. Configuration web-UI launch handlers must receive an explicitly empty capability snapshot.
   Pin the initial configuration-HTTP inventory to
   `confighttp.cpp:5701-5857` (all default/resource registrations),
   `confighttp.cpp:580-595,1037,1072,1336,3307,3334,3911,3956,4323,5076,
   5416,5656` (logger/parser and web-UI launch seams),
   `confighttp_rtss.cpp:38,95,233-235`, and
   `confighttp_playnite.cpp:62,130,228,258,288,320,372,411,425,468,
   1349,2137,2182,2228-2259` (callback/logger/parser seams). The static
   check must enumerate every route and every raw parser/logger access in all
   three files, not only the main configuration implementation.
11. Keep /launch and /resume behavior symmetric and preserve old-client behavior when the field is missing.
12. Add `ClientDisplayCapabilitiesVersion=1` to Vibepollo's `/serverinfo`
    response. The field is a protocol advertisement only; it does not expose
    client data or change old-client responses. Do not advertise it from a
    build that lacks the host parser/policy implementation.

**Verification:** Run host unit/component tests, including separate launch and resume ordering, startup cloning, request compatibility, runtime cleanup, and logging-redaction tests. Add deterministic lifecycle tests for A-start/B-join/A-end/C-join, A-paused/B-HTTP-success/RTSP-timeout/A-restored, first-launch failure to an empty map, pending cancellation/expiry, WebRTC failure, and `C succeeds before B's delayed rollback` with C preserved. Assert that joins inherit the published effective value, stale generations cannot roll back newer owners, unrelated-key transactions survive lease rollback, generic whole-map omission cannot clear an owned HDR target, live explicit-peak edits update provenance atomically, and only a later idle request can select a new target. Exercise the raw-query adapter with exact 12288/12289-byte values, percent expansion, mixed-case/encoded keys, repeated fields, malformed percent sequences, and path-shaped values; prove unrelated parameters survive, no oversized value is allocated/decoded, every route registration on both HTTP surfaces is wrapped, no route callback calls the raw parser, every handler parses the sanitized query, configuration web-UI launches receive no capability, and no logger receives the capability value. Exercise the centralized `stream.cpp:2705-2808` finalizer and every RTSP/WebRTC caller for pending expiry, first publication, timeout, teardown, paused retention, replacement, and termination. Run the writer-inventory static check.

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
3. Reuse the DISPLAYCONFIG mapping pattern already present in app/streaming/video/ffmpeg-renderers/d3d11va.cpp: query active paths, match the selected HWND/HMONITOR's GDI name, require one unique active path, and retain the complete identity: SDL display index, DXGI adapter/output ordinal, GDI source name, source adapter LUID/source ID, target adapter LUID/target ID, active-path flags, and target availability. Bind DXGI by enumerating the adapter whose LUID equals `path.sourceInfo.adapterId`, then require the selected output's `DeviceName` and `HMONITOR`/desktop bounds to match the selected GDI/HMONITOR. Use `targetInfo.adapterId` plus `sourceInfo.id` only for ColorProfile `targetAdapterID`/`sourceID`.
4. On Windows, dynamically resolve ColorProfileGetDisplayUserScope and ColorProfileGetDisplayDefault from mscms.dll. Query the selected scope and request CPT_ICC with CPST_EXTENDED_DISPLAY_COLOR_MODE. Free the returned profile name with LocalFree. Resolve a bare returned filename through GetColorDirectoryW when available, otherwise the existing system color-directory helper. Require one basename component and reject separators, drive/device/UNC prefixes, `:`, and `.`/`..` components. Open the canonical color directory with `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT`, explicitly use share modes that prevent delete/rename while the authority handle is retained, reject a reparse-tagged directory, and retain its handle plus volume/file ID through candidate open. Enumerate the exact direct-child entry through that retained directory handle and record its volume/file ID. Open the candidate read-only with `FILE_FLAG_OPEN_REPARSE_POINT | OPEN_EXISTING` and the same no-delete/rename share policy, query reparse attributes before reading, and require its volume/file ID to match the retained directory's direct-child entry. Compare the candidate final handle path with the retained directory final path using case-insensitive ordinal comparison and an exact separator boundary. Reopening the directory is diagnostic only; the retained-directory child entry plus candidate file-ID match is the containment authority. A replacement/junction/swap-back race or identity mismatch omits the profile. Perform one bounded read with all validated handles live through the read and enforce a 250 ms wall-clock budget using a cancellable/overlapped read where required. If the budget is exceeded, cancel the read, omit only ICC, and continue with a valid DXGI fallback. Then apply the 32 MiB and MHC2 checks. If exports, OS support, scope, subtype, file, or profile parsing are unavailable, omit calibrated and continue to the display-reported fallback.
5. Apply the host's proven MHC2 bounds and fixed-point conversion in a client-local helper: maximum 32 MiB file, valid MHC2 tag structure, finite positive peak in 1..100000, nearest-nit rounding. Do not use WcsGetDefaultColorProfile or GetICMProfile as the HDR authority.
6. Query the mapped DXGI output through IDXGIOutput6::GetDesc1().MaxLuminance and normalize it to an integer nit value only when the output is current, uniquely mapped, `AttachedToDesktop`, finite/positive, and `ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`. Reject every other color space, including SDR, scRGB, studio-PQ, YCbCr, clone/default, detached, or stale output data. Keep the JSON member named edid for v1 compatibility but label its source dxgi-output.
7. Capture the QScreen/SDL/DISPLAYCONFIG/DXGI identity before profile and output reads, then revalidate every field afterward: adapter/output ordinal, GDI name, source/target LUIDs and IDs, active flags, target availability, output device name, desktop coordinates, AttachedToDesktop, ColorSpace, and the luminance fields used by the snapshot. Windows exposes no descriptor generation number; a successful requery with exact identity/descriptor equality is the currentness check. Any mutation from hotplug, clone transition, relocation, or output replacement omits the capability snapshot.
8. Emit calibrated and display-reported values independently when available. A calibrated value is not replaced by the display-reported fallback, and the fallback remains useful if the ICC/MHC2 association is absent or unreadable.
9. Serialize only the normalized values and source labels. Never send the physical ICC path, profile bytes, monitor name, adapter/source identifiers, EDID blob, or other identifying data.
10. Store a value-only `client_display_preparation_t` on the GUI-owned
    `Session`, containing the encoded capability string and the complete
    selected-display identity/descriptor snapshot. Retain the hidden probe
    window with an RAII lease until `Session::start()` finalizes the
    preparation; do not store QScreen/SDL/API pointers in the worker snapshot.
    Ensure collection occurs on the GUI/display thread, performs one bounded
    profile read per launch, records elapsed collection time with a focused
    latency test, and does not repeat profile scans except when final
    validation detects a changed display.
11. Add the new sources/headers to app.pro and link the required DXGI/Windows libraries; resolve mscms.dll functions dynamically rather than requiring a new hard link that breaks older supported Windows versions.

**Verification:** Run the Moonlight QtTest suite with synthetic MHC2/DXGI/API fixtures. On Windows, exercise one monitor, a multi-monitor layout with negative coordinates, null/unmatched/duplicate QScreen mapping, hidden-window placement mismatch, ICC missing, STANDARD-only profile, missing mscms exports, pre-20348 behavior, malformed MHC2, DXGI mapping failure, non-PQ/SDR/clone/default output, and a display with both sources. Add bare profile-name fixtures for missing, traversal-shaped, UNC/device, alternate-stream, junction/reparse, oversized, and unreadable files; retain the directory handle and volume/file ID through each race fixture, assert canonical-handle containment with case variants and prefix-boundary siblings, and require omission when the directory is replaced or a junction/reparse point is introduced between canonicalization and open. Mutate each captured source/target/DXGI identity and descriptor field between reads and require omission. Confirm the payload follows the selected stream display rather than whichever monitor owns the process window, and is omitted for every unresolved/ambiguous/stale case. Assert the collector and finalizer run on the GUI thread, the worker receives only the immutable value snapshot, and initialization failure, cancellation, repeated finalization, probe-window recreation, and a screen change between initialization and `Session::start()` trigger invalidation, recollection, or omission.

**Commit:** feat: collect calibrated client HDR display capabilities

## 6. Send the capability on Moonlight launch and resume without changing legacy hosts

**Files:**

- Update MoonlightQt/app/backend/nvhttp.h.
- Update MoonlightQt/app/backend/nvhttp.cpp.
- Update MoonlightQt/app/backend/nvcomputer.h/.cpp to retain the parsed
  `ClientDisplayCapabilitiesVersion` server-info capability.
- Update MoonlightQt/app/backend/computermanager.cpp to clear the runtime
  advertisement on failed/offline/identity-replaced polling.
- Update MoonlightQt/app/streaming/session.cpp call sites.
- Update the Moonlight QtTest fixture if request construction is covered there.

**Work:**

1. Extend NvHTTP::startApp with an optional encoded capability argument whose default preserves existing callers.
2. Append clientDisplayCapabilities=<percent-encoded-base64url> to both /launch and /resume query construction when the request is HDR, the host advertises exactly `ClientDisplayCapabilitiesVersion == 1`, and the encoded value is non-empty and within the client-side bound. Do not infer support from `!isNvidiaServerSoftware`, `appVersion`, or a generic non-GFE classification.
3. Preserve all existing hdrMode, clientHdrCapVersion, clientHdrCapSupportedFlagsInUint32, clientHdrCapMetaDataId, and clientHdrCapDisplayData parameters unchanged.
4. Omit the custom field for GFE, an unadvertised/unknown host, and when collection yields no usable value. The pinned pre-feature Vibepollo parser baseline `f8c4ac2762b351457ee57aef0863655d18b936e4` does not advertise the capability, so the new client omits the field; this exact behavior is the v1 old-host guarantee.
5. Add the ephemeral `NvComputer::clientDisplayCapabilitiesVersion` field with a zero initializer. Parse `/serverinfo` into a local zero value on every refresh, accept only integer `1`, and assign it through the existing refresh/merge `ASSIGN_IF_CHANGED` list, including an explicit zero on absent, malformed, zero, or unknown values. Same-host copy construction and assignment carry the current runtime value; default construction, persisted reload, and construction for a replacement host identity start at zero. Add `MoonlightQt/app/backend/computermanager.cpp` to the implementation scope and explicitly invalidate the field under the host lock when all-address polling fails, the host goes offline, or an identity replacement prevents `NvComputer::update()`; a later successful refresh may restore version 1. Do not add it to settings persistence or serialized equality. Add field-level fixtures for version 1 followed by absent/zero/malformed/2, all-address failure, identity mismatch, same-host copy/assignment, host replacement, and persistence round-trip, and assert no stale advertisement survives any path.
6. Define a concrete GUI-owned `client_display_preparation_t` handoff. `Session` owns a GUI-thread-only RAII hidden probe-window member and an optional preparation member with states `unprepared`, `prepared`, `invalidated`, `handed-off`, and `completed`. Each preparation carries a monotonically increasing generation. Initialization failure, cancellation, or display invalidation increments the generation, destroys/clears the window and snapshot, and prevents handoff. `Session::start()` is GUI-thread-only and, immediately before creating `AsyncConnectionStartThread`, invokes `finalize_client_display_preparation()`, reuses or recreates the probe window, re-resolves the selected QScreen/SDL/DISPLAYCONFIG/DXGI identity, and recollects or clears the value if any identity/descriptor differs. It then atomically moves only a copied immutable value snapshot plus generation into the worker constructor, destroys the probe window on the GUI thread, and marks the preparation handed-off. Replace the current worker's direct `Session` pointer, `m_AsyncConnectionSuccess` mutation, and direct success/error side effects with a value-only `connection_start_result_t { generation, success, error }` signal. A queued GUI-thread slot alone validates generation and active state before mutating `Session`, emitting UI signals, or entering `exec`; cancellation invalidates the generation before signaling the worker. The worker may call `NvHTTP::startApp()` with the immutable string but may not call QScreen, SDL, DISPLAYCONFIG, DXGI, ColorProfile APIs, or mutate `Session`. Redact `clientDisplayCapabilities` from the client URL logger before any verbose `toString()`/request logging. Add request serialization tests for both verbs, HDR/non-HDR, advertised/unadvertised/GFE, empty payload, and percent-encoding, plus thread-affinity and out-of-order tests for initialization failure, finalization, cancellation, repeated finalization/start, worker success/failure, Session destruction, late completion, probe-window recreation, screen mutation, and verbose URL redaction.

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

**Files:** No new production files; add/update the review artifact
`docs/superpowers/verification/2026-08-16-automatic-client-hdr-calibration-compatibility.md`
with exact host commit, advertised version, launch result, resume result, and
`observed`/`unavailable`/`not-applicable` status for every row.

The artifact must contain these named rows, with no implicit “representative”
coverage:

| Row | Classification | Advertisement | Launch | Resume | Expected decision |
| --- | --- | --- | --- | --- | --- |
| Pinned pre-feature Vibepollo `f8c4ac2762b351457ee57aef0863655d18b936e4` | software-required | absent | omit field | omit field | observed required |
| Current Vibepollo implementation commit | software-required | `1` | accept field and resolve peak | accept field and resolve peak | observed required |
| Pinned old Moonlight client → current Vibepollo | software-required | `1` | omit field | omit field | observed required |
| Current Moonlight client → pinned pre-feature Vibepollo | software-required | absent | omit field | omit field | observed required |
| Generic Sunshine baseline | external compatibility | absent unless explicitly verified otherwise | omit field | omit field | observed, unavailable, or not-applicable |
| Generic Apollo baseline | external compatibility | absent unless explicitly verified otherwise | omit field | omit field | observed, unavailable, or not-applicable |
| GFE | external/legacy | excluded | omit field | omit field | observed or not-applicable |

Every software-required row must have observed launch and resume results;
`unavailable` cannot satisfy those rows. External/hardware rows may be marked
`unavailable` only with an environment reason and never satisfy the core gate.
Any host that advertises version `1` but fails either verb is a compatibility
gate failure and must be removed from the sender's supported advertisement
set; it cannot be recorded as a passing row.

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
   - new Moonlight to the pinned pre-feature Vibepollo host baseline `f8c4ac2762b351457ee57aef0863655d18b936e4`: `/serverinfo` has no `ClientDisplayCapabilitiesVersion`, so the client omits the custom field for both launch/resume; freeze this as the old-host compatibility contract;
   - current Vibepollo host: `/serverinfo` advertises exactly version 1, and both launch/resume accept the field; record the response and parsed effective peak for each verb;
   - representative generic Sunshine/Apollo hosts without the advertisement: client omits the field for both launch/resume; do not claim generic non-GFE support based only on `isNvidiaServerSoftware` or `appVersion`;
   - any future host claiming version 1: require an explicit launch/resume pass row in the compatibility artifact before enabling that host build; a rejection fails the gate and the client must omit the field for that version;
   - GFE launch/resume: no custom field;
   - malformed, oversized, unknown-version, wrong-platform, and non-finite payloads: invalid values ignored; in-range fractional values are rounded identically by host and client; enforce the 4096-byte URL-decoded base64, 3072-byte decoded JSON, and 12288-byte raw URL-encoded limits at the common raw-query adapter; exact 12288/12289 boundaries, repeated/mixed-case/percent-encoded keys, malformed percent sequences, and path-shaped values never reach generic parsing/logging; valid client values on SDR requests are ignored by host policy;
   - manual valid/missing/malformed profile: manual selection suppresses client values and preserves global fallback on failure;
   - calibrated ICC plus Windows/DXGI display-reported value: ICC wins; invalid calibrated plus valid display-reported preserves the latter, and the inverse preserves calibrated;
   - display-reported value only: it is used only for a current, unique HDR output;
   - unresolved, mirrored, SDR, detached, or clone/default output: no client capability is sent;
   - multiple viewers: first idle request owns the target, later viewers inherit it, failed first/replacement launches restore the prior owner, paused-app disconnect retains the map, and a later idle request can replace it transactionally;
   - Sunshine: resolved peak reaches the existing `hdr_max_luminance_nits` virtual-display request;
   - SudoVDA: no new luminance argument reaches `createVirtualDisplay`, while the resolved peak reaches the existing `rtx_hdr_peak_brightness`/RTSP RTX HDR consumer and runtime-only behavior is reported accurately in redacted logs and static documentation;
   - native HDR and RTX HDR: verify only the existing consumers are affected;
   - exact HDR predicate: `enable_hdr && !prefer_sdr_10bit && !force_sdr` after legacy/client/force-on/force-off inputs are resolved, on both launch and resume;
   - final pre-worker client validation: move/hotplug the selected display after initialization but before `Session::start()` and require recollection or omission; record the remaining post-handoff race as a limitation;
   - profile opening: valid bare name, replacement/reparse race, containment attacks, 32 MiB boundary, injected slow read, and one-read invocation/latency evidence;
   - all `set_runtime_config_overrides`/clear paths, including the repository-wide inventory over `src/**/*.cpp` and `src/**/*.h`, the known `nvhttp.cpp`, `webrtc_stream.cpp`, `process.cpp`, and `config.cpp` seams, WebRTC, live RTX HDR editing, and process termination, preserve map-plus-owner consistency; every result is classified and the static writer check has no unclassified or feature-key callers outside `hdr_runtime_owner.cpp` and the low-level config definitions.
4. Capture launch/resume logs with verbose logging enabled and confirm capability payload contents are never logged.
5. Compare the final diff against the committed design spec and this plan. Remove any implementation or test artifacts not required by the feature.

**Verification target:** All focused and full tests pass in the available environments. Each software-required compatibility row has observed launch and resume results. Each external/hardware row is recorded with one of `observed`, `unavailable`, or `not-applicable`; `unavailable` is an environment limitation, never a passing result. Artifact validation fails if a software-required cell is unavailable or if either old-client/new-host row is absent.

**Commit:** test: verify automatic client HDR calibration integration

## 9. Final gap review and handoff

1. Check every requirement in docs/superpowers/specs/2026-08-16-automatic-client-hdr-calibration-design.md against the actual diff, tests, UI copy, and build output.
2. Confirm there is no raw ICC/profile/EDID data crossing the wire, no persistence of client display data, no unintended GFE query change, and no SudoVDA protocol claim.
3. Confirm the host and client repositories contain only intentional changes, with separate scoped commits and no credentials/generated local state.
4. Report the effective precedence, supported driver scope, verification commands/results, and any hardware-only limitations.

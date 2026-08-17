# Automatic Client HDR Calibration Compatibility

**Date:** 2026-08-17

**Scope:** Capability advertisement and `clientDisplayCapabilities` behavior on
`/launch` and `/resume`.

**Status:** Host/client build and focused contract verification complete; live
launch/resume compatibility remains pending.

## Evidence boundary

- The exact host implementation revision is
  `bb4632271d5b16be4455f0892a713b07681ce47c`. The staged host build produced
  `sunshine.exe` on 2026-08-17.
- The exact client implementation revision is
  `c6e5764343a5c9a28ecfe3cb34348a1b420f2e9c`. The staged Windows client build
  produced `Moonlight.exe` on 2026-08-17.
- The pinned pre-feature host revision is
  `f8c4ac2762b351457ee57aef0863655d18b936e4`.
- Static source inspection finds
  `ClientDisplayCapabilitiesVersion=1` at `src/nvhttp.cpp:2951` in the current
  working tree. The pinned pre-feature `src/nvhttp.cpp` has no matching
  advertisement. These are source observations, not runtime `/serverinfo`
  captures.
- The staged host/client builds and focused tests are local evidence only. No
  connected generic Sunshine, Apollo, GFE, or real host/client launch-resume
  environment was available, so no live launch or resume result is presented
  as observed.

`pending` means a software-required runtime observation is genuinely missing and
does not satisfy the compatibility gate. `unavailable` means the external
environment or hardware needed for the row is not present; it is not a pass.

## Compatibility matrix

| Row | Classification | Exact host revision | Advertisement | Launch | Resume | Status | Reason | Expected decision |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Pinned pre-feature Vibepollo `f8c4ac2762b351457ee57aef0863655d18b936e4` | software-required | `f8c4ac2762b351457ee57aef0863655d18b936e4` | `absent` (static source observation; runtime pending) | `pending` | `pending` | `pending` | No built pinned host/client pairing is available in this checkout. | Omit field for both verbs. |
| Current Vibepollo implementation commit | software-required | `bb4632271d5b16be4455f0892a713b07681ce47c` | `pending` (source advertises `1`; runtime `/serverinfo` not captured) | `pending` | `pending` | `pending` | Host/client builds and focused contract tests pass, but no connected launch/resume capture was available. | Advertise `1`; accept the field and resolve the peak for both verbs. |
| Pinned old Moonlight client → current Vibepollo | software-required | `bb4632271d5b16be4455f0892a713b07681ce47c` | `pending` (current source advertises `1`) | `pending` | `pending` | `pending` | No pinned old Moonlight client checkout or executable is available for launch/resume capture. | Omit field for both verbs. |
| Current Moonlight client → pinned pre-feature Vibepollo | software-required | `f8c4ac2762b351457ee57aef0863655d18b936e4` | `absent` (static source observation; runtime pending) | `pending` | `pending` | `pending` | No current Moonlight client executable or built pinned host is available for the two-verb check. | Omit field for both verbs. |
| Generic Sunshine baseline | external compatibility | not applicable | `unavailable` | `unavailable` | `unavailable` | `unavailable` | No generic Sunshine host/session is available; compatibility is not inferred from a product name or non-GFE classification. | Omit field unless version `1` is explicitly advertised and both verbs pass. |
| Generic Apollo baseline | external compatibility | not applicable | `unavailable` | `unavailable` | `unavailable` | `unavailable` | No generic Apollo host/session is available; compatibility is not inferred from a product name or non-GFE classification. | Omit field unless version `1` is explicitly advertised and both verbs pass. |
| GFE | external/legacy | not applicable | `unavailable` (excluded by protocol contract) | `unavailable` | `unavailable` | `unavailable` | No GFE client/session is available for a launch/resume capture. | Exclude GFE and omit the field for both verbs. |

## Local build and contract evidence

- Host staged build: `ninja sunshine` completed successfully with the Windows
  toolchain.
- Host focused CTest: 8/8 passed — client capability parser, peak policy,
  runtime owner, request policy, request-view redaction, RTSP startup snapshot,
  locale consistency, and the static writer/feature-key/route inventory.
- Client staged build: qmake-configured `nmake` completed successfully and
  linked `Moonlight.exe`.
- Client focused tests: capability serializer 9/9, negotiation 5/5, Windows
  collector 14/14, and all four existing VRR regression executables exited 0;
  the session handoff executable also exited 0.

## Required follow-up

The four software-required rows marked `pending` need real host/client launch
and resume captures before this artifact can be treated as a passing matrix.
The three external rows remain explicitly unavailable until their named
environments are tested; no hardware or external compatibility observation is
claimed by this document.

# SysVAD MAT10 Feasibility Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and statically validate a dedicated HDMI-only Windows driver package that advertises only PCM and exact IEC 61937 Dolby MLP/MAT10, without installing or starting the driver. This Stage-2 goal is complete; the status below records the evidence boundary rather than implying live-driver readiness.

**Architecture:** Derive a new, narrowly compiled `Mat10AudioSample` KMDF driver from the SysVAD HDMI path instead of altering the stock multi-endpoint project. Its single endpoint carries independently declared PCM and MAT10 tables, a dedicated topology, and metadata-only MAT10 lifecycle/state evidence; the companion `Mat10Package` is the sole package surface. PowerShell validation fails closed on identity, advertisement, descriptor, package, or dry-run recovery mismatches, so this stage produces a buildable feasibility artifact but performs no machine mutation. The current implementation does not contain a kernel payload tap, control device, capture ring, network transport, or client renderer; those are later-stage design work.

**Tech Stack:** WDK KMDF/PortCls SysVAD, Visual Studio MSBuild, C++17, INF/InfVerif, PowerShell 5.1, `Get-WindowsDriver`, compiled state/concurrency harnesses, mutation fixtures, and static source/package audits.

**Execution status (2026-08-16):** Stage 2 is independently GREEN and frozen at SysVAD commit `6160f5a` (`primez-x/Windows-driver-samples`, branch `agent/atmos-mat-endpoint`). The dedicated `Release|x64` package, source/package SYS equality, INF identity, catalog membership, unsigned checks, containment checks, lifecycle fixtures, and 17 compiled/static mutation suites passed. The frozen unsigned artifacts are `VibepolloAtmosMat10.sys` SHA-256 `380E32B97067B87F582241F33000188EF008114E2A48E6E611415F9FF13DAC2D`, INF `99AFDE8D8D5326BDA900D9CB7078DAE2F93BC617C57EBA541DD7C0C2207F3604`, and CAT `F88010B3B9E8E528076ED0D2611E396A027EA74945A2E731AC86B12F98A52F68`. No driver was signed, installed, loaded, or exercised against a live device; Uninstall/Recover intentionally fail closed until a genuine installer-authenticated receipt exists. Stage 3+ runtime, tap, transport, client, and public-release work remains deferred.

## Global Constraints

- Source changes execute only in `C:\Users\Matt\Documents\Codex\2026-08-15\vibepollo-atmos\worktrees\sysvad-atmos`; this plan and its reference contract, `docs/superpowers/specs/2026-08-15-atmos-transport-design.md`, live in the Vibepollo worktree.
- Build a new `Mat10AudioSample` project and a new `Mat10Package` project. Do not modify, package, install, or validate the stock `TabletAudioSample` package as the feasibility artifact.
- The package hardware ID is exactly `Root\VibepolloAtmosMat10`; driver binary, service, and catalog names are exactly `VibepolloAtmosMat10`; provider and manufacturer strings are exactly `Vibepollo Atmos Project`; endpoint friendly name is exactly `Vibepollo Atmos MAT10 Virtual HDMI`.
- Use these GUIDs exactly: driver `ProjectGuid` `FAC928CA-A9B2-446A-B857-0275013DD22E`, driver `SampleGuid` `1DDCF5FD-43DD-41A3-AF61-C96806A04521`, package `ProjectGuid` `586E8DDF-C140-4218-AB6A-C124E6A50DFD`, package `SampleGuid` `B0CAE769-4C4D-470F-B90D-0F3A18BE9879`.
- Compile precisely one HDMI render endpoint and no capture, APO, sideband, Bluetooth, USB, A2DP, SPDIF, or other endpoints. Do not define `SYSVAD_BTH_BYPASS`, `SYSVAD_USB_SIDEBAND`, or `SYSVAD_A2DP_SIDEBAND` in any configuration.
- The endpoint host formats are exactly five: PCM 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, and one 52-byte `WAVEFORMATEXTENSIBLE_IEC61937` Dolby MLP/MAT10 descriptor. MAT20, MAT21, DTS, Dolby Digital, and every other encoded subtype are absent from source tables, pointer arrays, compiled resources, INF, and package manifest.
- The exact MAT10 descriptor is `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP` / `{0000000C-0CEA-0010-8000-00AA00389B71}` with eight channels, 192000 Hz carrier, 3072000 bytes/s, block align 16, 16 bits, `cbSize=34`, 16 valid bits, `KSAUDIO_SPEAKER_7POINT1`, 96000 encoded samples/s, eight encoded channels, and zero encoded average bytes/s.
- Advertise two stream data ranges (PCM and MLP), one PCM-only loopback range, two bridge topology ranges (analog and MLP), four stream pointer entries, one loopback pointer entry, two bridge pointer entries, and `MAT10_DEVICE_MAX_CHANNELS == 8`.
- Add `eMat10RenderDevice` in `audio/sysvad/common.h` and teach `audio/sysvad/EndpointsCommon/minwavert.cpp` to recognize it wherever HDMI-specific format/DRM behavior is required.
- Preserve no audio bytes: metadata records may contain probe/stream identity, canonical format sizes, rights result, `RUN`, raw write-callback fields, derived committed frontier, consumed-byte/range counters, terminal reason, format identity, and alignment status only. They must contain no buffer address, byte array, copied payload, capture ring, IOCTL read interface, or capture/loopback endpoint beyond the ordinary PCM loopback format advertisement.
- Every `KSDATAFORMAT` validation path checks the buffer has at least `FIELD_OFFSET(KSDATAFORMAT, FormatSize) + sizeof(ULONG)`, reads `FormatSize` once, requires `FormatSize == sizeof(MAT10_KSDATAFORMAT) == 116` (`sizeof(KSDATAFORMAT) == 64`), and only then casts or reads the extended fields. `MAT10_KSDATAFORMAT` contains the `KSDATAFORMAT` header followed by the canonical 52-byte `WAVEFORMATEXTENSIBLE_IEC61937`; reject zero, short, oversized, compact/84-byte, or noncanonical MAT10 inputs before dereference.
- Build only `Release|x64` with `/WX` through `C:\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe`; run the current WDK's WHQL-aligned `InfVerif /h /v` mode against the generated INF.
- This slice never enables test signing, installs a driver, reboots, changes boot policy, opens a driver device, or creates a live audio stream. Install, uninstall, and recovery scripts must accept only `-ValidateOnly` or `-WhatIf` and prove exact-root identity before they emit their intended command.

## Current evidence boundary

- Stage 1 capability evidence is complete: the no-write probe and spatial carrier-state evidence cover the intended laptop HDMI route. A soundbar `Atmos` badge or continuously present carrier indication is route/carrier-state evidence only; it is not proof of native content provenance, a newly acquired receiver lock, or bit-exact relay.
- Stage 2 source/build/package evidence is complete and independently GREEN. The native MAT10 contract is a 116-byte `KSDATAFORMAT` wrapper (`sizeof(KSDATAFORMAT) == 64`) containing the exact 52-byte `WAVEFORMATEXTENSIBLE_IEC61937`; the metadata/state implementation covers stream identity, rights, `KSSTATE_RUN`, raw `SetWritePacket` geometry/frontier, aligned consumption, and terminal/quiesce behavior without audio payload fields.
- The Stage-2 package remains unsigned and uninstalled. Build-only verification, catalog membership, `NotSigned` checks, lifecycle containment, read-only fixture checks, and state/mutation harnesses are evidence for source/package correctness, not live driver/runtime proof. There is no genuine installer receipt; Uninstall and Recover fail closed even for fabricated read-only receipts.
- Steam/Realtek restoration code and focused tests are complete in the Vibepollo host tree, while transactional live deployment and controlled stream/teardown verification remain pending at this documentation boundary.
- Explicitly deferred: signed installation and kernel runtime eligibility; WDM control device and kernel tap/ring; opaque payload handoff; packetization/FEC/AEAD/retransmission; client WASAPI renderer/UI; clock recovery/A/V synchronization; full end-to-end Atmos; compatibility/lifecycle trials that require installation; and signing/public release.

---

## File Map

- Create `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj`: dedicated KMDF driver project with only the required parent and HDMI sources.
- Create `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj.filters`: IDE ownership for the narrow driver source set.
- Create `audio/sysvad/Mat10AudioSample/Mat10AudioSample.rc`: driver resource compiled by the new project.
- Create `audio/sysvad/Mat10AudioSample/mat10wavtable.h`: exact five-format host table, data-range arrays, pointer arrays, and compile-time count/size assertions.
- Create `audio/sysvad/Mat10AudioSample/mat10toptable.h`: HDMI topology table exposing analog and MLP only.
- Create `audio/sysvad/Mat10AudioSample/minipairs.h`: one `Mat10Miniports` endpoint pair, one render array, null capture array/count, and metadata contract declarations.
- Create `audio/sysvad/Mat10Package/Mat10Package.vcxproj`: package project that references only `Mat10AudioSample`.
- Create `audio/sysvad/Mat10Package/Mat10Package.vcxproj.filters`: package source ownership.
- Create `audio/sysvad/Mat10Package/VibepolloAtmosMat10.inx`: the only INF source, with the exact package identity and HDMI render interfaces.
- Create `audio/sysvad/scripts/Verify-Mat10Static.ps1`: read-only source, project, INF, and generated-package verifier.
- Create `audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1`: `-ValidateOnly`/`-WhatIf` lifecycle-command validator with exact-root identity checks.
- Create `audio/sysvad/Mat10StateTests/`: the compiled `/warnaserror` state, concurrency, native-format, notification-geometry, terminal/quiesce, and mutation harness.
- Create `audio/sysvad/scripts/Mat10Verification.psm1`, `Assert-Mat10BuildContainment.ps1`, `Mat10Stage2PackageBinding.json`, and the lifecycle/static/PowerShell compatibility fixtures used by the frozen package gate.
- Modify `audio/sysvad/common.h`: add `eMat10RenderDevice` and the fixed-size metadata-only event contract.
- Modify `audio/sysvad/EndpointsCommon/minwavert.cpp`: apply the bounded `FormatSize` gate and record same-stream metadata-only probe/creation/rights/RUN/write/consumption/terminal evidence for `eMat10RenderDevice`.
- Modify `audio/sysvad/EndpointsCommon/minwavert.h`, `minwavertstream.h`, and `minwavertstream.cpp`: enforce the native stream identity, rights transaction, contiguous packet frontier, exact consumption, and terminal/quiesce state transitions used by the metadata evidence.
- Modify `audio/sysvad/sysvad.sln`: add only `Mat10AudioSample` and `Mat10Package` solution entries and `Release|x64` build mappings.

## Fixed Interfaces

```cpp
// audio/sysvad/common.h
typedef enum _eDeviceType {
    // existing values unchanged
    eMat10RenderDevice,
} eDeviceType;

typedef enum _MAT10_METADATA_KIND {
    Mat10MetadataFormatProbed = 1,
    Mat10MetadataStreamCreated = 2,
    Mat10MetadataRightsUnprotected = 3,
    Mat10MetadataRightsRestricted = 4,
    Mat10MetadataRightsError = 5,
    Mat10MetadataRun = 6,
    Mat10MetadataWriteCommitted = 7,
    Mat10MetadataConsumed = 8,
    Mat10MetadataStop = 9,
    Mat10MetadataError = 10,
} MAT10_METADATA_KIND;

typedef enum _MAT10_RIGHTS_STATE {
    Mat10RightsUnknown = 0,
    Mat10RightsUnprotected = 1,
    Mat10RightsRestricted = 2,
    Mat10RightsError = 3,
} MAT10_RIGHTS_STATE;

typedef enum _MAT10_TERMINAL_REASON {
    Mat10TerminalNone = 0,
    Mat10TerminalCallbackBeforeReady = 1,
    Mat10TerminalInvalidNotificationGeometry = 2,
    Mat10TerminalUnsupportedProducerPacketSemantics = 3,
    Mat10TerminalInvalidEndOfStream = 4,
    Mat10TerminalArithmeticOverflow = 5,
    Mat10TerminalUnderlyingWriteRejected = 6,
    Mat10TerminalConsumptionBeyondFrontier = 7,
    Mat10TerminalInvalidConsumption = 8,
    Mat10TerminalRightsFailure = 9,
    Mat10TerminalRightsRestricted = 10,
    Mat10TerminalRunPaused = 11,
    Mat10TerminalStreamStopped = 12,
    Mat10TerminalNotificationBufferFreed = 13,
    Mat10TerminalNotificationBufferReallocated = 14,
    Mat10TerminalFormatChanged = 15,
    Mat10TerminalStreamClosed = 16,
    Mat10TerminalDestroyed = 17,
} MAT10_TERMINAL_REASON;

typedef struct _MAT10_METADATA_EVENT {
    ULONG Size;                 // sizeof(MAT10_METADATA_EVENT)
    MAT10_METADATA_KIND Kind;
    GUID SubFormat;             // MLP only
    ULONG KsFormatBytes;        // 116 only
    ULONG WaveFormatBytes;      // 52 only
    ULONG State;                // KSSTATE_RUN only for Run
    ULONGLONG StreamId;         // nonzero after successful stream creation
    ULONGLONG DriverGeneration; // nonzero after successful stream creation
    MAT10_RIGHTS_STATE RightsState;
    ULONGLONG CommittedBytes;   // frontier, never payload
    ULONGLONG ConsumedBytes;    // count only, never payload
    BOOLEAN Is16ByteAligned;
    LONG Status;
    ULONG DmaBufferBytes;
    ULONG NotificationCount;
    ULONG PacketBytes;
    ULONG PacketNumber;         // raw SetWritePacket input
    ULONG WriteFlags;           // raw SetWritePacket input
    ULONG EosPacketLength;      // raw SetWritePacket input
    ULONGLONG LogicalPacketNumber;
    ULONGLONG CommittedRangeStart;
    ULONGLONG CommittedRangeEnd;
    ULONGLONG ConsumedRangeStart;
    ULONGLONG ConsumedRangeEnd;
    MAT10_TERMINAL_REASON TerminalReason;
} MAT10_METADATA_EVENT;

typedef struct _MAT10_KSDATAFORMAT {
    KSDATAFORMAT DataFormat;
    WAVEFORMATEXTENSIBLE_IEC61937 WaveFormat;
} MAT10_KSDATAFORMAT;

// audio/sysvad/Mat10AudioSample/minipairs.h
extern PENDPOINT_MINIPAIR g_RenderEndpoints[];
extern const ULONG g_cRenderEndpoints;
extern PENDPOINT_MINIPAIR g_CaptureEndpoints;
extern const ULONG g_cCaptureEndpoints;

// audio/sysvad/EndpointsCommon/minwavert.cpp
NTSTATUS ValidateMat10Format(_In_reads_bytes_(bufferSize) const KSDATAFORMAT* format,
                             _In_ ULONG bufferSize,
                             _Out_ const MAT10_KSDATAFORMAT** validated);
VOID RecordMat10Metadata(_In_ const MAT10_METADATA_EVENT* eventRecord);
```

`ValidateMat10Format` returns `STATUS_BUFFER_TOO_SMALL` for an unreadable `FormatSize`, `STATUS_INVALID_BUFFER_SIZE` unless `FormatSize` is exactly `sizeof(MAT10_KSDATAFORMAT) == 116`, and `STATUS_NO_MATCH` for a well-sized non-MLP or noncanonical descriptor. The helper validates the 64-byte native `KSDATAFORMAT` plus the exact 52-byte `WAVEFORMATEXTENSIBLE_IEC61937` before dereference. `RecordMat10Metadata` accepts only a fixed-size metadata event, validates the native format identity, rights/state/frontier invariants, and emits no pointer, payload, capture buffer, or IOCTL read path. The accepted evidence is a same-stream sequence of `STREAM_CREATED`, `RIGHTS_UNPROTECTED`, `RUN`, `WRITE_COMMITTED`, and `CONSUMED`; `FORMAT_PROBED` is separate and cannot satisfy stream creation, while rights/terminal/error events fail closed.

### Task 1: Add the isolated projects and make the static verifier fail first

**Files:**
- Create: `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj`
- Create: `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj.filters`
- Create: `audio/sysvad/Mat10AudioSample/Mat10AudioSample.rc`
- Create: `audio/sysvad/Mat10Package/Mat10Package.vcxproj`
- Create: `audio/sysvad/Mat10Package/Mat10Package.vcxproj.filters`
- Create: `audio/sysvad/scripts/Verify-Mat10Static.ps1`
- Modify: `audio/sysvad/sysvad.sln`

**Interfaces:**
- Consumes: the existing `TabletAudioSample.vcxproj`, `Package/package.VcxProj`, and `sysvad.sln` only as structure references.
- Produces: `Mat10AudioSample` and `Mat10Package` as independently buildable `Release|x64` projects, and `Invoke-Mat10StaticVerification` for later tasks.

- [x] **Step 1: Write the failing project-isolation assertions**

Create `Verify-Mat10Static.ps1` with this executable test surface before creating either project:

```powershell
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $RepositoryRoot,
    [string] $Configuration = 'Release',
    [string] $Platform = 'x64',
    [string] $PackageRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-Match([string] $Path, [string] $Pattern) {
    if (-not (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "required pattern missing: $Pattern in $Path"
    }
}
function Forbid-Match([string] $Path, [string] $Pattern) {
    if (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet) {
        throw "forbidden pattern present: $Pattern in $Path"
    }
}

$driver = Join-Path $RepositoryRoot 'audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj'
$package = Join-Path $RepositoryRoot 'audio/sysvad/Mat10Package/Mat10Package.vcxproj'
if (-not (Test-Path -LiteralPath $driver)) { throw "missing dedicated driver project: $driver" }
if (-not (Test-Path -LiteralPath $package)) { throw "missing dedicated package project: $package" }
Require-Match $driver '<ProjectGuid>{FAC928CA-A9B2-446A-B857-0275013DD22E}</ProjectGuid>'
Require-Match $driver '<SampleGuid>{1DDCF5FD-43DD-41A3-AF61-C96806A04521}</SampleGuid>'
Require-Match $package '<ProjectGuid>{586E8DDF-C140-4218-AB6A-C124E6A50DFD}</ProjectGuid>'
Require-Match $package '<SampleGuid>{B0CAE769-4C4D-470F-B90D-0F3A18BE9879}</SampleGuid>'
Forbid-Match $driver 'SYSVAD_(BTH_BYPASS|USB_SIDEBAND|A2DP_SIDEBAND)'
Forbid-Match $package 'TabletAudioSample|SwapAPO|DelayAPO|KWSApo|AecApo|KeywordDetector'
```

- [x] **Step 2: Run the verifier to confirm RED**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: failure beginning `missing dedicated driver project:`. This proves the verifier refuses to accept the stock SysVAD projects as the package under test.

- [x] **Step 3: Create the two projects and solution entries**

Create `Mat10AudioSample.vcxproj` by retaining only `Debug|x64` and `Release|x64` configuration groups from the sample structure, then set these exact globals and build items:

```xml
<ProjectGuid>{FAC928CA-A9B2-446A-B857-0275013DD22E}</ProjectGuid>
<SampleGuid>{1DDCF5FD-43DD-41A3-AF61-C96806A04521}</SampleGuid>
<TargetName>VibepolloAtmosMat10</TargetName>
<PreprocessorDefinitions>%(PreprocessorDefinitions);_USE_WAVERT_;_USE_IPortClsRuntimePower;_NEW_DELETE_OPERATORS_</PreprocessorDefinitions>
<AdditionalDependencies>%(AdditionalDependencies);.\..\EndpointsCommon\$(IntDir)\EndpointsCommon.lib</AdditionalDependencies>
<ClCompile Include="..\adapter.cpp" />
<ClCompile Include="..\basetopo.cpp" />
<ClCompile Include="..\common.cpp" />
<ClCompile Include="..\hw.cpp" />
<ClCompile Include="..\kshelper.cpp" />
<ClCompile Include="..\savedata.cpp" />
<ClCompile Include="..\tonegenerator.cpp" />
<ClCompile Include="..\TabletAudioSample\hdmitopo.cpp" />
<ResourceCompile Include="Mat10AudioSample.rc" />
```

Set `<TreatWarningAsError>true</TreatWarningAsError>` and `<WarningLevel>Level4</WarningLevel>` for `Release|x64`; include `..\EndpointsCommon` and `.` only. Do not include `A2dpHpDevice.cpp`, `BthhfpDevice.cpp`, `UsbHsDevice.cpp`, `micintopo.cpp`, `spdiftopo.cpp`, any APO project, or any sideband definition.

Create `Mat10Package.vcxproj` with only this driver reference and exact package globals:

```xml
<ProjectReference Include="..\Mat10AudioSample\Mat10AudioSample.vcxproj">
  <Project>{FAC928CA-A9B2-446A-B857-0275013DD22E}</Project>
</ProjectReference>
<ProjectGuid>{586E8DDF-C140-4218-AB6A-C124E6A50DFD}</ProjectGuid>
<SampleGuid>{B0CAE769-4C4D-470F-B90D-0F3A18BE9879}</SampleGuid>
<DriverType>Package</DriverType>
<HardwareIdString>Root\VibepolloAtmosMat10</HardwareIdString>
```

Add those two project paths to `sysvad.sln` with the same project type used by the existing driver/package projects, then map only `Release|x64` to `Build.0`. Add a `Mat10AudioSample` solution dependency on the existing `EndpointsCommon` project so its modified `minwavert.cpp` library is built before the driver links; do not add a second common-library project. The filters must list exactly the compiled files above plus the three MAT10 headers and `VibepolloAtmosMat10.inx` once it exists.

- [x] **Step 4: Run the isolation verifier to confirm GREEN**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: exit `0`; the verifier finds both unique GUID pairs and finds no stock driver, sideband, or APO package dependency.

**Review boundary:** inspect `git diff -- audio/sysvad/Mat10AudioSample audio/sysvad/Mat10Package audio/sysvad/sysvad.sln audio/sysvad/scripts/Verify-Mat10Static.ps1`; reject the task if a stock package project is referenced or any non-MAT10 endpoint source is compiled.

### Task 2: Define the exact MAT10 wave and topology tables with count guards

**Files:**
- Create: `audio/sysvad/Mat10AudioSample/mat10wavtable.h`
- Create: `audio/sysvad/Mat10AudioSample/mat10toptable.h`
- Modify: `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj.filters`
- Modify: `audio/sysvad/scripts/Verify-Mat10Static.ps1`

**Interfaces:**
- Consumes: WDK `WAVEFORMATEXTENSIBLE_IEC61937`, `KSDATAFORMAT_WAVEFORMATEXTENSIBLE`, PortCls table types, and the existing HDMI table layout.
- Produces: `Mat10HostPinSupportedDeviceFormats`, `Mat10WavePinDataRangePointersStream`, `Mat10WavePinDataRangePointersLoopback`, `Mat10TopoPinDataRangePointersBridge`, and `Mat10WaveMiniportFilterDescriptor` / `Mat10TopoMiniportFilterDescriptor` for Task 3.

- [x] **Step 1: Add failing descriptor and count checks to the verifier**

Append these checks before the headers exist:

```powershell
$wave = Join-Path $RepositoryRoot 'audio/sysvad/Mat10AudioSample/mat10wavtable.h'
$topo = Join-Path $RepositoryRoot 'audio/sysvad/Mat10AudioSample/mat10toptable.h'
foreach ($path in @($wave, $topo)) { if (-not (Test-Path -LiteralPath $path)) { throw "missing MAT10 table: $path" } }
Require-Match $wave 'MAT10_HOST_FORMAT_COUNT\s+5'
Require-Match $wave 'MAT10_STREAM_POINTER_COUNT\s+4'
Require-Match $wave 'MAT10_LOOPBACK_POINTER_COUNT\s+1'
Require-Match $topo 'MAT10_BRIDGE_POINTER_COUNT\s+2'
Require-Match $wave 'KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP'
foreach ($forbidden in @('DOLBY_DIGITAL','DOLBY_MAT20','DOLBY_MAT21','IEC61937_DTS','DTSX')) {
    Forbid-Match $wave $forbidden
    Forbid-Match $topo $forbidden
}
```

- [x] **Step 2: Run the table check to confirm RED**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: failure beginning `missing MAT10 table:`.

- [x] **Step 3: Implement the canonical tables and compile-time invariants**

Create `mat10wavtable.h` with these named constants, one host-format array of five entries, and `C_ASSERT` checks that keep table count and descriptor layout coupled:

```cpp
#define MAT10_DEVICE_MAX_CHANNELS       8
#define MAT10_HOST_FORMAT_COUNT          5
#define MAT10_STREAM_POINTER_COUNT       4
#define MAT10_LOOPBACK_POINTER_COUNT     1
#define MAT10_BRIDGE_POINTER_COUNT       2

C_ASSERT(sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == 52);
C_ASSERT(sizeof(MAT10_KSDATAFORMAT) == sizeof(KSDATAFORMAT) + 52);
C_ASSERT(SIZEOF_ARRAY(Mat10HostPinSupportedDeviceFormats) == MAT10_HOST_FORMAT_COUNT);
C_ASSERT(SIZEOF_ARRAY(Mat10WavePinDataRangePointersStream) == MAT10_STREAM_POINTER_COUNT);
C_ASSERT(SIZEOF_ARRAY(Mat10WavePinDataRangePointersLoopback) == MAT10_LOOPBACK_POINTER_COUNT);
```

Declare four stereo PCM `KSDATAFORMAT_WAVEFORMATEXTENSIBLE` entries at 44100, 48000, 88200, and 96000 Hz with 16 bits, 4-byte block align, 176400/192000/352800/384000 bytes/s, `KSAUDIO_SPEAKER_STEREO`, and PCM subtype. The fifth entry must be initialized as:

```cpp
{
  { sizeof(MAT10_KSDATAFORMAT), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
  { { WAVE_FORMAT_EXTENSIBLE, 8, 192000, 3072000, 16, 16, 34 },
    16, KSAUDIO_SPEAKER_7POINT1,
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP) },
  96000, 8, 0
}
```

Make stream data ranges exactly PCM plus MLP, expose four stream pointers, and make loopback data range/pointer arrays PCM-only with one pointer. Create `mat10toptable.h` with only two bridge ranges/pointers: `KSDATAFORMAT_SUBTYPE_ANALOG` and `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP`; set its `C_ASSERT(SIZEOF_ARRAY(Mat10TopoPinDataRangePointersBridge) == MAT10_BRIDGE_POINTER_COUNT)`.

- [x] **Step 4: Extend the verifier to inspect every exact descriptor field**

Add a PowerShell here-string-free parser that asserts the MAT10 initializer contains each required literal and that the table contains exactly five initializers. The checks must include `sizeof(MAT10_KSDATAFORMAT)`, `sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == 52`, `8`, `192000`, `3072000`, `16`, `34`, `KSAUDIO_SPEAKER_7POINT1`, `96000`, and `, 0`, plus all four PCM sample rates. It must also require the `C_ASSERT` statements above.

- [x] **Step 5: Run the table check to confirm GREEN**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: exit `0`; the tables advertise only four PCM formats plus one exact 52-byte MLP descriptor, with 4/1/2 pointer counts and no forbidden encoded subtype.

**Review boundary:** inspect the three data-range arrays and pointer arrays manually. Reject the task if any count/pointer names still refer to HDMI stock arrays, if the loopback carries MLP, or if any MAT20/MAT21/DTS/Dolby Digital token remains.

### Task 3: Wire one endpoint and enforce safe metadata-only MAT10 handling

**Files:**
- Create: `audio/sysvad/Mat10AudioSample/minipairs.h`
- Modify: `audio/sysvad/common.h`
- Modify: `audio/sysvad/EndpointsCommon/minwavert.cpp`
- Modify: `audio/sysvad/Mat10AudioSample/Mat10AudioSample.vcxproj`
- Modify: `audio/sysvad/scripts/Verify-Mat10Static.ps1`

**Interfaces:**
- Consumes: Task 2's filter descriptors and format arrays, existing `ENDPOINT_MINIPAIR`, and the shared WaveRT format-negotiation path.
- Produces: `g_RenderEndpoints` with exactly `&Mat10Miniports`, `g_CaptureEndpoints == NULL`, `g_cCaptureEndpoints == 0`, and the bounded `ValidateMat10Format`/`RecordMat10Metadata` behavior defined in Fixed Interfaces.

- [x] **Step 1: Add failing single-endpoint and safety checks**

Append these verifier requirements:

```powershell
$pairs = Join-Path $RepositoryRoot 'audio/sysvad/Mat10AudioSample/minipairs.h'
$common = Join-Path $RepositoryRoot 'audio/sysvad/common.h'
$waveRt = Join-Path $RepositoryRoot 'audio/sysvad/EndpointsCommon/minwavert.cpp'
foreach ($path in @($pairs, $common, $waveRt)) { if (-not (Test-Path -LiteralPath $path)) { throw "missing required file: $path" } }
Require-Match $pairs 'g_RenderEndpoints\[\].*=.*&Mat10Miniports'
Require-Match $pairs 'g_cRenderEndpoints\s*=\s*1'
Require-Match $pairs 'g_CaptureEndpoints\s*=\s*NULL'
Require-Match $pairs 'g_cCaptureEndpoints\s*=\s*0'
Require-Match $common 'eMat10RenderDevice'
Require-Match $waveRt 'FormatSize\s*==\s*sizeof\(MAT10_KSDATAFORMAT\)'
Require-Match $waveRt 'ConsumedBytes\s*%\s*16\s*==\s*0'
Forbid-Match $pairs 'CaptureMiniports|ENDPOINT_SOUNDDETECTOR_SUPPORTED|ENDPOINT_OFFLOAD_SUPPORTED'
Forbid-Match $waveRt 'Mat10.*(Capture|Ring|Payload|GetBuffer|CopyMemory)'
```

- [x] **Step 2: Run the endpoint check to confirm RED**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: failure beginning `missing required file:` for `minipairs.h`.

- [x] **Step 3: Implement the isolated endpoint pair and shared device type**

Add `eMat10RenderDevice` as a new unique value in the shared device enum without renumbering existing device values. Define `MAT10_METADATA_EVENT` exactly as in Fixed Interfaces; keep it stack-only at its trace/diagnostic emission site.

Create `minipairs.h` to include only `mat10toptable.h` and `mat10wavtable.h`, then define one `Mat10Miniports` pair using `eMat10RenderDevice`, names `TopologyMat10` and `WaveMat10`, `CreateHdmiMiniportTopology`, `CreateMiniportWaveRTSYSVAD`, `MAT10_DEVICE_MAX_CHANNELS`, and `ENDPOINT_LOOPBACK_SUPPORTED`. Set the module fields to `NULL, 0, NULL`.

```cpp
PENDPOINT_MINIPAIR g_RenderEndpoints[] = { &Mat10Miniports };
const ULONG g_cRenderEndpoints = 1;
PENDPOINT_MINIPAIR g_CaptureEndpoints = NULL;
const ULONG g_cCaptureEndpoints = 0;
C_ASSERT(SIZEOF_ARRAY(g_RenderEndpoints) == 1);
```

The driver project must include the new `minipairs.h` and exactly the parent source list in Task 1; do not compile a capture topology or endpoint source.

- [x] **Step 4: Implement the bounded format gate and metadata contract**

In `minwavert.cpp`, branch on `m_DeviceType == eMat10RenderDevice` before an extended-format cast. Require `bufferSize >= FIELD_OFFSET(KSDATAFORMAT, FormatSize) + sizeof(ULONG)`, read `FormatSize`, require it equals `sizeof(MAT10_KSDATAFORMAT)`, then check the exact audio type/specifier, MLP subtype, eight channels, 192000 Hz, 3072000 byte rate, 16-byte alignment, 16 bits, `cbSize=34`, 16 valid bits, `KSAUDIO_SPEAKER_7POINT1`, 96000 encoded rate, eight encoded channels, and zero encoded byte rate. Use a validated `MAT10_KSDATAFORMAT` pointer only after those checks; its `WaveFormat` member remains exactly 52 bytes.

    Record `Mat10MetadataFormatProbed` only for probe-time acceptance; a successfully initialized stream then emits `Mat10MetadataStreamCreated`, `Mat10MetadataRightsUnprotected`, `Mat10MetadataRun`, `Mat10MetadataWriteCommitted`, and `Mat10MetadataConsumed` in same-stream order. Write events carry the raw `PacketNumber`, `WriteFlags`, `EosPacketLength`, DMA-buffer bytes, and `NotificationCount` plus the independently derived logical frontier; consumption must be positive, 16-byte aligned, and wholly within that frontier. Rights restriction/failure, invalid geometry, unsupported packet semantics, format change, stop, and teardown emit metadata-only terminal/error evidence and permanently close that stream's evidence path. The event contains no pointer, audio byte, payload, capture ring, or IOCTL read field. Do not allocate a ring, retain an input pointer, invoke a render client, create IOCTLs, or copy sample data. Extend existing HDMI-specific conditionals to include `eMat10RenderDevice` only where HDMI DRM/format behavior is necessary.

- [x] **Step 5: Run the endpoint check to confirm GREEN**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: exit `0`; static checks prove exactly one render endpoint, zero capture endpoints, the native 116/52-byte before-dereference gate, and metadata-only rights/frontier/alignment evidence.

**Review boundary:** trace each `MAT10_METADATA_EVENT` initializer. Reject the task if any member carries a pointer or audio bytes, if a RUN event can be emitted before `KSSTATE_RUN`, if rights are accepted from default-zero state or another stream, if write frontier arithmetic trusts a precomputed field rather than raw contiguous callbacks, or if an unaligned/zero/out-of-frontier consumed count can yield a consumed event.

### Task 4: Build the dedicated INF/package surface and lifecycle validators

**Files:**
- Create: `audio/sysvad/Mat10Package/VibepolloAtmosMat10.inx`
- Create: `audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1`
- Modify: `audio/sysvad/Mat10Package/Mat10Package.vcxproj`
- Modify: `audio/sysvad/Mat10Package/Mat10Package.vcxproj.filters`
- Modify: `audio/sysvad/scripts/Verify-Mat10Static.ps1`

**Interfaces:**
- Consumes: `VibepolloAtmosMat10.sys` from Task 1 and the one endpoint's `TopologyMat10`/`WaveMat10` names from Task 3.
- Produces: a single-INF package surface, `Test-Mat10PackageIdentity`, and `Invoke-Mat10LifecycleValidation` that never performs device or boot mutation.

- [x] **Step 1: Add failing identity and dry-run checks**

Add these assertions to `Verify-Mat10Static.ps1`:

```powershell
$inf = Join-Path $RepositoryRoot 'audio/sysvad/Mat10Package/VibepolloAtmosMat10.inx'
if (-not (Test-Path -LiteralPath $inf)) { throw "missing MAT10 INF source: $inf" }
foreach ($required in @(
    'Root\\VibepolloAtmosMat10', 'VibepolloAtmosMat10.sys',
    'VibepolloAtmosMat10.cat', 'Vibepollo Atmos Project',
    'Vibepollo Atmos MAT10 Virtual HDMI', 'WaveMat10', 'TopologyMat10')) {
    Require-Match $inf ([regex]::Escape($required))
}
foreach ($forbidden in @('TabletAudioSample','sysvad.cat','SwapAPO','DelayAPO','KWSApo','AecApo','MAT20','MAT21','DTS','DOLBY_DIGITAL')) {
    Forbid-Match $inf $forbidden
}
```

- [x] **Step 2: Run the package check to confirm RED**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
```

Expected: failure beginning `missing MAT10 INF source:`.

- [x] **Step 3: Implement the unique INF and package linkage**

Create `VibepolloAtmosMat10.inx` with `CatalogFile=VibepolloAtmosMat10.cat`, `HKR,,Driver,,VibepolloAtmosMat10.sys`, a root-enumerated model for `Root\VibepolloAtmosMat10`, and `[Strings]` values exactly:

```ini
ProviderName = "Vibepollo Atmos Project"
MfgName = "Vibepollo Atmos Project"
DeviceDesc = "Vibepollo Atmos MAT10 Virtual HDMI"
```

Declare only the audio, render, realtime, and topology interfaces for `WaveMat10`/`TopologyMat10`. Do not add capture, APO, keyword, MIDI, mixer, sideband, or stock SysVAD interfaces. Include the new INX in `Mat10Package.vcxproj` and its filters; leave it as the package project's sole INF source and sole driver project reference.

- [x] **Step 4: Implement safe lifecycle validation**

Create `Validate-Mat10Lifecycle.ps1` with this command grammar and exact identity guard:

```powershell
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)] [ValidateSet('Install','Uninstall','Recover')] [string] $Action,
    [Parameter(Mandatory)] [ValidateSet('ValidateOnly','WhatIf')] [string] $Mode,
    [Parameter(Mandatory)] [string] $InfPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$requiredId = 'Root\VibepolloAtmosMat10'
$requiredInf = 'VibepolloAtmosMat10.inf'
if ((Split-Path -Leaf $InfPath) -ne $requiredInf) { throw "unexpected INF leaf: $InfPath" }
if (-not (Select-String -LiteralPath $InfPath -Pattern ([regex]::Escape($requiredId)) -Quiet)) {
    throw "exact root identity missing: $requiredId"
}
if ($Mode -eq 'ValidateOnly') { Write-Output "VALIDATED $Action $requiredId"; exit 0 }
if (-not $WhatIfPreference) { throw 'WhatIf mode must be invoked with -WhatIf' }
Write-Output "WHATIF $Action $requiredId"
```

Do not call `pnputil`, `devcon`, `bcdedit`, `Restart-Computer`, `Start-Service`, `sc.exe`, or `Add-WindowsDriver` anywhere in this script. In `WhatIf` mode it must emit only the identity-confirmed intended action; in `ValidateOnly` mode it validates paths/content and emits no action command.

- [x] **Step 5: Run package and lifecycle checks to confirm GREEN**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Install -Mode ValidateOnly -InfPath audio/sysvad/Mat10Package/VibepolloAtmosMat10.inf
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Recover -Mode WhatIf -InfPath audio/sysvad/Mat10Package/VibepolloAtmosMat10.inf -WhatIf
```

Expected: static verifier exits `0`; lifecycle outputs exactly `VALIDATED Install Root\VibepolloAtmosMat10` then `WHATIF Recover Root\VibepolloAtmosMat10`; no OS configuration changes occur.

**Review boundary:** reject the task if either script reaches a mutating command, accepts a nonexact root ID, or the package INF contains a stock catalog/service/binary identity.

### Task 5: Build, verify the generated package, and record the pre-install evidence boundary

**Files:**
- Modify: `audio/sysvad/scripts/Verify-Mat10Static.ps1`
- Modify: `audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1`
- Modify: `docs/superpowers/plans/2026-08-15-sysvad-mat10-package.md` only if a command path proved inaccurate during this task

**Interfaces:**
- Consumes: all prior project, driver, package, and validation artifacts.
- Produces: a verified `Release|x64` package directory and a clear distinction between static package proof and the later authorized live-driver evidence collection.

- [x] **Step 1: Add failing generated-artifact checks before the build**

Extend `Verify-Mat10Static.ps1` so `-PackageRoot` requires exactly one `VibepolloAtmosMat10.sys`, one `VibepolloAtmosMat10.inf`, and one `VibepolloAtmosMat10.cat`; rejects `tabletaudiosample.sys`, `sysvad.cat`, every APO binary, and all forbidden codec tokens in the generated INF. It must also require the generated INF model to contain `Root\VibepolloAtmosMat10` and the endpoint name.

- [x] **Step 2: Run the artifact check to confirm RED**

Run before building:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location) -PackageRoot audio/sysvad/Mat10Package/x64/Release
```

Expected: failure because the `Release|x64` package directory does not yet contain all three uniquely named artifacts.

- [x] **Step 3: Build only the dedicated package with warnings as errors**

Run:

```powershell
& 'C:\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe' audio\sysvad\sysvad.sln /t:Mat10Package /p:Configuration=Release /p:Platform=x64 /m /warnaserror
if ($LASTEXITCODE -ne 0) { throw "Mat10Package build failed: $LASTEXITCODE" }
```

Expected: MSBuild produces `VibepolloAtmosMat10.sys`, `.inf`, and `.cat` under the Mat10 package's `x64\Release` output; it does not build a stock package as the selected target.

- [x] **Step 4: Run InfVerif and all static/package checks to confirm GREEN**

Run:

```powershell
$inf = (Get-ChildItem -LiteralPath audio\sysvad\Mat10Package -Recurse -Filter VibepolloAtmosMat10.inf | Select-Object -First 1 -ExpandProperty FullName)
if (-not $inf) { throw 'generated VibepolloAtmosMat10.inf not found' }
& InfVerif.exe /h /v $inf
if ($LASTEXITCODE -ne 0) { throw "InfVerif failed: $LASTEXITCODE" }
pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location) -PackageRoot (Split-Path -Parent $inf)
```

Expected: both commands exit `0`. The generated package has only the exact driver/catalog/INF identities and all descriptor, endpoint, pointer-count, forbidden-subtype, and metadata-only checks remain green.

- [x] **Step 5: Validate the evidence boundary without running a driver**

Run:

```powershell
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Install -Mode ValidateOnly -InfPath $inf
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Install -Mode WhatIf -InfPath $inf -WhatIf
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Uninstall -Mode ValidateOnly -InfPath $inf # expected fail-closed
pwsh -NoProfile -File audio/sysvad/scripts/Validate-Mat10Lifecycle.ps1 -Action Recover -Mode WhatIf -InfPath $inf -WhatIf # expected fail-closed
```

The Install rehearsal commands exit `0` and produce only identity/baseline/action-model output. Uninstall and Recover intentionally reject with a fail-closed error because Stage 2 has no genuine installer-authenticated, tamper-evident receipt. There is no accepted/RUN/consumed *live-driver* record in this stage because no driver is installed or started. The later runtime stage must require the complete same-stream sequence `STREAM_CREATED(116/52) -> RIGHTS_UNPROTECTED -> KSSTATE_RUN -> WRITE_COMMITTED` derived from raw contiguous callbacks -> positive aligned `CONSUMED` within the independently recomputed frontier, while rejecting any payload or capture evidence path.

**Review boundary:** inspect build target selection, generated package contents, InfVerif output, and all lifecycle transcript lines. Reject the task if test signing, installation, reboot, a device open, a live stream, or any audio-byte artifact occurs.

## Final Verification Checklist

- [x] `pwsh -NoProfile -File audio/sysvad/scripts/Verify-Mat10Static.ps1 -RepositoryRoot (Get-Location)` exits `0` under Windows PowerShell 5.1 and pwsh.
- [x] The dedicated `Mat10Package` `Release|x64` build through `C:\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe` exits `0` under `/warnaserror`, with the required `EndpointsCommon`, `Mat10AudioSample`, and package targets.
- [x] `InfVerif.exe /h /v` exits `0` and reports `INF is VALID` for the generated `VibepolloAtmosMat10.inf`; Inf2Cat reports no errors or warnings.
- [x] `Verify-Mat10Static.ps1 -PackageRoot <generated-package-directory>` confirms exactly one uniquely named SYS/INF/CAT, source/package SYS equality, frozen binding, catalog membership, and rejects stock, APO, sideband, MAT20, MAT21, DTS, and Dolby Digital surfaces.
- [x] The MAT10 source table has four PCM host entries plus exactly one 52-byte MLP entry; stream/loopback/bridge pointer counts are 4/1/2 and max channels is 8.
- [x] `ValidateMat10Format` checks `FormatSize` before all extended-field dereferences and rejects short, zero, oversized, compact/84-byte, wrong-wrapper-size, or noncanonical MLP descriptors while requiring its embedded IEC 61937 payload to be exactly 52 bytes.
- [x] The endpoint arrays contain exactly one `eMat10RenderDevice` render pair and no capture pair; no APO or sideband macro/source is compiled.
- [x] Metadata evidence contains only fixed metadata fields: probe/creation, authoritative rights, RUN, raw write callback/frontier, aligned consumption, and terminal/error state. It cannot contain payload/capture bytes, ring buffers, pointers, or an IOCTL read path. The compiled state harness and mutation suites pass.
- [x] Install, uninstall, and recovery scripts are exercised only with `-ValidateOnly` or `-WhatIf`; no test-signing, install, reboot, boot-policy, or Windows spatial-setting mutation occurs. Uninstall/Recover fail closed without a genuine installer-authenticated receipt.
- [x] `git diff --check -- docs/superpowers/plans/2026-08-15-sysvad-mat10-package.md docs/superpowers/specs/2026-08-15-atmos-transport-design.md` exits `0` after the documentation change.

## Self-Review Record

- **Spec coverage:** Tasks 1-5 cover the dedicated project/package identity, narrow source map, one render/zero capture endpoint graph, exact descriptor and table counts, bounded `FormatSize` validation, the full metadata-only stream/rights/frontier/terminal contract, generated package/INF verification, compiled/static mutation coverage, and dry-run-only lifecycle policy from the transport design and this feasibility scope.
- **Completeness scan:** Every Stage-2 task and verification checkbox is checked against the frozen source/package evidence. Deferred work is explicit: no signed install/load/runtime test, kernel tap or control device, payload handoff, packet transport, client renderer, clock recovery, end-to-end Atmos, or public release is claimed here.
- **Type consistency:** `eMat10RenderDevice`, `MAT10_METADATA_EVENT`, `ValidateMat10Format`, `RecordMat10Metadata`, `Mat10Miniports`, `g_RenderEndpoints`, the state harness, binding manifest, and validation scripts use the current native 116/52 and full event/state contracts.
- **Concrete unresolved boundary:** Live driver behavior and Uninstall/Recover remain intentionally unavailable until a separately authorized signed-driver stage and genuine installer-authenticated receipt exist. Stage 1 machine-side capability/carrier evidence and Stage 2 source/build/package evidence are complete; neither proves downstream native Atmos provenance or a new receiver lock.

Stage-2 plan complete and saved to `docs/superpowers/plans/2026-08-15-sysvad-mat10-package.md`. Execution options are specified by the repository workflow; the next stages require separately authorized signing/install/runtime controls and are not implied by this completed plan.

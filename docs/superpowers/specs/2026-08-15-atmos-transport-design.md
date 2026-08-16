# Capability-Gated Dolby MAT Transport

**Status:** Approved for staged local prototyping  
**Date:** 2026-08-15  
**Primary upstream:** [Nonary/Vibepollo](https://github.com/Nonary/Vibepollo)  
**Client upstreams:** [Nonary/moonlight-qt](https://github.com/Nonary/moonlight-qt) and [Nonary/moonlight-common-c](https://github.com/Nonary/moonlight-common-c)

**Current implementation status (2026-08-16):** Stages 1 and 2 are complete at their stated evidence boundaries. Stage 1 has the no-write capability/spatial carrier-state evidence for the intended laptop HDMI route. Stage 2 is GREEN and pushed at commit `98a0fc0` (`primez-x/Windows-driver-samples`, branch `agent/atmos-mat-endpoint`): the isolated MAT10 endpoint, same-binary read-only control device, bounded kernel payload ring, exact consumed-range publication, fixed record ABI, lifecycle/rights/frontier/terminal tests, WDK API validation, INF/catalog signability, and compiled state harness all pass. The package remains unsigned and was not installed or loaded. The Vibepollo host/client tree contains the matching record reader, TLS 1.3 relay framing, exclusive MAT10 client writer, capability probes, and focused tests. Steam/Realtek restoration is separately deployed and upstreamed; an end-to-end Atmos run now depends on publisher signing and packaging the driver.

The signed-NVIDIA experiment was completed on 2026-08-16 and is not an exact-Atmos route on this host. NVIDIA's installed, signed `nvaudcap64v.dll` factory and 1.9 interface are usable from an ordinary process, and its NVVAD session endpoint can be registered and captured, but the matching signed DLL and kernel driver encode only one to eight channels of PCM. A live 8-channel endpoint succeeded; 12-channel PCM and MAT 1.0/2.0/2.1 were rejected, and Windows reported Dolby Atmos for Home Theater unsupported. NVIDIA's official `NvAPI_GPU_SetEDID` software-forced-EDID API was also exercised against an unused RTX 5090 output with the client's freshly acquired EDID and returned `NVAPI_NOT_SUPPORTED`, matching NVIDIA's documented GeForce exclusion. The endpoint and all three default roles were then restored and verified.

The primary exact-Atmos route is therefore the isolated SysVAD MAT endpoint plus the existing host relay and client exclusive writer, with the driver package signed and published by Vibepollo's driver publisher. Secure Boot remains enabled; no unsigned package is installed on the target host. The NVIDIA integration remains useful as a signed, low-latency PCM fallback and as a capability probe, but it must never label its 7.1 output as native Atmos. The captured `Beyond TV` EDID is a test fixture only; no EDID, endpoint ID, or receiver identity may be compiled into Vibepollo or used as a production default.

A soundbar `Atmos` badge or continuously present carrier indication remains route/carrier-state evidence only: it is not proof of native content provenance, a newly acquired receiver lock, or bit-exact relay.

## Decision

Add an experimental, backwards-compatible opaque MAT path beside the existing GameStream Opus path. The paired client discovers its currently selected physical HDMI/eARC sink and sends a validated live capability result over the authenticated control channel. When both peers commit MAT10, Windows Spatial Sound and Dolby Access render to the publisher-signed host MAT endpoint, Vibepollo reads exact consumed carrier ranges from the endpoint's bounded tap, and the client writes those bytes unchanged to its selected HDMI endpoint in exclusive mode.

The negotiated order is: exact MAT10 through the publisher-signed endpoint; signed NVIDIA 7.1 PCM as an explicitly non-native fallback; then existing Opus stereo/5.1/7.1. A registered endpoint, successful shared-mode open, or soundbar badge alone never proves exact MAT or native-object provenance.

Atmos is available to the user only when both peers pass explicit capability checks. A pre-launch failure leaves the session on the existing stereo/5.1/7.1 Opus path. After an opaque epoch is committed, a route or transport failure stops opaque audio while preserving video/input; version 1 never changes the live stream back to Opus without a new session.

This is not a 12-channel extension to Opus. Dolby MAT's eight 192 kHz carrier lanes are a non-PCM transport representation, not eight speaker channels. Treating MAT as 7.1 PCM would destroy its object and height metadata.

## Goals

- Preserve the host OS's Dolby Atmos for Home Theater output through a Windows laptop's HDMI/eARC path to a TV and soundbar.
- Preserve static 7.1.4 beds and dynamic-object metadata by relaying the MAT carrier bit-for-bit.
- Discover and proxy the active client's sink EDID per session; never hardcode a receiver EDID.
- Expose Atmos in the client only when the selected physical output path is currently capable and configured for it.
- Keep unmodified Vibepollo and Moonlight peers fully interoperable through the existing Opus path.
- Detect endpoint changes and fail safely without emitting arbitrary or malformed carrier bytes.
- Keep added latency small, measured, and stable enough for game streaming.
- Build the solution from open source plus the user's licensed Windows/Dolby runtime. Do not implement a Dolby encoder or decoder.

## Non-goals

- Decoding, encoding, transcoding, inspecting, or editing Dolby program content.
- Supporting protected or DRM-restricted playback.
- Claiming compatibility with Atmos for Headphones, Windows Sonic, or DTS spatial formats.
- Inferring capability from an EDID name, channel count, Dolby Access installation, or a receiver badge alone.
- Shipping a captured client EDID, endpoint ID, or receiver identity as a built-in default.
- Adding an external HDMI capture, forwarding, EDID-emulation, or AV-over-IP device.
- Modifying or replacing NVIDIA's signed driver, INF, catalog, or system DLL.
- Shipping public Dolby branding or a redistributable driver package before licensing and trademark review.
- Replacing the existing Opus audio implementation for normal sessions.

## Verified starting point

Vibepollo currently captures shared-mode 48 kHz float PCM and encodes only 2-, 6-, or 8-channel Opus. Moonlight's common audio contract and Qt UI have the same 2/6/8-channel assumptions. The Steam Streaming Speakers driver does not advertise the HDMI/MAT endpoint required for Dolby Atmos for Home Theater.

Microsoft's SysVAD TabletAudioSample already contains an HDMI endpoint topology and MAT10/MLP, MAT 2.0, and MAT 2.1 render data ranges with an eight-lane, 16-bit, 192 kHz carrier. Its public loopback pin remains PCM-only. The Stage-2 package isolates the exact MAT10 endpoint and proves the producer/state metadata contract without copying audio bytes; the future relay stages still require a driver-internal tap of bytes consumed from the WaveRT render buffer.

The reviewed no-write probe measured the intended laptop path, `Beyond TV (NVIDIA High Definition Audio)`, as active HDMI with Dolby Atmos for Home Theater active. Its exact exclusive-mode intersection is MAT10 only: MAT10 support and initialization return `S_OK`, while MAT20 and MAT21 return `AUDCLNT_E_UNSUPPORTED_FORMAT`. Therefore the SysVAD-derived source endpoint must retain its existing exact MAT10/MLP data range and prove that the licensed Windows Dolby producer actually selects MAT10. The opaque session may be offered only when the observed host source profile and this client sink profile intersect exactly.

The fixed MAT carrier rate is:

```
192000 frames/s * 8 lanes * 16 bits = 24,576,000 bits/s
                                      3,072,000 bytes/s
```

## Repository boundaries

The work is split along existing project and license boundaries:

| Repository | Responsibility | Upstream/license boundary |
| --- | --- | --- |
| `Nonary/Vibepollo` fork | Negotiation, host capability state, driver reader, packetization, FEC, encryption, clock control, Opus fallback | Preserve Vibepollo's existing license and architecture |
| `Nonary/moonlight-common-c` fork | Versioned wire contract, receive/reorder/recovery state, feedback messages, legacy compatibility | Keep the extension absent for standard peers |
| `Nonary/moonlight-qt` fork | Windows HDMI probing, conditional UI, exclusive MAT renderer, hot-plug handling, diagnostics | Use Nonary's latency-focused client as the baseline |
| Separate SysVAD-derived repository | **Implemented:** isolated virtual HDMI render endpoint, MAT10 format advertisement, metadata/state evidence. **Future:** internal capture ring, tap, controllable WaveRT clock | Retain Microsoft sample notices and MS-PL-derived source separation |

The driver source must not be copied into the Vibepollo repository. A future host reader will communicate with it through a documented, versioned device-control interface; no such control device or payload reader exists in Stage 2.

## End-to-end flow

```text
Rejected signed-NVIDIA exact route (retained as PCM fallback only):

Client active HDMI/eARC sink -> validated live EDID + capability offer
        |
        v
Vibepollo session + NVIDIA NVVAD endpoint (maximum 8-channel PCM)
        |
        v
No MAT/Atmos endpoint exposed on the tested signed stack
        |
        `--> exact 7.1 PCM fallback -> existing/authenticated Vibepollo audio path

Publisher-signed exact-MAT route:

Game / Windows spatial API
        |
        v
Windows Spatial Sound + Dolby provider on host
        |
        v
SysVAD-derived virtual HDMI WaveRT render endpoint (exact negotiated profile; canonical MAT10 for the measured prototype)
        |
        +--> simulated hardware consumes cyclic-buffer bytes
        |         |
        |         v
        |    [FUTURE STAGE 4] kernel capture tap/ring + timing metadata
        |         |
        v         v
Vibepollo host reader [FUTURE] -> MAT packetizer [FUTURE] -> AES-128-GCM [FUTURE] -> FEC/retransmit [FUTURE]
        |
        v
Moonlight common receive/recovery [FUTURE] -> bit-exact carrier stream
        |
        v
Moonlight Qt WASAPI exclusive renderer [FUTURE]
        |
        v
Laptop HDMI -> TCL TV/eARC -> Atmos-capable soundbar
```

## Capability contract

### Dynamic sink-EDID probe contract (signed-NVIDIA experiment and test fixtures)

The EDID is session data, not configuration. The client obtains it from the monitor interface associated with the exact currently selected HDMI/eARC render route during discovery and repeats the acquisition immediately before commit. A saved EDID may be used only as an automated-test fixture or a manually identified bench-programming artifact.

Before offering the route, the client must:

1. require the EDID base header, a byte length exactly equal to `(1 + extension_count) * 128`, and no more than the 255 extension blocks (256 total blocks) representable by the base descriptor;
2. verify the checksum of every 128-byte block and parse CTA/DisplayID bounds without trusting display names or free-form strings;
3. bind the EDID bytes and SHA-256 to the exact client render endpoint, monitor interface, active Atmos format, and route bookends;
4. send those values only inside the authenticated paired-session control channel; and
5. invalidate the offer on endpoint change, monitor hot-plug, EDID hash change, sleep/resume, active-spatial-format change, or default-route change.

The host validates the descriptor again before any NVIDIA experiment. The tested GeForce driver cannot accept a forced EDID through the official API and NVVAD cannot expose MAT, so production exact-MAT negotiation does not depend on host EDID injection. A future signed-driver implementation may only revive this path after the exact client EDID is applied and read back byte-for-byte and the resulting endpoint independently passes MAT format initialization. A mismatch, stale route, unsupported audio block, NVIDIA API/version mismatch, endpoint registration failure, or unavailable signed stack selects the stored fallback before launch.

The EDID contract never authorizes arbitrary registry edits, a bundled receiver descriptor, direct NVIDIA driver/INF modification, or unchecked bytes passed into a kernel component. On teardown Vibepollo releases the session-specific NVIDIA endpoint and virtual-display binding and restores the exact pre-session audio defaults; it does not rewrite the client's sink EDID.

### Client discovery gate (future client implementation; Stage-1 probe evidence only)

The client may display the experimental Atmos choice only when every condition below is true for the currently selected render endpoint:

1. The endpoint is active.
2. `PKEY_AudioEndpoint_FormFactor` equals `DigitalAudioDisplayDevice` and the physical connector is positively identified as HDMI. Require `PKEY_AudioEndpoint_JackSubType == KSNODETYPE_HDMI_INTERFACE`; an implementation may corroborate it with `IKsJackSinkInformation::GetJackSinkInformation().ConnType == KSJACK_SINK_CONNECTIONTYPE_HDMI`. A missing, malformed, DisplayPort, or inconclusive subtype fails closed.
3. Spatial configuration is linked to this exact endpoint. `IMMDevice::GetId` and WinRT `MediaDevice`/`DeviceInformation` render-device IDs are distinct opaque namespaces: never pass the former to `SpatialAudioDeviceConfiguration`, parse either namespace, or infer linkage from textual similarity. For default selection, bookend endpoint, spatial, and MAT observation by sampling all three Core Audio default roles plus the WinRT Default and Communications render IDs before and after it; authorize the link only when the selected endpoint and the nonempty opaque WinRT ID remain exact throughout. Literal equality between a WinRT ID and the MMDevice ID is rejected as a namespace-collapse error, not treated as linkage. An explicit endpoint needs an API-provided link to its WinRT render-device ID; an unlinked or changed endpoint fails closed.
4. `SpatialAudioDeviceConfiguration.IsSpatialAudioSupported` is true.
5. `IsSpatialAudioFormatSupported(DolbyAtmosForHomeTheater)` is true.
6. `ActiveSpatialAudioFormat` exactly equals Dolby Atmos for Home Theater GUID `{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}`.
7. WASAPI exclusive-mode `IsFormatSupported` accepts at least one exact IEC 61937 descriptor from the peer-supported intersection: `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP` (MAT10), `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20`, or `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21`.
8. The host advertises the versioned opaque-audio extension.

The UI must not use the default spatial format as a substitute for the active format. Atmos for Headphones, Windows Sonic, DTS, inactive endpoints, Bluetooth, DisplayPort, and ordinary multichannel PCM do not satisfy the gate. Windows groups HDMI and DisplayPort under `DigitalAudioDisplayDevice`, so the form-factor property alone is insufficient.

At stream start, the client repeats the checks and performs an exclusive-mode `IAudioClient::Initialize` preflight for the negotiated exact format. The mode is committed only after initialization succeeds. Discovery makes the choice visible; the start-time preflight authorizes actual use. The capability report uses schema version 2 and records link provenance plus the opaque WinRT input and API-returned configuration IDs separately from the opaque MMDevice ID.

### Host activation gate (future host integration; Stage-2 metadata contract only)

The host may accept an opaque MAT request only when:

1. The paired client offered protocol version 1 and an exact MAT subtype.
2. The virtual driver interface and ABI version match the host reader.
3. The virtual HDMI endpoint is active and selected for the stream.
4. Dolby Atmos for Home Theater is supported and currently active on that endpoint.
5. The endpoint reports a native static-object capability mask containing every required 7.1.4 bit and exactly 20 dynamic objects. Extra advertised native static types are ignored and never activated by the test.
6. The driver reports that the complete negotiated 52-byte descriptor is the exact accepted stream format, that same stream has reached `KSSTATE_RUN`, and at least one aligned carrier frame has been consumed.
7. The capture ring is healthy before the first network packet is sent.

Public Windows APIs do not allow Vibepollo to activate a spatial provider it does not own. The prototype therefore expects a one-time user selection of Dolby Atmos for Home Theater in Windows/Dolby Access. Vibepollo verifies that state and explains a failed preflight; it does not use undocumented registry edits.

Client sink support for MAT10, MAT20, or MAT21 does not prove that the host source emits that profile. The host gate's active-render-format observation must match the negotiated profile, and the later legal OS-generated spatial-stream test must independently prove only that the machine-side spatial API completes on the intended stable route. Windows endpoint properties and audio-engine logs do not attest the downstream eARC decoder state, so the test records the exact run window and correlates it with the TCL/soundbar's physical or on-screen `Dolby Atmos` indication. If that indication is already continuously present whenever Windows Atmos is enabled, it is not evidence that the test acquired a new receiver lock. Because Windows Spatial Sound can mix non-spatial applications into its real-time Atmos output, the receiver badge proves only the route's carrier state, never native-content provenance or absence of upmixing. Source provenance and preservation require the later exact host-tap/client-pre-render hash match plus an exclusive MAT writer that never invokes the client spatial renderer.

### Negotiated descriptor (future wire contract; MAT10 shape implemented in Stage 2)

The protocol represents opaque audio separately from the legacy channel layout:

```text
codec:          OPAQUE_IEC61937
profile:        MAT10 | MAT20 | MAT21
sample_rate:    192000
carrier_lanes:  8
sample_bits:    16
block_align:    16
byte_rate:      3072000
version:        1
```

The Windows preflight builds the documented 52-byte `WAVEFORMATEXTENSIBLE_IEC61937` form: `WAVE_FORMAT_EXTENSIBLE`, `nChannels=8`, `nSamplesPerSec=192000`, `nAvgBytesPerSec=3072000`, `nBlockAlign=16`, `wBitsPerSample=16`, `cbSize=34`, valid bits `16`, and `KSAUDIO_SPEAKER_7POINT1`. The decoded-content fields are `dwEncodedSamplesPerSec=96000`, `dwEncodedChannelCount=8`, and `dwAverageBytesPerSec=0`. MAT10 (`KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP`) uses `{0000000C-0CEA-0010-8000-00AA00389B71}`; MAT20 uses `{0000010C-0CEA-0010-8000-00AA00389B71}`; MAT21 uses `{0000030C-0CEA-0010-8000-00AA00389B71}`.

An exact MAT version is required. Support for any of MAT10, MAT20, and MAT21 never implies support for either other profile. The sink selects the highest independently ready profile in order MAT21, MAT20, MAT10, subject to the peer/source intersection. The legacy channel-count and Opus mapping fields remain unchanged and are ignored only after both peers explicitly commit the opaque descriptor.

## Virtual HDMI driver

### Endpoint

Start from the SysVAD TabletAudioSample HDMI wave and topology descriptors. The Stage-2 feasibility package exposes only the PCM modes needed for Windows endpoint setup plus one canonical 52-byte MAT10/MLP descriptor matching the measured client sink. MAT20/MAT21 are not present in the enabled Stage-2 source/package surface; a future independently measured profile must be reintroduced as a separately reviewed build profile, never inferred from this MAT10 package. Do not add the stock omitted MAT20/MAT21 topology pointers: advertising an unrelayable source profile could cause the licensed producer to select it. Set `HDMI_DEVICE_MAX_CHANNELS` to eight for the HDMI miniport while leaving the existing two-channel PCM host/loopback maxima unchanged. Add an exact MAT validator; subtype equality alone is insufficient because every field of the negotiated 52-byte descriptor is part of the opaque transport contract.

Package only this HDMI feasibility endpoint under a unique root hardware ID, service name, catalog, provider, and endpoint name. Do not install the stock componentized SysVAD package, its unrelated speaker/headphone/SPDIF/microphone/sideband endpoints, APO extensions, or keyword-detector components. A future signed install rehearsal must record the exact root instance ID and assigned `oemN.inf`; rollback must remove only those recorded identities and restore the captured boot, certificate, default-route, and spatial-state baseline. Stage 2 has no genuine installer receipt and therefore does not claim that install/rollback contract was exercised.

**Stage-2 delivery:** The isolated `Mat10AudioSample`/`Mat10Package` source and unsigned `Release|x64` package implement the one-render/zero-capture MAT10 endpoint, exact native 116-byte wrapper plus 52-byte wave descriptor, and metadata/state reducer. Build, InfVerif/Inf2Cat, source/package hash binding, static checks, state/concurrency tests, mutation tests, and read-only lifecycle fixtures are independently GREEN. This is a build/package result, not proof that Windows installed the endpoint, selected it as an active spatial provider, or produced live MAT bytes.

The future Stage-3 driver kill test uses an otherwise minimally modified endpoint. It must prove all of the following before transport development depends on it:

- Windows exposes Dolby Atmos for Home Theater for the endpoint.
- The format becomes the active spatial format after user activation.
- the spatial client reports a native static-object capability mask containing every required 7.1.4 bit and exactly 20 dynamic objects; extra advertised native static types are ignored and never activated by the test;
- one boot-unique stream ID records this exact order from callbacks on that same successfully initialized miniport stream: `STREAM_CREATED` with the copied canonical MAT10 format (`sizeof(KSDATAFORMAT) == 64`, `KSDATAFORMAT::FormatSize == sizeof(MAT10_KSDATAFORMAT) == 116`, embedded `sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == 52`), an authoritative `RIGHTS_UNPROTECTED` result, `KSSTATE_RUN`, a monotonic `WRITE_COMMITTED` frontier, and a positive 16-byte-aligned `CONSUMED` range that does not cross that frontier. Probe-time format acceptance is recorded separately as `FORMAT_PROBED` and can never satisfy `STREAM_CREATED`.

If the Dolby provider never selects MAT for this endpoint, the opaque-relay architecture is not viable with this virtual-driver model. The fallback research path is a client-side spatial renderer fed with 7.1.4 PCM, with the explicit loss of dynamic objects.

### Internal capture tap (future Stage 4; not implemented)

The public SysVAD loopback pin is not used for MAT. After the metadata-only eligibility gate proves the producer's commit semantics, a future miniport tap would copy bytes as its simulated hardware consumes the WaveRT cyclic buffer and publish them through a nominal 2 MiB nonpaged, kernel-owned fixed-slot SPSC ring: 64 slots with at most 32 KiB of payload each plus fixed descriptors. Stage 2 does not allocate this ring, copy audio bytes, expose a control device, or provide a reader. The producer must never wait for user mode. It must reserve every slot required for an advance before copying anything; if the full advance cannot fit, it must publish no payload and poison that KS stream. Each future record would contain:

```text
ABI version
driver generation (64-bit)
boot-unique stream ID (64-bit)
exact MAT subtype
first carrier-frame index (64-bit)
host QPC and QPC frequency
payload byte count
flags: start, discontinuity, format-change, stop
payload, always a multiple of 16 bytes
```

The future tap must never decode, inspect Dolby payload semantics, resample, apply gain, synthesize missing bytes, or publish data not consumed by the render engine. Ring exhaustion, possible source overwrite, non-aligned advancement, reader loss, failed copy, rights restriction, pause, format change, or teardown must atomically advance the driver generation, record one terminal reason, and permanently disable payload publication for that KS stream while simulated hardware consumption continues. Version 1 must never resume from an arbitrary 16-byte boundary; it must require the host to recreate the OS/Dolby MAT stream.

**Future tap contract:** Exactly one active canonical MAT stream may own the tap. The stream must track a monotonic committed linear frontier from PortCls `SetWritePacket` notifications rather than treating the cyclic write offset or elapsed time as proof that bytes are valid. `SetWritePacket` must be accepted only on the exact stream object that passed the copied native 116/52-byte MAT validator and received its boot-unique stream ID after successful `NewStream` initialization; externally synthesized metadata and probe callbacks are never eligible evidence.

The callback contract is deliberately stricter than the most permissive WaveRT behavior. The driver records the exact DMA-buffer byte count and `NotificationCount` returned by the successful notification-buffer allocation, requires a nonzero count that divides the DMA buffer exactly, and defines `packet_bytes = dma_buffer_bytes / NotificationCount`; `packet_bytes` must be a nonzero multiple of the 16-byte carrier frame. The first committed packet must be `PacketNumber == 0`. Each later normal callback must equal the previous 32-bit packet number plus one modulo 2^32. A 64-bit logical packet counter is advanced only for that exact successor, so the committed half-open range is `[logical_packet * packet_bytes, (logical_packet + 1) * packet_bytes)`. The 32-bit wrap is accepted only at the exact successor boundary; a duplicate, backward value, skip, ambiguous jump, integer overflow, buffer/count change, or callback before stream identity/allocation is established poisons the stream instead of guessing. Although Windows permits packet numbers to skip in some resynchronization cases, version 1 intentionally stops on any skip because it cannot prove the omitted ranges were validly committed; the diagnostic classifies this as `UNSUPPORTED_PRODUCER_PACKET_SEMANTICS`, never as successful evidence or silent payload corruption.

For `KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM`, no flag bits other than EOS are accepted. `EosPacketLength` must be no greater than `packet_bytes` and must be 16-byte aligned; the final frontier becomes `logical_packet * packet_bytes + EosPacketLength`, including the documented valid zero-length EOS case. EOS latches the frontier and any later write callback or attempted consumption beyond it poisons the stream. `EosPacketLength` must be zero for a non-EOS callback. The miniport's reported packet count and presentation position must remain consistent with this logical-packet interpretation, and stop resets the per-stream packet sequence only by destroying that stream identity. The eligibility gate first proves that the actual Dolby producer supplies this contiguous authoritative commit sequence; if it does not, tap development stops until another PortCls-authoritative signal is identified.

For each simulated DMA advance, the miniport uses a 64-bit QPC-based carrier-frame accumulator at 192,000 frames/s, snapshots the old linear position and proposed new linear end, and requires a positive displacement that is 16-byte aligned, no larger than the DMA buffer, and no greater than the committed linear frontier. It reserves every output slot, copies exactly the half-open range `[oldLinearPosition, newLinearEnd)` including ordered circular-buffer wraps, release-publishes all records, and only then publishes the new play position. A failed validation or copy publishes no partial payload. The 16-byte unit is named `carrier_frame`; it is alignment and clock accounting, not a parsed Dolby access-unit boundary. The record timestamp is the scheduled consumption time of its first carrier frame, derived from the pre-advance clock state.

The future tap must increment out-of-band `stream_generation`, `dropped_bytes`, and `discontinuity_count` fields returned with every read/status request, so a full ring cannot hide its own failure. The driver generation is a local lifetime counter and must never be reused as the later cryptographic/network epoch.

The future tap must be fail-closed on PortCls content rights. Each MAT stream begins in `RIGHTS_UNKNOWN`; zero-initialized rights fields are never treated as authorization. Payload eligibility requires both the stream rights and a successfully refreshed mixed-rights result from the real content-ID/miniport rights callbacks to establish `CopyProtect == false` and `DigitalOutputDisable == false`. A metadata event may be emitted only after that refresh succeeds on the same stream ID; default-zero fields and synthetic ETW input cannot transition the state. Any restriction or rights-operation failure must atomically disable publication before evaluation, flush unread records, advance the generation, and record one metadata-only terminal reason. Clearing a restriction must never resume that stream; a newly created MAT stream must establish `RIGHTS_UNPROTECTED` again. `SetContentId` must save `m_ulContentId` before forwarding the new ID and restore that actual prior value on failure; deterministic tests cover forwarding failure, mixed-rights refresh failure, concurrent publication, and restricted-to-clear transitions. Rights transitions must be serialized with producer publication.

The future tap version exposes no kernel-memory mapping to user mode. The miniport may retain its kernel-only WaveRT DMA mapping solely for the ordered consumed-range copy; neither DMA nor ring pages may be mapped into a user process. Because this PortCls miniport sets `WdfDriverInitNoDispatchOverride`, it cannot use a KMDF framework control-device object. The same driver binary would instead create one named WDM control device with `IoCreateDeviceSecure` and a unique class GUID. After `PcInitializeAdapterDriver`, it would save and wrap `IRP_MJ_CREATE`, `CLEANUP`, `CLOSE`, and `DEVICE_CONTROL`, handle only its own control-device object, and forward every PortCls device request to the saved dispatch functions. A request reaching the wrong dispatch path would be a fatal verification failure.

The future WDM control device would expose buffered synchronous `QUERY_CAPS/STATUS` and overlapped `IOCTL_VIBE_MAT_TAP_READ` with `METHOD_OUT_DIRECT`, `FILE_READ_DATA`, at most one pending request, and a 256 KiB result cap. It would use an `IO_CSQ` cancel-safe queue plus explicit rundown/remove locking, copy complete records into caller-owned locked output buffers, complete cancellation on `CancelIoEx`, handle cleanup, process death, or removal, and reject a second consumer. No IRP may be completed while a spin lock is held.

The future control-device DACL would grant `GENERIC_READ` only to `SYSTEM` and Built-in Administrators; Vibepollo would normally open it as `SYSTEM`. Every fixed-width, explicitly packed ABI record would validate magic, major/minor version, header/total lengths, expected/current driver generation, boot-unique stream ID, exact MAT subtype, the native 116-byte KS-format size, 52-byte wave-format size, record count, carrier-frame index, QPC/frequency, payload length, flags, cumulative counters, integer arithmetic, cancellation, and teardown. It would contain no pointers, native enums, `BOOLEAN`, `size_t`, or implicit padding, and every reserved/output byte would be zeroed. A generation mismatch would return metadata only; a read would never mix generations or return a partial record. No write/control operation would be exposed until the later clock-control stage adds a separately versioned and range-checked rate IOCTL.

The native driver format contract is exactly 116 bytes: the platform's 64-byte `KSDATAFORMAT` header followed by the 52-byte `WAVEFORMATEXTENSIBLE_IEC61937` descriptor. There is no 84-byte native KS wrapper. A future compact transport record may define a different length only under its own explicit version and schema; it must never be passed as `KSDATAFORMAT`, accepted through `FormatSize`, or reported as the `STREAM_CREATED` native format size.

### Clock control (future transport/renderer work; not implemented)

The virtual device clock and laptop HDMI clock will differ by tens to hundreds of parts per million. Because an exclusive non-PCM WASAPI stream cannot be sample-rate adjusted by the client, the host must slave the virtual WaveRT consumption clock to the physical HDMI clock.

The renderer uses WASAPI exclusive event-driven mode. At 4 Hz and on state changes, the client reports a fixed `clock-buffer-v1` sample containing:

```text
feedback sequence and active epoch
IAudioClock device position and IAudioClock::GetFrequency result
the correlated QPC value returned by IAudioClock::GetPosition, in 100 ns units
submitted carrier frames and played carrier frames
queued carrier frames and endpoint buffer capacity in carrier frames
endpoint stream latency in microseconds
last contiguous carrier-frame index
underrun, overrun, and restart counters
```

The client computes played and queued frames from the device-clock position and its own submitted-frame counter. `IAudioClient::GetCurrentPadding` is diagnostic corroboration only; event-driven exclusive rendering does not treat it as a continuously adjustable shared-mode fill signal.

The host estimates the physical HDMI rate from deltas of device position and the correlated client QPC, then combines the rate estimate with filtered queue-occupancy error. It never compares absolute client QPC with host QPC. The controller sends a smooth simulated-clock correction to the driver, bounded to plus or minus 300 ppm with a maximum slew of 20 ppm per second. The driver changes only when bytes are consumed; it never edits or drops carrier bytes. Corrections and estimators reset on epoch changes. Saturation for five seconds, an invalid frequency, a non-monotonic feedback sequence/position, or queue movement outside the safe band is a clock failure and requests a fresh opaque epoch.

## Wire protocol (future transport; not implemented)

No Vibepollo or Moonlight wire extension, opaque RTP payload path, FEC, AEAD, retransmission, or clock-feedback implementation exists in the current tree. The following is the versioned design contract to implement only after the signed/runtime tap and direct-relay gates pass.

### Negotiation

Add a namespaced Vibepollo/Moonlight RTSP extension rather than overloading `audioChannels`, `surroundAudioInfo`, or an Opus mapping. The host DESCRIBE advertises `x-ss-audio[0].opaqueTransport:1`, exact profiles, and `clock-buffer-v1` feedback. After local discovery passes, the client ANNOUNCE offers `x-ml-audio[0].opaqueTransport:1`, its exact profiles, receive window, and feedback support. The audio SETUP response commits the selected transport with:

```text
X-SS-Audio-Transport: opaque-iec61937;version=1;format=mat10|mat20|mat21;clock=192000;fec=4+2;pt=99;payload=1200;salt=<32 lowercase hex digits>
```

Opaque version 1 additionally requires the paired session's encrypted-control-v2 capability. A missing, malformed, unauthenticated, unsupported, or non-opaque SETUP result selects legacy Opus before launch.

Old clients omit the offer and receive the current SDP and Opus packets byte-for-byte. Old hosts ignore an unknown client offer; the client observes no affirmative answer and uses its configured legacy layout.

### Epoch start and restart barrier

Negotiation selects a transport but does not authorize rendering. Every initial start and restart uses authenticated messages over the existing encrypted control channel:

1. Host sends `OPAQUE_PREPARE(epoch, profile, first_carrier_frame, host_qpc_frequency, start_buffer_frames)` only after it has a matching driver generation whose exact descriptor has reached `KSSTATE_RUN` and consumed at least one carrier frame. The 64-bit epoch is unique for the paired session.
2. Client repeats the endpoint gate, initializes but does not start the exact exclusive WASAPI stream, clears all old receive state, and replies `OPAQUE_READY(epoch)`.
3. Host may retain at most 100 ms of the new driver's initial carrier while waiting. It sends `OPAQUE_COMMIT(epoch, first_carrier_frame, first_packet_sequence)`; the client installs that epoch and replies `OPAQUE_COMMIT_ACK(epoch, first_packet_sequence)`. The host sends no data until the matching ACK. COMMIT and ACK are idempotently retransmitted. A timeout or mismatched reply sends `OPAQUE_ABORT(epoch)` and discards that generation.
4. Client admits data only for the acknowledged committed epoch. After it has a contiguous authenticated start buffer, it calls `IAudioClient::Start` and replies `OPAQUE_RUNNING(epoch, first_carrier_frame)`.
5. `OPAQUE_STOP`, route loss, protected content, a driver discontinuity, or an unrecoverable gap closes the sink and discards every queued byte from that epoch. A restart must recreate the host spatial render/KS stream so the new driver generation begins at an OS/Dolby-produced MAT stream boundary; 16-byte carrier alignment alone is not a valid decoder reacquisition point. It then begins again at `OPAQUE_PREPARE` with a new epoch and epoch key. If the host cannot prove a fresh source-stream boundary, version 1 stops audio until a new session. Old and new epochs never overlap at the renderer.

Data received before the client sends `OPAQUE_COMMIT_ACK`, after `OPAQUE_STOP`, or for an unknown epoch is dropped. A lost control message can be retransmitted idempotently, time out, or abort the attempt but cannot half-commit a sink.

### Packetization and integrity

Opaque data uses RTP payload type 99 with a 192 kHz clock; payload type 127 remains the FEC packet type. Each data packet contains a packed 52-byte network-order header:

```text
u8  version = 1
u8  profile = 1 MAT10 | 2 MAT20 | 3 MAT21
u16 flags
u64 epoch
u64 packet_sequence
u64 first_carrier_frame
u16 carrier_frame_count
u16 plaintext_length
u64 host_qpc
u64 fec_group_first_sequence
u8  fec_shard_index
u8  fec_data_shards = 4
u8  fec_parity_shards = 2
u8  reserved = 0
```

`plaintext_length` is from 16 through 1200 and is a multiple of 16; `carrier_frame_count` must equal `plaintext_length / 16`. The plaintext carrier bytes are zero-padded to exactly 1200 bytes before encryption. The receiver verifies authenticated padding is zero and discards it. Each epoch starts from a cryptographically random 32-bit RTP timestamp, and each following data packet increments it by the preceding packet's `carrier_frame_count`; semantic ordering still uses the non-wrapping 64-bit `packet_sequence` and `first_carrier_frame`. A packet therefore advances the 192 kHz RTP clock by its carrier-frame count and never treats that alignment unit as a parsed Dolby access unit.

The host creates the SETUP response's 16-byte `session_salt` with a cryptographic random generator. Derive a distinct 16-byte epoch key with HKDF-SHA-256 from the paired session audio key, salt `session_salt || epoch_be`, and info string `vibepollo opaque audio v1 aes-128-gcm`. The 96-bit AES-GCM nonce is four bytes `4D 41 54 31` (`MAT1`) followed by the big-endian 64-bit `packet_sequence`. The 52-byte header is additional authenticated data. Packet sequence starts at zero for each new epoch key and may never wrap. An epoch collision, sequence reuse, or key-derivation failure aborts the session.

For each data packet, the FEC-protected unit is exactly `header[52] || ciphertext[1200] || gcm_tag[16]`, or 1268 bytes. Reed-Solomon FEC operates over four consecutive equal protected units and creates two parity units after data encryption. The FEC header names payload type 99.

Each parity packet carries a packed 24-byte network-order header followed by 1268 parity bytes and a 16-byte authenticator:

```text
u8  version = 1
u8  parity_index = 0 | 1
u8  data_shards = 4
u8  parity_shards = 2
u64 epoch
u64 first_packet_sequence
u16 protected_unit_size = 1268
u16 reserved = 0
```

Derive a separate 32-byte parity key from the same IKM and salt with info string `vibepollo opaque audio v1 fec hmac-sha256`. The parity authenticator is the first 16 bytes of `HMAC-SHA-256(parity_key, parity_header || parity_bytes)`. The receiver authenticates every parity unit before it enters FEC. On receive it HMAC-verifies parity shards, GCM-verifies every present data shard and marks failures as erasures, reconstructs only when the available authenticated shards suffice, and GCM-verifies every reconstructed data shard before admission. Thus corrupt received data can never influence a reconstructed plaintext as trusted input.

With the fixed RTP header and no CSRC list or RTP extension, a data datagram is 1280 bytes (`12 + 1268`) and a parity datagram is 1320 bytes (`12 + 24 + 1268 + 16`). Both remain below the existing 1400-byte Moonlight audio packet cap. Shared protocol headers require compile-time size assertions and packet encode/decode tests that reject RTP extensions, CSRC entries, or any layout that exceeds that cap.

The receiver authenticates before admitting a fragment to the reorder window. It rejects duplicate, stale, cross-epoch, misaligned, oversized, inconsistent, or unauthenticated fragments without writing them to the endpoint.

### Loss recovery

The transport combines the fixed 4+2 FEC group with deadline-bounded retransmission. The encrypted control channel carries authenticated NACKs only. A retransmission on the audio data socket is a byte-for-byte copy of the original RTP packet and protected unit identified by epoch and packet sequence; it is never re-encrypted under the same nonce. Recovery must complete before the fragment's playout deadline. The jitter window is bounded and reports its effective latency.

An unrecovered carrier gap cannot be concealed. The client sends `OPAQUE_RESET_REQUEST`, stops the exclusive stream, reports the failure, and discards the incomplete epoch. It accepts only a new PREPARE/READY/COMMIT barrier for a fresh opaque epoch of the same exact format during that session. It never inserts arbitrary zeroes, repeats encrypted carrier data, or reinterprets packets as Opus.

Host and client diagnostics calculate hashes over reconstructed plaintext ranges in test mode. Sustained tests require identical hashes and carrier-frame counts.

## Client renderer and UX (future client implementation; not implemented)

The future Moonlight Qt Windows renderer gains a separate opaque endpoint backend. It must not pass MAT through the existing Opus decoder callback or speaker-layout abstraction.

The settings surface shows an option such as **Atmos over HDMI (experimental)** only after the discovery gate passes. Supporting detail shows the selected endpoint and exact negotiated MAT profile. When hidden, ordinary Stereo, 5.1, and 7.1 choices behave exactly as they do today.

Preflight failures are actionable and name the failed condition, for example:

- selected endpoint is not HDMI;
- Dolby Atmos for Home Theater is installed but not active;
- the TV/eARC route does not accept MAT10, MAT20, or MAT21 exclusively;
- the host or client fork lacks opaque-audio version 1;
- exclusive access is held by another application.

Endpoint notifications immediately invalidate cached capability. HDMI unplug, TV mode changes, spatial-provider changes, sleep/resume, default-device changes, and exclusive-stream invalidation close the current epoch before any same-format restart or audio stop.

## Audio/video synchronization (future transport/renderer implementation; not implemented)

The future driver's host QPC timestamp represents consumption of the first carrier frame in each record. Vibepollo must map that clock into the same session timing domain used by video. The client must establish an initial playout point from the mapped timestamp, its measured WASAPI stream latency, and its current buffer fill.

Steady-state sync follows the HDMI hardware clock feedback loop. A per-endpoint eARC output-delay calibration may be applied after measured WASAPI latency; it is stored by stable endpoint identity and never inferred from a receiver name. The diagnostic overlay reports network recovery delay, receive-buffer depth, endpoint latency, clock correction, underruns, restarts, and estimated A/V offset.

The acceptance target is no unbounded drift and an A/V offset within plus or minus 20 ms for one hour after endpoint-specific eARC calibration.

## Fallback and state machine (future session integration; not implemented)

```text
LEGACY_OPUS
  -> client offer + host answer + both preflights pass
OPAQUE_HOST_PREPARED
  -> OPAQUE_PREPARE + exclusive sink initialized
OPAQUE_CLIENT_READY
  -> OPAQUE_READY + OPAQUE_COMMIT + contiguous authenticated start buffer
OPAQUE_RUNNING
  -> hot-plug / format change / auth failure / unrecovered gap / clock failure
OPAQUE_STOPPING
  -> fresh host source boundary and same exact client endpoint/format pass
OPAQUE_HOST_PREPARED
  -> opaque restart or source-boundary proof fails
OPAQUE_FAILED_AUDIO_STOPPED
```

Before session launch, the Atmos-over-HDMI selection may fall back to the user's separately stored legacy layout when opaque negotiation or either preflight fails. A failed Atmos request never silently changes Stereo to 5.1 or 7.1.

After opaque streaming begins, version 1 never reinterprets the active RTP stream or silently switches codecs. It may open a new opaque epoch only on the same exact format after a bounded endpoint-recovery attempt. If that fails, it stops audio, reports an actionable route error, and preserves video/input. The next launch can select the stored Opus fallback. A live, acknowledged MAT-to-Opus transition is reserved for a later protocol version.

## Security and privacy (future tap/transport implementation)

These are requirements for the deferred tap, control, transport, and renderer stages. Stage 2's implemented boundary is metadata-only and has no payload handoff or network key material.

- Authenticate all opaque carrier data and reconstruction metadata.
- Derive new stream keys from the paired session; never persist carrier keys.
- Enforce unique GCM nonces and 64-bit epoch/sequence wrap checks.
- Bound packet lengths, reorder windows, FEC allocations, ring sizes, and feedback rates.
- Restrict the driver device interface and prevent unprivileged capture.
- Zero key material, cancel pending reads, and invalidate driver handles during teardown.
- Exclude protected/DRM content and do not attempt to bypass OS policy.
- Do not log carrier payloads by default. Test capture and hashes require an explicit diagnostic switch.

## Falsification and delivery stages

Each stage is a gate for the next. A failed gate is documented and resolved or the architecture is changed; it is not hidden by widening scope.

1. **[COMPLETE: machine-side evidence] Laptop sink capability and carrier-state correlation**
   - The no-write capability probe covers the actual TCL HDMI endpoint with the active Atmos GUID, exact MAT10 support, and no-start exclusive initialization. A legal OS-generated spatial scene provides machine-side spatial-stream/carrier-state evidence on the stable route. A continuously present Atmos badge is only route/carrier-state evidence; it cannot prove that this scene acquired a new lock, that content was native rather than upmixed, or that a relay was bit-exact.
2. **[COMPLETE: source/build/package only] MAT10-isolated HDMI package build and recovery rehearsal**
   - The dedicated HDMI-only package has the unique hardware ID, service, catalog, provider, and endpoint name. It enables PCM plus the one canonical MAT10 descriptor, sets the HDMI device maximum to eight channels, hides MAT20/MAT21 and every other encoded subtype from every enabled source/pointer/count/package surface, and enforces native 116-byte KS-wrapper/52-byte wave-descriptor validation. The metadata/state implementation and compiled/static tests cover exact stream creation, authoritative rights, `RUN`, raw write commitment, aligned consumption, terminal errors, exact notification-buffer/contiguous-packet/EOS/wrap mapping, same-stream causal attribution, mixed-rights refresh, prior-content-ID rollback, and rejection of stale, duplicate, skipped, late, overrun, cross-stream, probe-only, default-zero-rights, and consumption-beyond-frontier records. `Release|x64`, InfVerif/Inf2Cat, unsigned package checks, containment, and read-only lifecycle fixtures are GREEN. Stage 2 does not enable test signing, reboot, install, open a device, or create a live stream; it has no genuine installer receipt, so Uninstall/Recover fail closed and no root instance/oem INF is claimed.
3. **[DEFERRED: requires signed install/runtime] MAT10-isolated SysVAD runtime eligibility**
   - Preserve the host boot/device baseline and exact recovery path, then use an authorized driver-signing/test environment to install only the identified HDMI-only package and activate Atmos through supported UI. Require the spatial API to report a native static-object capability mask containing every required 7.1.4 bit and exactly 20 dynamic objects; extra advertised native static types are ignored and never activated by the test. Require one boot-unique stream ID and driver generation to emit exactly `STREAM_CREATED(116/52 MAT10 plus exact DMA bytes, NotificationCount, and packet bytes) -> RIGHTS_UNPROTECTED(successful refreshed mixed rights) -> KSSTATE_RUN -> WRITE_COMMITTED(contiguous logical packet/range) -> CONSUMED`, with every consumed range positive, 16-byte aligned, and within the monotonic committed frontier. A separately built verifier owns the acceptance decision: it consumes the raw callback inputs, derives packet size, logical sequence, EOS state, and frontier from scratch, and only afterward compares driver-emitted derived fields; it never treats `WRITE_COMMITTED`, `CONSUMED`, or a precomputed frontier as their own proof. Missing commits, packet gaps/duplicates/ambiguity, unknown/restricted rights, cross-stream or probe-only evidence, arithmetic overflow, or consumption beyond the verifier-recomputed frontier is a STOP before any byte handoff. Cleanly uninstall and restore the captured baseline if the gate fails.
4. **[DEFERRED: requires kernel tap/control implementation] Synthetic control surface and driver tap**
   - First prove the same-binary WDM control device, strict PortCls dispatch demultiplexing, fixed-slot ring/state modules, malformed-request rejection, second-open rejection, `CancelIoEx`, process death, cleanup, removal, and unload under Driver Verifier using synthetic records only. Then connect the tap at the pre-play-position point and read with a standalone SYSTEM/administrator diagnostic client. Run 30-60 minutes without payload persistence, checking exact carrier-frame continuity, generation stability, ring high-water, DPC latency, PortCls glitches, forced reader starvation, rights invalidation, cancellation, and clean teardown.
5. **[DEFERRED: requires tap, authenticated handoff, and client writer] Ephemeral direct-relay integrity and carrier-state correlation**
   - Relay the already proven eight-second legal OS-generated scene through the virtual endpoint, tap, mutually authenticated system TLS with explicit framing and bounded queues, and the laptop's exact MAT10 exclusive writer. A measured fixed prebuffer is sufficient for this bounded gate; clock control remains disabled. Hash the contiguous stream immediately after driver read and immediately before the client render-buffer copy. Both endpoints write create-new, payload-free, hash-chained measurement records and a final immutable manifest binding the exact source and sink binary hashes, boot-unique stream ID, driver generation, full MAT descriptor hash, first and last carrier-frame indices, exact byte/frame counts, SHA-256, run timestamps, and clean terminal status. A separate verifier reads and hashes the actual staged binaries and transcript inputs itself, validates both hash chains, and constructs the final manifest; endpoint self-reported binary or manifest hashes are never trusted. The client transcript must name the same source stream/range received over the authenticated connection; equality from unrelated buffers or generations is rejected. On queue full, gap, sink underrun, exclusive invalidation, terminal poison, non-clean stop, or any missing byte, stop without silence insertion or repetition. Require exact bound range, hash, byte-count, and carrier-frame-count equality, prove that the client writer holds the intended MAT10 endpoint exclusively without invoking the spatial renderer, and correlate the run window with a maintained receiver Dolby Atmos carrier indication before porting to the upstream Virtual Audio Driver or investing in RTP, FEC, AEAD, or clock-control implementation. A continuously present indication does not prove that this run acquired the receiver lock.
6. **[DEFERRED: requires wire implementation] Synthetic transport and clock recovery**
   - Send deterministic carrier-like blocks at 3,072,000 bytes/s through the fork. Inject reorder, loss, burst loss, corruption, duplicate packets, and plus/minus 200 ppm clock error. Require bit identity and zero under/overruns for two hours.
7. **[DEFERRED: requires all prior stages] Real end-to-end spatial stream**
   - Run a Windows spatial sample and then a game through host Dolby, virtual HDMI, network transport, laptop HDMI, TCL/eARC, and soundbar. Require matching carrier hashes, correlate the run with a maintained receiver Atmos indication, and evaluate moving height/object behavior, latency, and sync. Do not treat a continuously present indication as proof of acquisition or native-content provenance.
8. **[DEFERRED: requires install and end-to-end implementation] Compatibility and lifecycle**
   - Verify old host/new client, new host/old client, unsupported endpoints, spatial Off, Atmos for Headphones, HDMI unplug, TV mode change, exclusive contention, suspend/resume, service restart, driver update/uninstall, and Opus fallback.

Test signing, production signing, reboot, and live driver/runtime work remain deferred. They require the Stage-3 uninstall path, genuine installer-authenticated recovery receipt, recovery instructions, and preserved host baseline before any machine mutation.

## Acceptance criteria

**Stage 1-2 evidence currently satisfied:**

- The intended client HDMI route passes the no-write capability intersection for the active Atmos configuration and MAT10; machine-side carrier-state evidence is recorded with the explicit receiver-badge/provenance caveat.
- The isolated SysVAD MAT10 source/package snapshot is independently GREEN for native 116/52 format validation, one render/zero capture endpoint shape, rights/frontier/terminal metadata state, compiled concurrency and mutation tests, `Release|x64` build, InfVerif/Inf2Cat, unsigned package identity, and read-only lifecycle fixtures.
- No driver install/load/runtime, kernel payload tap/control device, opaque transport, client renderer, or end-to-end Atmos result is claimed.

**Future acceptance criteria (Stages 3-8; not yet met by the current tree):**

- The Atmos choice is absent for every negative capability-gate case.
- The host virtual endpoint exposes and actively uses Dolby Atmos for Home Theater.
- A spatial test proves a native static-object capability mask containing every required 7.1.4 bit and exactly 20 dynamic objects; one boot-unique miniport stream then proves copied native 116/52-byte canonical MAT10 creation, authoritative successfully refreshed unprotected rights, `KSSTATE_RUN`, a monotonic committed-write frontier independently derived from contiguous raw `SetWritePacket` inputs, and positive aligned consumption wholly within that frontier. Extra advertised native static types are ignored and never activated.
- Synthetic WDM control-device tests prove exact PortCls dispatch demultiplexing, single-reader access, malformed-input rejection, cancel-safe pending reads, process-death cleanup, removal/unload safety, fixed-generation records, and zero payload exposure before the tap is connected.
- The bounded direct relay yields exact host-tap/client-pre-render plaintext hash, byte-count, and carrier-frame-count equality for one manifest-bound stream ID, driver generation, descriptor, and first/last carrier-frame range; uses the intended exclusive MAT writer without the client spatial renderer; and maintains the externally observed receiver Atmos carrier indication throughout the correlated run window before full network transport work begins.
- Host and client plaintext block hashes match over a sustained stream.
- Every uncorrectable gap permanently rejects its epoch and causes either a complete PREPARE/READY/COMMIT restart from a valid source boundary or an explicit audio stop; no carrier concealment occurs.
- Synthetic plus/minus 200 ppm drift runs for two hours without underflow, overflow, or byte mutation.
- Measured added latency and all buffer components are reported; no latency claim is inferred from throughput.
- A/V drift stays within plus or minus 20 ms for one hour after eARC calibration.
- Driver Verifier, malformed IOCTL, access-control, install/uninstall, reboot, sleep/resume, and teardown tests pass.
- Existing stereo/5.1/7.1 Opus behavior and unmodified peer interoperability pass regression tests.
- No protected-content bypass, proprietary Dolby codec implementation, or bundled Dolby test payload is introduced.

## Public-release gate

The private local prototype may establish technical feasibility using the user's licensed Windows and Dolby installation. Before public binaries, public branding, or an upstream release, record a licensing and trademark disposition from Dolby/Microsoft and confirm the SysVAD-derived driver's notice and redistribution obligations. Source-only engineering can proceed without representing the result as Dolby-certified.

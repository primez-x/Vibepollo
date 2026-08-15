# Capability-Gated Dolby MAT Transport

**Status:** Approved for staged local prototyping  
**Date:** 2026-08-15  
**Primary upstream:** [Nonary/Vibepollo](https://github.com/Nonary/Vibepollo)  
**Client upstreams:** [Nonary/moonlight-qt](https://github.com/Nonary/moonlight-qt) and [Nonary/moonlight-common-c](https://github.com/Nonary/moonlight-common-c)

## Decision

Add an experimental, backwards-compatible opaque IEC 61937 Dolby MAT path beside the existing GameStream Opus path. Windows Spatial Sound and Dolby Access render game beds and dynamic objects into MAT on a virtual HDMI endpoint on the host. Vibepollo transports those bytes without decoding, remixing, resampling, or changing volume. A paired Windows Moonlight client writes the recovered carrier to an Atmos-capable HDMI endpoint in WASAPI exclusive mode.

Atmos is available to the user only when both peers pass explicit capability checks. A pre-launch failure leaves the session on the existing stereo/5.1/7.1 Opus path. After an opaque epoch is committed, a route or transport failure stops opaque audio while preserving video/input; version 1 never changes the live stream back to Opus without a new session.

This is not a 12-channel extension to Opus. Dolby MAT's eight 192 kHz carrier lanes are a non-PCM transport representation, not eight speaker channels. Treating MAT as 7.1 PCM would destroy its object and height metadata.

## Goals

- Preserve the host OS's Dolby Atmos for Home Theater output through a Windows laptop's HDMI/eARC path to a TV and soundbar.
- Preserve static 7.1.4 beds and dynamic-object metadata by relaying the MAT carrier bit-for-bit.
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
- Shipping public Dolby branding or a redistributable driver package before licensing and trademark review.
- Replacing the existing Opus audio implementation for normal sessions.

## Verified starting point

Vibepollo currently captures shared-mode 48 kHz float PCM and encodes only 2-, 6-, or 8-channel Opus. Moonlight's common audio contract and Qt UI have the same 2/6/8-channel assumptions. The Steam Streaming Speakers driver does not advertise the HDMI/MAT endpoint required for Dolby Atmos for Home Theater.

Microsoft's SysVAD TabletAudioSample already contains an HDMI endpoint topology and MAT 2.0/MAT 2.1 render data ranges with an eight-lane, 16-bit, 192 kHz carrier. Its public loopback pin remains PCM-only, so the prototype requires a driver-internal tap of bytes consumed from the WaveRT render buffer.

The reviewed no-write probe measured the intended laptop path, `Beyond TV (NVIDIA High Definition Audio)`, as active HDMI with Dolby Atmos for Home Theater active. Its exact exclusive-mode intersection is MAT10 only: MAT10 support and initialization return `S_OK`, while MAT20 and MAT21 return `AUDCLNT_E_UNSUPPORTED_FORMAT`. Therefore the SysVAD-derived source endpoint must add the exact MAT10/MLP data range and prove that the licensed Windows Dolby producer actually selects MAT10. The opaque session may be offered only when the observed host source profile and this client sink profile intersect exactly.

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
| Separate SysVAD-derived repository | Virtual HDMI render endpoint, MAT format advertisement, internal capture ring, controllable WaveRT clock | Retain Microsoft sample notices and MS-PL-derived source separation |

The driver source must not be copied into the Vibepollo repository. The host communicates with it through a documented, versioned device-control interface.

## End-to-end flow

```text
Game / Windows spatial API
        |
        v
Windows Spatial Sound + Dolby provider on host
        |
        v
SysVAD-derived virtual HDMI WaveRT render endpoint (MAT20 or MAT21)
        |
        +--> simulated hardware consumes cyclic-buffer bytes
        |         |
        |         v
        |    kernel capture ring + timing metadata
        |         |
        v         v
Vibepollo host reader -> MAT packetizer -> AES-128-GCM -> FEC/retransmit
        |
        v
Moonlight common receive/recovery -> bit-exact block stream
        |
        v
Moonlight Qt WASAPI exclusive renderer
        |
        v
Laptop HDMI -> TCL TV/eARC -> Atmos-capable soundbar
```

## Capability contract

### Client discovery gate

The client may display the experimental Atmos choice only when every condition below is true for the currently selected render endpoint:

1. The endpoint is active.
2. `PKEY_AudioEndpoint_FormFactor` equals `DigitalAudioDisplayDevice` and the physical connector is positively identified as HDMI. Require `PKEY_AudioEndpoint_JackSubType == KSNODETYPE_HDMI_INTERFACE`; an implementation may corroborate it with `IKsJackSinkInformation::GetJackSinkInformation().ConnType == KSJACK_SINK_CONNECTIONTYPE_HDMI`. A missing, malformed, DisplayPort, or inconclusive subtype fails closed.
3. Spatial configuration is linked to this exact endpoint. `IMMDevice::GetId` and WinRT `MediaDevice`/`DeviceInformation` render-device IDs are distinct opaque namespaces: never pass the former to `SpatialAudioDeviceConfiguration`. For default selection, bookend endpoint, spatial, and MAT observation by sampling all three Core Audio default roles plus the WinRT Default and Communications render IDs before and after it; authorize the link only when the selected endpoint and the nonempty opaque WinRT ID remain exact throughout. An explicit endpoint needs an API-provided link to its WinRT render-device ID; an unlinked or changed endpoint fails closed.
4. `SpatialAudioDeviceConfiguration.IsSpatialAudioSupported` is true.
5. `IsSpatialAudioFormatSupported(DolbyAtmosForHomeTheater)` is true.
6. `ActiveSpatialAudioFormat` exactly equals Dolby Atmos for Home Theater GUID `{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}`.
7. WASAPI exclusive-mode `IsFormatSupported` accepts at least one exact IEC 61937 descriptor from the peer-supported intersection: `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MLP` (MAT10), `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT20`, or `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_MAT21`.
8. The host advertises the versioned opaque-audio extension.

The UI must not use the default spatial format as a substitute for the active format. Atmos for Headphones, Windows Sonic, DTS, inactive endpoints, Bluetooth, DisplayPort, and ordinary multichannel PCM do not satisfy the gate. Windows groups HDMI and DisplayPort under `DigitalAudioDisplayDevice`, so the form-factor property alone is insufficient.

At stream start, the client repeats the checks and performs an exclusive-mode `IAudioClient::Initialize` preflight for the negotiated exact format. The mode is committed only after initialization succeeds. Discovery makes the choice visible; the start-time preflight authorizes actual use. The capability report uses schema version 2 and records link provenance plus the opaque WinRT input and API-returned configuration IDs separately from the opaque MMDevice ID.

### Host activation gate

The host may accept an opaque MAT request only when:

1. The paired client offered protocol version 1 and an exact MAT subtype.
2. The virtual driver interface and ABI version match the host reader.
3. The virtual HDMI endpoint is active and selected for the stream.
4. Dolby Atmos for Home Theater is supported and currently active on that endpoint.
5. The endpoint reports the expected spatial capacity: a 7.1.4 static bed and 20 dynamic objects.
6. The driver reports that the negotiated MAT subtype is the actual active render format.
7. The capture ring is healthy before the first network packet is sent.

Public Windows APIs do not allow Vibepollo to activate a spatial provider it does not own. The prototype therefore expects a one-time user selection of Dolby Atmos for Home Theater in Windows/Dolby Access. Vibepollo verifies that state and explains a failed preflight; it does not use undocumented registry edits.

Client sink support for MAT10, MAT20, or MAT21 does not prove that the host source emits that profile. The host gate's active-render-format observation must match the negotiated profile, and the later legal OS-generated spatial-stream test must independently confirm receiver Atmos lock on that route.

### Negotiated descriptor

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

Start from the SysVAD TabletAudioSample HDMI wave and topology descriptors. Retain the PCM modes needed for Windows endpoint setup and add no invented formats. Before attributing a failure to Dolby eligibility, correct and test two defects in the sample: `HdmiTopoPinDataRangesBridge` declares MAT20/MAT21 ranges but omits their pointers, and `HdmiMiniports` supplies a two-channel device maximum while the MAT carrier formats use eight lanes. The corrected values must remain internally consistent across the WaveRT pin, topology bridge, and per-channel control allocation.

The first driver kill test uses an otherwise minimally modified endpoint. It must prove all of the following before transport development depends on it:

- Windows exposes Dolby Atmos for Home Theater for the endpoint.
- The format becomes the active spatial format after user activation.
- the spatial client reports a 7.1.4 bed and 20 dynamic objects;
- a Microsoft spatial-audio sample causes the WaveRT miniport to receive MAT20 or MAT21.

If the Dolby provider never selects MAT for this endpoint, the opaque-relay architecture is not viable with this virtual-driver model. The fallback research path is a client-side spatial renderer fed with 7.1.4 PCM, with the explicit loss of dynamic objects.

### Internal capture tap

The public SysVAD loopback pin is not used for MAT. The miniport copies bytes as its simulated hardware consumes the WaveRT cyclic buffer and publishes them through a fixed 2 MiB nonpaged, kernel-owned ring. The producer never waits for user mode; if the next complete aligned record does not fit, it publishes an overflow marker when space becomes available and forces a new stream generation. Each published record contains:

```text
ABI version
stream epoch (64-bit)
exact MAT subtype
first carrier-block index (64-bit)
host QPC and QPC frequency
payload byte count
flags: start, discontinuity, format-change, stop
payload, always a multiple of 16 bytes
```

The tap must never decode, inspect Dolby payload semantics, resample, apply gain, synthesize missing bytes, or publish data not consumed by the render engine. On ring exhaustion it marks a discontinuity, advances its driver stream generation, and requires user mode to abandon the associated network epoch.

For each simulated DMA advance, the miniport snapshots `oldPlayPosition` and `newPlayPosition`, verifies that the advance cannot exceed or refer to overwritten source-buffer capacity, reserves complete output records, and copies exactly the half-open range `[oldPlayPosition, newPlayPosition)` before publishing the advanced play position. It splits a circular-buffer wrap into ordered copies without changing the logical carrier-block sequence and handles multi-period advances in one transaction. The record timestamp is the scheduled consumption time of its first 16-byte carrier block, derived from the pre-advance clock state. A release barrier publishes the completed ring record before the new play position becomes observable.

Ring overflow, possible source overwrite, non-aligned advancement, and a failed copy never emit partial payload. They atomically increment out-of-band `stream_generation`, `dropped_bytes`, and `discontinuity_count` fields returned with every read/status request, so a full ring cannot hide its own overflow notification.

The tap is fail-closed on PortCls content rights. Payload publication requires a successfully established rights state with both `CopyProtect == false` and `DigitalOutputDisable == false`. If either restriction is present or a rights operation fails, the tap atomically stops before the next copy, flushes unread payload records, advances the generation, and publishes one metadata-only `PROTECTED_CONTENT` record. Capture remains disabled until PortCls starts a new explicitly unprotected stream generation; clearing a flag never resumes the old generation.

Version 1 exposes no kernel-memory mapping to user mode. The miniport may retain its kernel-only WaveRT DMA mapping solely for the ordered consumed-range copy; neither DMA nor ring pages are mapped into a user process. A separate private KMDF control device permits one read-only consumer. `IOCTL_VIBE_MAT_TAP_READ` uses overlapped `METHOD_OUT_DIRECT` reads into caller-owned locked output buffers, with at most one pending read and a 256 KiB result cap. The driver copies complete records from its ring, completes cancellation on handle close/PnP removal, and rejects a second consumer.

The control-device DACL grants `GENERIC_READ` only to `SYSTEM` and Built-in Administrators; Vibepollo normally opens it as `SYSTEM`. Every request validates ABI version, input/output sizes, record length, 16-byte alignment, integer arithmetic, caller access, cancellation, stream generation, and teardown. No write/control operation is exposed until the later clock-control stage adds a separately versioned and range-checked rate IOCTL.

### Clock control

The virtual device clock and laptop HDMI clock will differ by tens to hundreds of parts per million. Because an exclusive non-PCM WASAPI stream cannot be sample-rate adjusted by the client, the host must slave the virtual WaveRT consumption clock to the physical HDMI clock.

The renderer uses WASAPI exclusive event-driven mode. At 4 Hz and on state changes, the client reports a fixed `clock-buffer-v1` sample containing:

```text
feedback sequence and active epoch
IAudioClock device position and IAudioClock::GetFrequency result
the correlated QPC value returned by IAudioClock::GetPosition, in 100 ns units
submitted carrier frames and played carrier frames
queued carrier frames and endpoint buffer capacity in carrier frames
endpoint stream latency in microseconds
last contiguous carrier-block index
underrun, overrun, and restart counters
```

The client computes played and queued frames from the device-clock position and its own submitted-frame counter. `IAudioClient::GetCurrentPadding` is diagnostic corroboration only; event-driven exclusive rendering does not treat it as a continuously adjustable shared-mode fill signal.

The host estimates the physical HDMI rate from deltas of device position and the correlated client QPC, then combines the rate estimate with filtered queue-occupancy error. It never compares absolute client QPC with host QPC. The controller sends a smooth simulated-clock correction to the driver, bounded to plus or minus 300 ppm with a maximum slew of 20 ppm per second. The driver changes only when bytes are consumed; it never edits or drops carrier bytes. Corrections and estimators reset on epoch changes. Saturation for five seconds, an invalid frequency, a non-monotonic feedback sequence/position, or queue movement outside the safe band is a clock failure and requests a fresh opaque epoch.

## Wire protocol

### Negotiation

Add a namespaced Vibepollo/Moonlight RTSP extension rather than overloading `audioChannels`, `surroundAudioInfo`, or an Opus mapping. The host DESCRIBE advertises `x-ss-audio[0].opaqueTransport:1`, exact profiles, and `clock-buffer-v1` feedback. After local discovery passes, the client ANNOUNCE offers `x-ml-audio[0].opaqueTransport:1`, its exact profiles, receive window, and feedback support. The audio SETUP response commits the selected transport with:

```text
X-SS-Audio-Transport: opaque-iec61937;version=1;format=mat10|mat20|mat21;clock=192000;fec=4+2;pt=99;payload=1200;salt=<32 lowercase hex digits>
```

Opaque version 1 additionally requires the paired session's encrypted-control-v2 capability. A missing, malformed, unauthenticated, unsupported, or non-opaque SETUP result selects legacy Opus before launch.

Old clients omit the offer and receive the current SDP and Opus packets byte-for-byte. Old hosts ignore an unknown client offer; the client observes no affirmative answer and uses its configured legacy layout.

### Epoch start and restart barrier

Negotiation selects a transport but does not authorize rendering. Every initial start and restart uses authenticated messages over the existing encrypted control channel:

1. Host sends `OPAQUE_PREPARE(epoch, profile, first_carrier_block, host_qpc_frequency, start_buffer_blocks)` only after it has a matching driver generation. The 64-bit epoch is unique for the paired session.
2. Client repeats the endpoint gate, initializes but does not start the exact exclusive WASAPI stream, clears all old receive state, and replies `OPAQUE_READY(epoch)`.
3. Host may retain at most 100 ms of the new driver's initial carrier while waiting. It sends `OPAQUE_COMMIT(epoch, first_carrier_block, first_packet_sequence)`; the client installs that epoch and replies `OPAQUE_COMMIT_ACK(epoch, first_packet_sequence)`. The host sends no data until the matching ACK. COMMIT and ACK are idempotently retransmitted. A timeout or mismatched reply sends `OPAQUE_ABORT(epoch)` and discards that generation.
4. Client admits data only for the acknowledged committed epoch. After it has a contiguous authenticated start buffer, it calls `IAudioClient::Start` and replies `OPAQUE_RUNNING(epoch, first_carrier_block)`.
5. `OPAQUE_STOP`, route loss, protected content, a driver discontinuity, or an unrecoverable gap closes the sink and discards every queued byte from that epoch. A restart must recreate the host spatial render/KS stream so the new driver generation begins at an OS/Dolby-produced MAT stream boundary; 16-byte carrier alignment alone is not a valid decoder reacquisition point. It then begins again at `OPAQUE_PREPARE` with a new epoch and epoch key. If the host cannot prove a fresh source-stream boundary, version 1 stops audio until a new session. Old and new epochs never overlap at the renderer.

Data received before the client sends `OPAQUE_COMMIT_ACK`, after `OPAQUE_STOP`, or for an unknown epoch is dropped. A lost control message can be retransmitted idempotently, time out, or abort the attempt but cannot half-commit a sink.

### Packetization and integrity

Opaque data uses RTP payload type 99 with a 192 kHz clock; payload type 127 remains the FEC packet type. Each data packet contains a packed 52-byte network-order header:

```text
u8  version = 1
u8  profile = 1 MAT20 | 2 MAT21 | 3 MAT10
u16 flags
u64 epoch
u64 packet_sequence
u64 first_carrier_block
u16 carrier_block_count
u16 plaintext_length
u64 host_qpc
u64 fec_group_first_sequence
u8  fec_shard_index
u8  fec_data_shards = 4
u8  fec_parity_shards = 2
u8  reserved = 0
```

`plaintext_length` is from 16 through 1200 and is a multiple of 16; `carrier_block_count` must equal `plaintext_length / 16`. The plaintext carrier bytes are zero-padded to exactly 1200 bytes before encryption. The receiver verifies authenticated padding is zero and discards it. Each epoch starts from a cryptographically random 32-bit RTP timestamp, and each following data packet increments it by the preceding packet's `carrier_block_count`; semantic ordering still uses the non-wrapping 64-bit `packet_sequence` and `first_carrier_block`. A packet therefore advances the 192 kHz RTP clock by its carrier-block count and never requires a fractional 48 kHz duration.

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

Host and client diagnostics calculate hashes over reconstructed plaintext blocks in test mode. Sustained tests require identical hashes and carrier-block counts.

## Client renderer and UX

The Moonlight Qt Windows renderer gains a separate opaque endpoint backend. It does not pass MAT through the existing Opus decoder callback or speaker-layout abstraction.

The settings surface shows an option such as **Atmos over HDMI (experimental)** only after the discovery gate passes. Supporting detail shows the selected endpoint and exact negotiated MAT profile. When hidden, ordinary Stereo, 5.1, and 7.1 choices behave exactly as they do today.

Preflight failures are actionable and name the failed condition, for example:

- selected endpoint is not HDMI;
- Dolby Atmos for Home Theater is installed but not active;
- the TV/eARC route does not accept MAT10, MAT20, or MAT21 exclusively;
- the host or client fork lacks opaque-audio version 1;
- exclusive access is held by another application.

Endpoint notifications immediately invalidate cached capability. HDMI unplug, TV mode changes, spatial-provider changes, sleep/resume, default-device changes, and exclusive-stream invalidation close the current epoch before any same-format restart or audio stop.

## Audio/video synchronization

The driver's host QPC timestamp represents consumption of the first carrier block in each record. Vibepollo maps that clock into the same session timing domain used by video. The client establishes an initial playout point from the mapped timestamp, its measured WASAPI stream latency, and its current buffer fill.

Steady-state sync follows the HDMI hardware clock feedback loop. A per-endpoint eARC output-delay calibration may be applied after measured WASAPI latency; it is stored by stable endpoint identity and never inferred from a receiver name. The diagnostic overlay reports network recovery delay, receive-buffer depth, endpoint latency, clock correction, underruns, restarts, and estimated A/V offset.

The acceptance target is no unbounded drift and an A/V offset within plus or minus 20 ms for one hour after endpoint-specific eARC calibration.

## Fallback and state machine

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

## Security and privacy

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

1. **Laptop sink capability and lock**
   - Run the no-write capability probe against the actual TCL HDMI endpoint and require the active Atmos GUID, exact MAT subtype support, and no-start exclusive initialization. Then use a legal OS-generated spatial stream to confirm the receiver takes Atmos lock. Stop if either half fails.
2. **Descriptor-corrected SysVAD build and recovery rehearsal**
   - Correct the topology MAT pointer omissions and eight-carrier-lane consistency without adding the capture tap. Build `Debug|x64`, inspect the package, record the exact root instance/INF identity, and validate install/uninstall/recovery scripts in dry-run mode. Do not enable test signing, reboot, or install the driver in this stage.
3. **Descriptor-corrected SysVAD runtime eligibility**
   - Preserve the host boot/device baseline, enable test signing, reboot, install only the identified test package, activate Atmos through supported UI, and instrument format selection. Require the spatial API to report a 7.1.4 bed plus 20 dynamic objects and require the miniport to observe MAT20 or MAT21. Cleanly uninstall and restore boot policy if the gate fails.
4. **Driver tap**
   - Capture consumed MAT bytes and timing for 30-60 minutes with continuity checks, Driver Verifier, clean cancellation, and no audio-engine stalls.
5. **Synthetic transport and clock recovery**
   - Send deterministic carrier-like blocks at 3,072,000 bytes/s through the fork. Inject reorder, loss, burst loss, corruption, duplicate packets, and plus/minus 200 ppm clock error. Require bit identity and zero under/overruns for two hours.
6. **Real end-to-end spatial stream**
   - Run a Windows spatial sample and then a game through host Dolby, virtual HDMI, network transport, laptop HDMI, TCL/eARC, and soundbar. Confirm Atmos lock, moving height/object behavior, carrier hashes, latency, and sync.
7. **Compatibility and lifecycle**
   - Verify old host/new client, new host/old client, unsupported endpoints, spatial Off, Atmos for Headphones, HDMI unplug, TV mode change, exclusive contention, suspend/resume, service restart, driver update/uninstall, and Opus fallback.

Test signing and reboot are deferred until stages 1 and 2 pass and stage 3 has a recorded uninstall path, recovery instructions, and preserved host baseline.

## Acceptance criteria

- The Atmos choice is absent for every negative capability-gate case.
- The host virtual endpoint exposes and actively uses Dolby Atmos for Home Theater.
- A spatial test proves a 7.1.4 bed plus 20 dynamic objects and observed MAT20 or MAT21 bytes.
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

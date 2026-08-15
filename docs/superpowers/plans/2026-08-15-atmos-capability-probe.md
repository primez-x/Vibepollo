# Atmos Capability Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build one no-audio-write Windows executable that determines whether an exact render endpoint passes the endpoint half of the Atmos sink gate: active HDMI, Dolby Atmos for Home Theater active, and exact MAT 2.0 or MAT 2.1 WASAPI exclusive initialization.

**Architecture:** Keep canonical endpoint observations separate from a portable, deterministic gate evaluator. A Windows adapter collects MMDevice, C++/WinRT spatial-audio, and exact IEC 61937 results; a tested application coordinator binds explicit endpoint selection, collection, evaluation, invariant-checked reporting, and exit mapping. The same release executable and SHA-256 are run on the host and laptop. A green result is named `ENDPOINT_PREFLIGHT_READY`: it authorizes only the later legal OS-generated spatial-stream/receiver-lock test and never claims that the downstream TV/eARC/soundbar has taken Atmos lock.

**Tech Stack:** C++23, GoogleTest, Windows MMDevice/WASAPI, C++/WinRT `Windows.Media.Audio`, `WAVEFORMATEXTENSIBLE_IEC61937`, nlohmann ordered JSON, CMake/Ninja, MSYS2 UCRT64.

## Global Constraints

- Primary source baseline is `Nonary/Vibepollo` commit `b85bd588431030c998f3b0e6968317554d046252`; work stays on branch `agent/atmos-transport` in the existing isolated worktree.
- The selected endpoint alone drives the verdict; enumeration of default roles and other active endpoints is diagnostic context only.
- Require both `PKEY_AudioEndpoint_FormFactor == DigitalAudioDisplayDevice` and a valid `PKEY_AudioEndpoint_JackSubType` string equal to the authoritative HDMI node-type GUID `{D1B9CC2A-F519-417F-91C9-55FA65481001}`. Define HDMI and DisplayPort (`{E47E4031-3EA6-418D-8F9B-B73843CCBA97}`) locally because the selected MinGW `ksmedia.h` omits both symbols. Missing, wrong-type, malformed, DisplayPort, and other subtypes fail closed.
- Require `IsSpatialAudioSupported`, support for `DolbyAtmosForHomeTheater`, and an `ActiveSpatialAudioFormat` equal to `{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}`. `DefaultSpatialAudioFormat` is diagnostic only.
- Probe MAT20 and MAT21 independently. Support for one never implies support for the other; prefer MAT21 only when its own exact support and initialization both return `S_OK`.
- Build the exact 52-byte Microsoft IEC 61937 descriptor: 8 lanes, 192000 carrier frames/s, 3072000 bytes/s, 16-byte alignment, 16 bits, `cbSize=34`, `KSAUDIO_SPEAKER_7POINT1`, encoded rate 96000, encoded channels 8, encoded average bytes/s 0.
- MAT20 is `{0000010C-0CEA-0010-8000-00AA00389B71}` and MAT21 is `{0000030C-0CEA-0010-8000-00AA00389B71}`. Define these documented GUIDs locally because the current MinGW `ksmedia.h` omits their symbolic constants.
- Each `IAudioClient::Initialize` attempt uses a fresh activated client with `AUDCLNT_SHAREMODE_EXCLUSIVE`, `AUDCLNT_STREAMFLAGS_NOPERSIST`, durations `0, 0`, and the exact descriptor.
- Never call `IAudioClient::GetService`, `IAudioRenderClient::GetBuffer`, `ReleaseBuffer`, or `IAudioClient::Start`; the report must always contain `"audio_bytes_written": false`. Exclusive `Initialize` may briefly reserve the endpoint, but no renderer service or sample buffer is acquired.
- Run C++/WinRT and raw COM from one STA initialized by `winrt::init_apartment(winrt::apartment_type::single_threaded)`; do not also call `CoInitializeEx`.
- This slice does not install a driver, enable test signing, reboot either machine, change a Windows spatial setting, modify the live Vibepollo install, or play bundled Dolby payloads.
- A blocked baseline is a stop condition for this slice. Retain its report and defer any single-variable endpoint/spatial remediation to a separately authorized measured stage.
- `ENDPOINT_PREFLIGHT_READY` does not prove the physical sink. Before any driver/test-signing stage, a separate legal OS-generated spatial stream must make the actual receiver report Atmos lock on the same selected route.
- Preserve the existing stereo/5.1/7.1 Opus implementation and all unrelated user work.

---

## File Map

- Create `tools/atmos_capability_probe_policy.h`: Windows-free observation, diagnostic, profile, and gate-result contracts.
- Create `tools/atmos_capability_probe_policy.cpp`: deterministic fail-closed gate evaluation and stable enum names.
- Create `tools/atmos_capability_probe_windows.h`: Windows probe interface plus exact descriptor and GUID helpers.
- Create `tools/atmos_capability_probe_windows.cpp`: MMDevice enumeration, HDMI property validation, C++/WinRT spatial queries, and fresh-client exclusive MAT preflights.
- Create `tools/atmos_capability_probe_cli.h`: Windows-free CLI option and parse-result contracts.
- Create `tools/atmos_capability_probe_cli.cpp`: strict argument parser for `--json`, `--endpoint-id`, and `--help`.
- Create `tools/atmos_capability_probe_app.h`: Windows-free observation-provider, application-result, report-validation, and coordinator contracts.
- Create `tools/atmos_capability_probe_app.cpp`: explicit selection wiring, invariant-checked ordered JSON/human output, and process exit mapping.
- Create `tools/atmos_capability_probe.cpp`: thin STA lifetime and Windows-provider entry point.
- Create `tests/unit/test_atmos_capability_probe_policy.cpp`: portable gate truth-table tests with hand-derived expectations.
- Create `tests/unit/test_atmos_capability_probe_cli.cpp`: portable CLI behavior tests.
- Create `tests/unit/test_atmos_capability_probe_app.cpp`: portable explicit-ID-to-report integration and 0/1/2/3 exit-contract tests.
- Create `tests/unit/platform/windows/test_atmos_capability_probe_windows.cpp`: exact 52-byte descriptor and HDMI GUID parser tests using real production helpers.
- Modify `tools/CMakeLists.txt`: add the standalone probe target with only its required libraries.
- Modify `tests/CMakeLists.txt`: register three portable component tests and one Windows-only descriptor test.

## Execution Prerequisite: Reproducible Windows Build Environment

The root agent owns this machine-level prerequisite; implementation leaves do not install packages. Record the pre-install command resolution, then install MSYS2 and Node.js LTS only if absent:

```powershell
Get-Command msys2,cmake,ninja,g++,node,npm -ErrorAction SilentlyContinue
winget install --id MSYS2.MSYS2 --exact --accept-package-agreements --accept-source-agreements
winget install --id OpenJS.NodeJS.LTS --exact --accept-package-agreements --accept-source-agreements
```

Invoke `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64` from the worktree; do not use bare `usr\bin\bash.exe`, which lacks the UCRT64 environment. In that shell, update MSYS2 and install the repository's UCRT64 CI dependency set:

```bash
pacman -Syu --noconfirm
pacman -S --needed --noconfirm \
  git \
  mingw-w64-ucrt-x86_64-boost \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-cppwinrt \
  mingw-w64-ucrt-x86_64-curl-winssl \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-MinHook \
  mingw-w64-ucrt-x86_64-miniupnpc \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-nlohmann-json \
  mingw-w64-ucrt-x86_64-onevpl \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-opus \
  mingw-w64-ucrt-x86_64-sqlite3 \
  mingw-w64-ucrt-x86_64-tools
```

Initialize the pinned submodules and configure a release Ninja tree with tests while explicitly disabling unrelated WebRTC, virtual-display, and documentation builds:

```bash
git submodule update --init --recursive --depth 1
cmake -B build -G Ninja -S . \
  -DBUILD_WERROR=ON \
  -DBUILD_TESTS=ON \
  -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=OFF \
  -DBUILD_VIRTUALDISPLAY_PROBE=OFF \
  -DBUILD_VIRTUALDISPLAY_TOOLS=OFF \
  -DBUILD_VIRTUALDISPLAY_VULKAN_LAYER=OFF \
  -DBUILD_DOCS=OFF \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUNSHINE_ENABLE_WEBRTC=OFF
```

Verify `cmake --version`, `g++ --version`, `ninja --version`, and the configure exit code before Task 1. Do not install Visual Studio, the WDK, certificates, or drivers for this subsystem.

### Task 1: Portable capability gate

**Files:**
- Create: `tools/atmos_capability_probe_policy.h`
- Create: `tools/atmos_capability_probe_policy.cpp`
- Create: `tests/unit/test_atmos_capability_probe_policy.cpp`
- Modify: `tests/CMakeLists.txt:374-375`

**Interfaces:**
- Consumes: No earlier code; only standard-library types.
- Produces: canonical tagged endpoint-property observations, `probe_options`, `atmos_probe::evaluate(const probe_observation&) -> gate_result`, `to_string(mat_profile)`, and `to_string(diagnostic)` for the Windows adapter and application coordinator.

- [ ] **Step 1: Declare the portable contracts and write the first failing readiness tests**

Create `tools/atmos_capability_probe_policy.h` with this public shape:

```cpp
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace atmos_probe {
  using hresult_code = std::int32_t;

  enum class mat_profile { mat20, mat21 };

  struct probe_options {
    std::optional<std::string> endpoint_id;
  };

  enum class diagnostic {
    probe_runtime_error,
    selected_endpoint_not_found,
    selected_endpoint_not_active,
    selected_endpoint_not_display_audio,
    selected_endpoint_not_hdmi,
    spatial_configuration_unavailable,
    spatial_audio_unsupported,
    atmos_home_theater_unsupported,
    active_spatial_format_not_atmos_home_theater,
    mat20_exclusive_probe_failed,
    mat20_exclusive_format_unsupported,
    mat20_exclusive_initialize_failed,
    mat21_exclusive_probe_failed,
    mat21_exclusive_format_unsupported,
    mat21_exclusive_initialize_failed,
    no_exclusive_mat_profile_ready,
  };

  struct property_missing {};
  struct property_wrong_type { std::uint16_t variant_type; };
  struct display_audio_form_factor {};
  struct other_form_factor { std::uint32_t value; };
  using form_factor_observation = std::variant<
    property_missing,
    property_wrong_type,
    display_audio_form_factor,
    other_form_factor>;

  struct malformed_connector_guid { std::string raw; };
  struct hdmi_connector {};
  struct displayport_connector {};
  struct other_connector_guid { std::string canonical_guid; };
  using jack_subtype_observation = std::variant<
    property_missing,
    property_wrong_type,
    malformed_connector_guid,
    hdmi_connector,
    displayport_connector,
    other_connector_guid>;

  struct endpoint_observation {
    std::string id;
    std::string friendly_name;
    std::uint32_t state {};
    form_factor_observation form_factor {property_missing {}};
    jack_subtype_observation jack_subtype {property_missing {}};
  };

  struct role_endpoint_observation {
    std::string role;
    std::optional<endpoint_observation> endpoint;
  };

  struct spatial_observation {
    bool configuration_available {};
    bool spatial_audio_supported {};
    bool atmos_home_theater_supported {};
    std::string active_format_raw;
    std::string active_format_guid;
    std::string default_format_raw;
    std::string default_format_guid;
  };

  struct mat_observation {
    std::optional<hresult_code> format_support_hresult;
    std::optional<hresult_code> initialize_hresult;
  };

  struct api_error {
    std::string operation;
    hresult_code hresult;
  };

  struct probe_observation {
    bool probe_complete {true};
    std::optional<endpoint_observation> selected_endpoint;
    std::array<role_endpoint_observation, 3> default_endpoints;
    std::vector<endpoint_observation> active_endpoints;
    spatial_observation spatial;
    mat_observation mat21;
    mat_observation mat20;
    std::vector<api_error> errors;
  };

  struct gate_result {
    bool ready {};
    std::optional<mat_profile> selected_profile;
    std::vector<mat_profile> ready_profiles;
    std::vector<diagnostic> diagnostics;
  };

  gate_result evaluate(const probe_observation &observation);
  bool is_active(const endpoint_observation &endpoint);
  bool is_display_audio(const endpoint_observation &endpoint);
  bool is_hdmi(const endpoint_observation &endpoint);
  std::string_view to_string(mat_profile profile);
  std::string_view to_string(diagnostic value);
}
```

Create `tools/atmos_capability_probe_policy.cpp` with only this include so CMake has a real translation unit while the behavioral symbols remain deliberately undefined for RED:

```cpp
#include "tools/atmos_capability_probe_policy.h"
```

Write table-driven GoogleTests using literal observations and the tagged property alternatives. The first two tests assert that an otherwise valid observation with MAT21 `S_OK/S_OK` selects MAT21, while MAT20-only `S_OK/S_OK` selects MAT20 without inferring MAT21. Build expected vectors as literals rather than calling production name helpers. Because activity is derived from the raw state and connector/form-factor truth is represented by mutually exclusive variants, contradictory `inactive + active=true` or `malformed + hdmi=true` fixtures are not representable.

- [ ] **Step 2: Register and run the policy test to verify RED**

Add this registration outside `if(WIN32)` in `tests/CMakeLists.txt`:

```cmake
sunshine_register_component(
    NAME test_component_atmos_capability_probe_policy
    TEST_SOURCE unit/test_atmos_capability_probe_policy.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_policy.cpp"
)
```

Run from the configured MSYS2 UCRT64 build environment:

```bash
cmake --build build --target test_component_atmos_capability_probe_policy
ctest --test-dir build -R '^test_component_atmos_capability_probe_policy$' --output-on-failure
```

Expected RED: the target link fails with an undefined reference to `atmos_probe::evaluate`, proving the tests reach the declared production API.

- [ ] **Step 3: Implement the smallest fail-closed evaluator**

Implement exact-`S_OK` readiness (`HRESULT == 0`). Derive active state from raw state value `1` (`DEVICE_STATE_ACTIVE`), display-audio state only from the `display_audio_form_factor` alternative, and HDMI state only from the `hdmi_connector` alternative. Missing endpoint returns only `SELECTED_ENDPOINT_NOT_FOUND`. Otherwise emit diagnostics in endpoint, spatial, MAT20, MAT21, then summary order. If the spatial configuration is unavailable, do not emit subordinate spatial conditions. A profile is ready only when both optional HRESULTs are present and exactly zero.

Check ready profiles in MAT21-then-MAT20 preference order. When neither is ready, emit `*_PROBE_FAILED` for a missing support HRESULT, `*_FORMAT_UNSUPPORTED` for a nonzero support result, or `*_INITIALIZE_FAILED` when support is zero but initialization is missing/nonzero, followed by `NO_EXCLUSIVE_MAT_PROFILE_READY`. `PROBE_RUNTIME_ERROR` always prevents readiness.

Implement both `to_string` overloads with exhaustive switches. Diagnostic strings are the uppercase enum spellings, including `SELECTED_ENDPOINT_NOT_DISPLAY_AUDIO` and `SELECTED_ENDPOINT_NOT_HDMI` as separate failures.

- [ ] **Step 4: Add the failure matrix and verify GREEN**

Add focused tests for these hand-built cases:

```text
missing selected endpoint -> SELECTED_ENDPOINT_NOT_FOUND only
inactive endpoint -> SELECTED_ENDPOINT_NOT_ACTIVE
DigitalAudioDisplayDevice false -> SELECTED_ENDPOINT_NOT_DISPLAY_AUDIO
display audio true plus HDMI false -> SELECTED_ENDPOINT_NOT_HDMI
inactive raw state with otherwise passing canonical properties -> SELECTED_ENDPOINT_NOT_ACTIVE
DisplayPort, other GUID, malformed, missing, or wrong-type connector alternative -> SELECTED_ENDPOINT_NOT_HDMI
missing, wrong-type, or other form-factor alternative -> SELECTED_ENDPOINT_NOT_DISPLAY_AUDIO
spatial configuration unavailable -> SPATIAL_CONFIGURATION_UNAVAILABLE
spatial supported and provider supported but active GUID is Sonic -> ACTIVE_SPATIAL_FORMAT_NOT_ATMOS_HOME_THEATER
default format Atmos but active format not Atmos -> blocked
MAT21 support S_OK plus Initialize AUDCLNT_E_DEVICE_IN_USE -> MAT21_EXCLUSIVE_INITIALIZE_FAILED
both format checks unsupported -> MAT20/MAT21 format codes then NO_EXCLUSIVE_MAT_PROFILE_READY
MAT20 and MAT21 ready -> ready profile order [MAT21, MAT20], selected MAT21
probe_complete false -> PROBE_RUNTIME_ERROR even when every capability field otherwise passes
```

Above each test, name the realistic production mutation it catches: wrong branch, wrong exact GUID, `SUCCEEDED` instead of `S_OK`, profile inference, or diagnostic reordering.

Run:

```bash
cmake --build build --target test_component_atmos_capability_probe_policy
ctest --test-dir build -R '^test_component_atmos_capability_probe_policy$' --output-on-failure
```

Expected GREEN: the focused target passes without warnings or failures.

- [ ] **Step 5: Root agent commits the portable gate**

```bash
git add tools/atmos_capability_probe_policy.h tools/atmos_capability_probe_policy.cpp tests/unit/test_atmos_capability_probe_policy.cpp tests/CMakeLists.txt
git commit -m "test: define fail-closed Atmos capability gate"
```

### Task 2: Exact Windows endpoint and MAT observation

**Files:**
- Create: `tools/atmos_capability_probe_windows.h`
- Create: `tools/atmos_capability_probe_windows.cpp`
- Create: `tests/unit/platform/windows/test_atmos_capability_probe_windows.cpp`
- Modify: `tests/CMakeLists.txt` inside the existing `if(WIN32)` block

**Interfaces:**
- Consumes: `atmos_probe::probe_observation`, `probe_options`, `mat_profile`, canonical endpoint-property variants, `spatial_observation`, and `api_error` from Task 1.
- Produces: `make_mat_format(mat_profile)`, `canonical_guid(std::wstring_view)`, pure `decode_form_factor(const PROPVARIANT&)`/`decode_jack_subtype(const PROPVARIANT&)`, and `collect_windows_observation(const probe_options&)`.

- [ ] **Step 1: Declare the Windows seam and write exact descriptor/GUID tests**

Create `tools/atmos_capability_probe_windows.h`:

```cpp
#pragma once

#include "tools/atmos_capability_probe_policy.h"

#include <ksmedia.h>
#include <mmreg.h>
#include <propidl.h>
#include <optional>
#include <string>
#include <string_view>

namespace atmos_probe {
  inline constexpr GUID k_hdmi_interface {
    0xd1b9cc2a, 0xf519, 0x417f,
    {0x91, 0xc9, 0x55, 0xfa, 0x65, 0x48, 0x10, 0x01}
  };
  inline constexpr GUID k_displayport_interface {
    0xe47e4031, 0x3ea6, 0x418d,
    {0x8f, 0x9b, 0xb7, 0x38, 0x43, 0xcc, 0xba, 0x97}
  };
  inline constexpr GUID k_iec61937_dolby_mat20 {
    0x0000010c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };
  inline constexpr GUID k_iec61937_dolby_mat21 {
    0x0000030c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };

  WAVEFORMATEXTENSIBLE_IEC61937 make_mat_format(mat_profile profile);
  std::optional<std::string> canonical_guid(std::wstring_view raw);
  form_factor_observation decode_form_factor(const PROPVARIANT &value);
  jack_subtype_observation decode_jack_subtype(const PROPVARIANT &value);
  probe_observation collect_windows_observation(const probe_options &options);
}
```

Write tests with literal expected fields for both profiles:

```cpp
static_assert(sizeof(WAVEFORMATEXTENSIBLE_IEC61937) == 52);

TEST(AtmosCapabilityProbeWindows, BuildsExactMat21Descriptor) {
  const auto format = atmos_probe::make_mat_format(atmos_probe::mat_profile::mat21);
  EXPECT_EQ(format.FormatExt.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
  EXPECT_EQ(format.FormatExt.Format.nChannels, 8);
  EXPECT_EQ(format.FormatExt.Format.nSamplesPerSec, 192000u);
  EXPECT_EQ(format.FormatExt.Format.nAvgBytesPerSec, 3072000u);
  EXPECT_EQ(format.FormatExt.Format.nBlockAlign, 16);
  EXPECT_EQ(format.FormatExt.Format.wBitsPerSample, 16);
  EXPECT_EQ(format.FormatExt.Format.cbSize, 34);
  EXPECT_EQ(format.FormatExt.Samples.wValidBitsPerSample, 16);
  EXPECT_EQ(format.FormatExt.dwChannelMask, KSAUDIO_SPEAKER_7POINT1);
  EXPECT_TRUE(IsEqualGUID(format.FormatExt.SubFormat, atmos_probe::k_iec61937_dolby_mat21));
  EXPECT_EQ(format.dwEncodedSamplesPerSec, 96000u);
  EXPECT_EQ(format.dwEncodedChannelCount, 8u);
  EXPECT_EQ(format.dwAverageBytesPerSec, 0u);
}
```

Add the corresponding MAT20 test plus property-decoder tests that:

```text
accept official HDMI {D1B9CC2A-F519-417F-91C9-55FA65481001}
reject official DisplayPort {E47E4031-3EA6-418D-8F9B-B73843CCBA97}
reject the previous erroneous HDMI literal {D9E55EA0-0C89-4692-84FF-EB3C4B0D172F}
reject the previous erroneous DisplayPort literal {E47E4031-3EA6-418D-8F9B-B73843CCB2AD}
distinguish VT_EMPTY, VT_CLSID, null VT_LPWSTR, malformed text, and other valid GUIDs
accept only VT_UI4 DigitalAudioDisplayDevice for the display-audio form factor
distinguish missing, wrong-type, and other numeric form-factor values
```

Use real `PROPVARIANT` values in these tests so the exact property-type boundary is exercised.

Create `tools/atmos_capability_probe_windows.cpp` with only the Windows header include before the RED run:

```cpp
#include "tools/atmos_capability_probe_windows.h"
```

- [ ] **Step 2: Register and run the Windows helper test to verify RED**

Inside the existing `if(WIN32)` block in `tests/CMakeLists.txt`, add:

```cmake
sunshine_register_component(
    NAME test_component_atmos_capability_probe_windows
    TEST_SOURCE unit/platform/windows/test_atmos_capability_probe_windows.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_windows.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_policy.cpp"
    LINK_LIBRARIES ole32 windowsapp ksuser
)
```

Run:

```bash
cmake --build build --target test_component_atmos_capability_probe_windows
ctest --test-dir build -R '^test_component_atmos_capability_probe_windows$' --output-on-failure
```

Expected RED: undefined references to `make_mat_format`, `canonical_guid`, `decode_form_factor`, and `decode_jack_subtype`.

- [ ] **Step 3: Implement exact descriptor and connector helpers**

Zero-initialize the descriptor and assign every documented field. Use `CLSIDFromString` for the endpoint property because `PKEY_AudioEndpoint_JackSubType` is `VT_LPWSTR`, not `VT_CLSID`. Normalize with `StringFromGUID2` into uppercase braced form. Compare the parsed GUID with the local authoritative `k_hdmi_interface` and `k_displayport_interface` values, never a friendly name or an unavailable MinGW SDK symbol. The decoders return mutually exclusive tagged alternatives, so missing, wrong-type, malformed, HDMI, DisplayPort, and other values cannot be represented simultaneously.

In the Windows source, put `#define INITGUID` before the relevant Windows headers and define `PKEY_Device_FriendlyName` locally using the same documented property key and pattern as `tools/audio.cpp`; the current MinGW header set does not reliably declare it.

Run the Windows helper test again and require GREEN before adding any MMDevice calls.

- [ ] **Step 4: Collect real endpoint and spatial observations without rendering**

Implement `collect_windows_observation` with these exact operations:

1. Create `IMMDeviceEnumerator` and enumerate `eRender`, `DEVICE_STATE_ACTIVE` endpoints.
2. Read all three role defaults (`eConsole`, `eMultimedia`, `eCommunications`) into the fixed role array.
3. Select `GetDefaultAudioEndpoint(eRender, eConsole)` unless `endpoint_id` is present; for an explicit ID call `IMMDeviceEnumerator::GetDevice` and keep a not-found selection as a normal blocked observation.
4. For every endpoint, read `GetId`, `GetState`, `PKEY_Device_FriendlyName`, `PKEY_AudioEndpoint_FormFactor`, and `PKEY_AudioEndpoint_JackSubType`. Pass the exact `PROPVARIANT`s through the production decoders; do not store independent `active`, `is_display_audio`, or `is_hdmi` booleans.
5. Call `SpatialAudioDeviceConfiguration::GetForDeviceId(winrt::to_hstring(endpoint.id))`; record `IsSpatialAudioSupported`, `IsSpatialAudioFormatSupported(SpatialAudioFormatSubtype::DolbyAtmosForHomeTheater())`, and raw/canonical active and default format strings.
6. For MAT21 then MAT20, activate a fresh `IAudioClient`, call exclusive `IsFormatSupported` with `nullptr` closest-match output, release it, activate another fresh client, and call `Initialize` only when exact support returned `S_OK`.
7. Release a successfully initialized client immediately. Do not obtain a render service, buffer, or start the stream.

Use the `util::safe_ptr`/`Release`/`CoTaskMemFree` and `PROPVARIANT` RAII patterns already present in `tools/audio.cpp`. Limit C++/WinRT to the spatial configuration API. Convert caught `winrt::hresult_error` values to `api_error` entries and mark only genuinely incomplete top-level enumeration as `probe_complete=false`; endpoint/spatial/profile failures retain their precise gate fields and HRESULTs. The public Windows adapter exposes observations and preflight operations only—no renderer type, render callback, or payload-writing abstraction belongs in this seam.

- [ ] **Step 5: Audit for render calls and verify both tests**

Run:

```bash
rg -n 'GetService|IAudioRenderClient|GetBuffer|ReleaseBuffer|->Start\(' tools/atmos_capability_probe*.h tools/atmos_capability_probe*.cpp
cmake --build build --target test_component_atmos_capability_probe_policy test_component_atmos_capability_probe_windows
ctest --test-dir build -R '^test_component_atmos_capability_probe_(policy|windows)$' --output-on-failure
```

Expected: the lint-style source audit has no matches and both tests pass. This confirms there is no known render-buffer/start path in the probe sources; the stronger evidence is the structurally narrow adapter plus independent code review, not text search alone.

- [ ] **Step 6: Root agent commits the Windows observation layer**

```bash
git add tools/atmos_capability_probe_windows.h tools/atmos_capability_probe_windows.cpp tests/unit/platform/windows/test_atmos_capability_probe_windows.cpp tests/CMakeLists.txt
git commit -m "feat: probe exact Windows HDMI Atmos capability"
```

### Task 3: Strict CLI, invariant-checked report, and two-machine smoke

**Files:**
- Create: `tools/atmos_capability_probe_cli.h`
- Create: `tools/atmos_capability_probe_cli.cpp`
- Create: `tools/atmos_capability_probe_app.h`
- Create: `tools/atmos_capability_probe_app.cpp`
- Create: `tools/atmos_capability_probe.cpp`
- Create: `tests/unit/test_atmos_capability_probe_cli.cpp`
- Create: `tests/unit/test_atmos_capability_probe_app.cpp`
- Modify: `tools/CMakeLists.txt:31-38`
- Modify: `tests/CMakeLists.txt` near the portable capability-policy registration

**Interfaces:**
- Consumes: `probe_options`, `probe_observation`, `gate_result`, `evaluate`, and the enum-name helpers from Tasks 1-2.
- Produces: a deterministic application coordinator plus executable `build/tools/atmos-capability-probe.exe`; exit `0` for `ENDPOINT_PREFLIGHT_READY` or help, `1` for `BLOCKED`, `2` for invalid CLI, and `3` for provider/runtime/report-invariant failure.

- [ ] **Step 1: Write CLI parser tests before its implementation**

Declare this Windows-free interface in `tools/atmos_capability_probe_cli.h`:

```cpp
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace atmos_probe {
  struct cli_options {
    bool json {};
    bool help {};
    std::optional<std::string> endpoint_id;
  };

  struct cli_parse_result {
    std::optional<cli_options> options;
    std::string error;
  };

  cli_parse_result parse_cli(std::span<const std::string_view> arguments);
}
```

Tests assert these literal behaviors:

```text
no arguments -> human output, default eConsole
--json -> JSON output, default eConsole
--endpoint-id <exact value> --json -> value preserved byte-for-byte
--help -> help true
--endpoint-id without value -> error "--endpoint-id requires a value"
duplicate --endpoint-id -> error "--endpoint-id may be specified once"
unknown option -> error "unknown option: --bogus"
```

Create `tools/atmos_capability_probe_cli.cpp` with only the CLI header include before the RED run. Register `test_component_atmos_capability_probe_cli` outside `if(WIN32)`, build it, observe the expected undefined-reference RED, implement only the declared grammar, rerun, and require GREEN.

- [ ] **Step 2: Write the coordinator and report-invariant tests before implementation**

Declare this Windows-free seam in `tools/atmos_capability_probe_app.h`:

```cpp
#pragma once

#include "tools/atmos_capability_probe_policy.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace atmos_probe {
  using observation_provider =
    std::function<probe_observation(const probe_options &)>;

  struct app_result {
    int exit_code {};
    std::string standard_output;
    std::string standard_error;
  };

  struct report_validation_result {
    bool valid {};
    std::string error;
  };

  app_result run_probe(
    std::span<const std::string_view> arguments,
    const observation_provider &provider);
  report_validation_result validate_serialized_report(std::string_view json);
}
```

Use `nlohmann::ordered_json` privately in the implementation rather than exposing its concrete template in the public header. Write `test_atmos_capability_probe_app.cpp` with a lambda provider that captures the received `probe_options`, returns one literal canonical MAT21-ready observation, and calls `run_probe` with a `std::array<std::string_view, 3>{"--endpoint-id", "exact-id", "--json"}`. Parse the returned JSON and assert all of the following together:

```text
captured provider endpoint_id == exact-id
selection.kind == explicit
selection.requested_endpoint_id == exact-id
selected_endpoint.id == exact-id
gate.verdict == ENDPOINT_PREFLIGHT_READY
gate.selected_profile == MAT21
gate.selected_profile is present in gate.ready_profiles
the matching MAT21 object has format_support_hresult == 0x00000000
the matching MAT21 object has initialize_hresult == 0x00000000
selected endpoint state is active, form-factor status is DISPLAY_AUDIO, connector status is HDMI
spatial_audio.active_format_guid == {A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}
audio_bytes_written == false
exit_code == 0
```

Add fixtures that mutate one prerequisite at a time (selected ID, state, form factor, connector, active Atmos GUID, selected/ready profile membership, either MAT HRESULT, or `audio_bytes_written`), reserialize, and require `validate_serialized_report` to reject the inconsistent green report. Also test a canonical blocked observation maps to exit `1`, invalid arguments map to exit `2` without invoking the provider, a throwing provider maps to exit `3`, and help maps to exit `0` without invoking the provider.

Register outside `if(WIN32)`:

```cmake
sunshine_register_component(
    NAME test_component_atmos_capability_probe_app
    TEST_SOURCE unit/test_atmos_capability_probe_app.cpp
    PRODUCT_SOURCES
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_app.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_cli.cpp"
        "${SUNSHINE_TEST_REPOSITORY_ROOT}/tools/atmos_capability_probe_policy.cpp"
    LINK_LIBRARIES nlohmann_json::nlohmann_json
)
```

The repository's common dependency module already makes `nlohmann_json::nlohmann_json` available globally. Build this target before defining `run_probe` and require an undefined-reference RED that proves the test reaches the production coordinator.

- [ ] **Step 3: Implement explicit selection wiring and the stable report**

Implement `run_probe` so the parsed endpoint ID is copied byte-for-byte into `probe_options`, the provider is invoked exactly once, and the selection object is constructed by the coordinator rather than trusted from the provider. Call `evaluate`, construct a `nlohmann::ordered_json` report, serialize it, then call the same `validate_serialized_report` entry point exercised by tests. Any exception or invalid green invariant returns exit `3` and must never emit `ENDPOINT_PREFLIGHT_READY`.

Use this field order:

```json
{
  "schema_version": 1,
  "selection": {"kind": "default:eConsole", "requested_endpoint_id": null},
  "selected_endpoint": null,
  "default_render_endpoints": {"console": null, "multimedia": null, "communications": null},
  "active_render_endpoints": [],
  "spatial_audio": {
    "configuration_available": false,
    "is_spatial_audio_supported": false,
    "atmos_home_theater_supported": false,
    "active_format_raw": "",
    "active_format_guid": "",
    "default_format_raw": "",
    "default_format_guid": ""
  },
  "mat_profiles": [],
  "probe_errors": [],
  "gate": {"verdict": "BLOCKED", "selected_profile": null, "ready_profiles": [], "diagnostics": []},
  "audio_bytes_written": false
}
```

Each endpoint object derives all presentation fields from its canonical observation in one serializer: `id`, `friendly_name`, numeric `state`, derived `state_name`, a tagged `form_factor` object, a tagged `jack_subtype` object, and derived `is_display_audio`/`is_hdmi`. No independently collected readiness booleans are serialized. Each MAT object contains `profile`, nullable fixed-width `format_support_hresult`, nullable fixed-width `initialize_hresult`, human-readable HRESULT labels, and derived `ready`. HRESULT strings use `0x` plus exactly eight uppercase hexadecimal digits. Recognize `S_OK`, `AUDCLNT_E_DEVICE_IN_USE`, `AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED`, `AUDCLNT_E_DEVICE_INVALIDATED`, and `AUDCLNT_E_UNSUPPORTED_FORMAT`; preserve unknown values as `UNKNOWN_HRESULT`.

Before green serialization, require: an explicit request matches `selected_endpoint.id`; the endpoint is active display-audio HDMI; the exact active Atmos Home Theater GUID is present; selected profile is a member of `ready_profiles`; and that same MAT object has both HRESULTs exactly zero. Human output starts with `ENDPOINT_PREFLIGHT_READY: MAT21`, `ENDPOINT_PREFLIGHT_READY: MAT20`, or `BLOCKED`, then prints the selected endpoint and diagnostics. Help and both output modes state: "Endpoint preflight does not prove downstream receiver Atmos lock." Neither mode writes a file or Windows setting.

Make `tools/atmos_capability_probe.cpp` a thin entry point: initialize one STA with `winrt::init_apartment`, pass `collect_windows_observation` to `run_probe`, print its returned streams, and return its exit code. Catch apartment initialization failure and map it to exit `3`.

- [ ] **Step 4: Add the Windows-only executable target and verify GREEN**

Add to `tools/CMakeLists.txt` without an extra `if(WIN32)` because the entire tools subdirectory is already Windows-only:

```cmake
add_executable(atmos-capability-probe
        atmos_capability_probe.cpp
        atmos_capability_probe_app.cpp
        atmos_capability_probe_cli.cpp
        atmos_capability_probe_policy.cpp
        atmos_capability_probe_windows.cpp)
target_link_libraries(atmos-capability-probe
        nlohmann_json::nlohmann_json
        ole32
        windowsapp
        ksuser
        ${PLATFORM_LIBRARIES}
)
target_compile_options(atmos-capability-probe PRIVATE ${SUNSHINE_COMPILE_OPTIONS})
```

Do not add `mmdevapi`, `uuid`, `propsys`, Boost, or logging sources unless a concrete linker error identifies a missing import. Do not add the feasibility tool to packaging or the CI PDB artifact list. Run all four focused component tests and require GREEN.

- [ ] **Step 5: Build and capture the host baseline**

From MSYS2 UCRT64:

```bash
cmake --build build --target \
  test_component_atmos_capability_probe_policy \
  test_component_atmos_capability_probe_cli \
  test_component_atmos_capability_probe_app \
  test_component_atmos_capability_probe_windows \
  atmos-capability-probe
ctest --test-dir build -R '^test_component_atmos_capability_probe_(policy|cli|app|windows)$' --output-on-failure
./build/tools/atmos-capability-probe.exe --json > build/atmos-host.json
host_exit=$?
sha256sum ./build/tools/atmos-capability-probe.exe > build/atmos-probe.sha256
ldd ./build/tools/atmos-capability-probe.exe
```

Accept host exit `0` or `1`; any other code is a probe failure. Parse the report and run the same invariant checks as the application test: schema `1`, no audio bytes, exact requested/selected ID equality when explicit, and every `ENDPOINT_PREFLIGHT_READY` prerequisite. Record the executable hash and stage any non-system UCRT64 runtime DLLs reported by `ldd` adjacent to the executable; do not install them. The host may truthfully be blocked because this is an unmodified-driver baseline.

- [ ] **Step 6: Run the identical executable on the laptop without installing it**

First verify the laptop SSH host key still matches `SHA256:ERhoPppZFQUtqJqRQv7ARDLNnL3HQiOMvHeDs01NA7o`. Once the host public key is authorized for `matt`, use `192.168.0.22` and fall back to `192.168.0.23` only if Ethernet is unreachable. Copy the executable and only its adjacent runtime DLL closure to a newly created `%TEMP%\vibepollo-atmos-probe` directory, execute it there, and retrieve JSON; do not install or register anything.

Compute SHA-256 on both machines and require the laptop executable hash to equal the recorded host hash. Run default eConsole first. If it is not the intended physical HDMI endpoint, choose only an exact ID from `active_render_endpoints` whose canonical form-factor status is `DISPLAY_AUDIO` and connector status is `HDMI`, then rerun with `--endpoint-id`. Never select from friendly-name text alone. Preserve the process exit code and both JSON reports.

Laptop endpoint-preflight acceptance is:

```text
process exit == 0
gate.verdict == ENDPOINT_PREFLIGHT_READY
selection.requested_endpoint_id == selected_endpoint.id when explicit
gate.selected_profile == MAT21 or MAT20 and is present in gate.ready_profiles
selected_endpoint.state == active
selected_endpoint.form_factor.status == DISPLAY_AUDIO
selected_endpoint.jack_subtype.status == HDMI
spatial_audio.active_format_guid == {A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}
the selected MAT profile has format support == 0x00000000 and Initialize == 0x00000000
audio_bytes_written == false
laptop executable SHA-256 == host executable SHA-256
```

If the probe is `BLOCKED`, retain the JSON and exit `1` as the measured stop condition. Do not change the endpoint, spatial configuration, driver, boot policy, or live Vibepollo installation in this slice.

- [ ] **Step 7: Final audit and root-owned commit**

```bash
rg -n 'GetService|IAudioRenderClient|GetBuffer|ReleaseBuffer|->Start\(' tools/atmos_capability_probe*.h tools/atmos_capability_probe*.cpp
git diff --check
git status --short
git add tools/atmos_capability_probe.cpp tools/atmos_capability_probe_app.h tools/atmos_capability_probe_app.cpp tools/atmos_capability_probe_cli.h tools/atmos_capability_probe_cli.cpp tools/CMakeLists.txt tests/unit/test_atmos_capability_probe_app.cpp tests/unit/test_atmos_capability_probe_cli.cpp tests/CMakeLists.txt
git commit -m "feat: add no-write Atmos capability probe"
```

Expected: the prohibited-call audit has no matches; diff check is clean; all four focused tests pass; the executable builds; host and laptop executable hashes match; and both machine reports are preserved outside committed repository content. Independently review the complete probe diff for any indirect renderer-service, buffer, start, setting-write, or false-green path.

## Completion Gate

This subsystem is complete only when:

- the portable truth table enforces exact active-format, canonical HDMI, and independent MAT-profile behavior;
- the property-decoder regressions accept only Microsoft's authoritative HDMI GUID and reject DisplayPort, both former bad literals, missing/wrong-type/malformed values, and other GUIDs;
- the descriptor test proves every Microsoft MAT20/MAT21 field and the 52-byte layout;
- the coordinator test proves explicit endpoint ID reaches the provider and exact selected ID/report, and all exit codes/invariants are enforced;
- the structural adapter boundary, full-source audit, and independent review find no renderer service, audio buffer, payload write, or stream start path;
- host JSON is captured as the unmodified-driver baseline;
- the byte-identical executable runs on the laptop's selected physical HDMI route;
- the laptop report is `ENDPOINT_PREFLIGHT_READY` for at least one exact MAT profile, or its precise `BLOCKED` diagnostic and exit `1` are retained as the stop condition;
- no driver, boot-policy, service, live Vibepollo, or Windows spatial-setting mutation occurred;
- even after `ENDPOINT_PREFLIGHT_READY`, no driver/test-signing stage begins until a separate legal OS-generated spatial stream makes the real TV/eARC/soundbar receiver report Atmos lock on that route.

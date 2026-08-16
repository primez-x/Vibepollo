#define INITGUID

#include "tools/atmos_spatial_lock_test.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <spatialaudioclient.h>

#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING

namespace {
  using atmos_spatial_lock::api_result;
  using atmos_spatial_lock::diagnostic;
  using atmos_spatial_lock::hresult_code;
  using atmos_spatial_lock::object_capabilities;
  using atmos_spatial_lock::object_format;
  using atmos_spatial_lock::route_observation;
  using atmos_spatial_lock::run_report;

  inline constexpr GUID k_hdmi_interface {
    0xd1b9cc2a, 0xf519, 0x417f,
    {0x91, 0xc9, 0x55, 0xfa, 0x65, 0x48, 0x10, 0x01}
  };
  inline constexpr GUID k_float_subformat {
    0x00000003, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };

  struct role_spec {
    const char *name;
    ERole role;
  };

  constexpr std::array k_default_roles {
    role_spec {"console", eConsole},
    role_spec {"multimedia", eMultimedia},
    role_spec {"communications", eCommunications},
  };

  constexpr std::array k_static_bed_objects {
    AudioObjectType_FrontLeft,
    AudioObjectType_FrontRight,
    AudioObjectType_FrontCenter,
    AudioObjectType_LowFrequency,
    AudioObjectType_SideLeft,
    AudioObjectType_SideRight,
    AudioObjectType_BackLeft,
    AudioObjectType_BackRight,
    AudioObjectType_TopFrontLeft,
    AudioObjectType_TopFrontRight,
    AudioObjectType_TopBackLeft,
    AudioObjectType_TopBackRight,
  };

  static_assert(atmos_spatial_lock::required_static_object_mask == 0x1ffe);
  static_assert(k_static_bed_objects.size() == 12);

  constexpr std::uint32_t static_bed_object_mask() {
    std::uint32_t mask {};
    for (const auto type : k_static_bed_objects) {
      mask |= static_cast<std::uint32_t>(type);
    }
    return mask;
  }

  constexpr bool static_bed_object_values_unique() {
    for (std::size_t first = 0; first < k_static_bed_objects.size(); ++first) {
      for (std::size_t second = first + 1;
           second < k_static_bed_objects.size();
           ++second) {
        if (k_static_bed_objects[first] == k_static_bed_objects[second]) {
          return false;
        }
      }
    }
    return true;
  }

  static_assert(static_bed_object_values_unique());
  static_assert(
    static_bed_object_mask() == atmos_spatial_lock::required_static_object_mask);

  class prop_variant {
  public:
    prop_variant() {
      PropVariantInit(&value);
    }

    prop_variant(const prop_variant &) = delete;
    prop_variant &operator=(const prop_variant &) = delete;

    ~prop_variant() {
      PropVariantClear(&value);
    }

    PROPVARIANT value;
  };

  class unique_handle {
  public:
    unique_handle() = default;
    explicit unique_handle(HANDLE value): value_(value) {}

    unique_handle(const unique_handle &) = delete;
    unique_handle &operator=(const unique_handle &) = delete;

    unique_handle(unique_handle &&other) noexcept:
        value_(std::exchange(other.value_, nullptr)) {}

    unique_handle &operator=(unique_handle &&other) noexcept {
      if (this != &other) {
        reset();
        value_ = std::exchange(other.value_, nullptr);
      }
      return *this;
    }

    ~unique_handle() {
      reset();
    }

    explicit operator bool() const {
      return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    HANDLE get() const {
      return value_;
    }

    void reset() {
      if (*this) {
        CloseHandle(value_);
      }
      value_ = nullptr;
    }

  private:
    HANDLE value_ {};
  };

  struct cotask_deleter {
    template<class T>
    void operator()(T *value) const {
      CoTaskMemFree(value);
    }
  };

  template<class T>
  using cotask_ptr = std::unique_ptr<T, cotask_deleter>;

  struct spatial_preparation {
    winrt::com_ptr<ISpatialAudioClient> client;
    WAVEFORMATEXTENSIBLE format_storage {};
    std::size_t format_size {};

    WAVEFORMATEX *format() {
      return reinterpret_cast<WAVEFORMATEX *>(&format_storage);
    }

    const WAVEFORMATEX *format() const {
      return reinterpret_cast<const WAVEFORMATEX *>(&format_storage);
    }
  };

  struct stream_bundle {
    unique_handle event;
    winrt::com_ptr<ISpatialAudioObjectRenderStream> stream;
    std::vector<winrt::com_ptr<ISpatialAudioObject>> objects;
  };

  struct stream_cleanup_context {
    ISpatialAudioObjectRenderStream *stream {};
    run_report *report {};
    std::uint32_t samples_per_second {};
    std::chrono::steady_clock::time_point started_at;
    HRESULT stop_hresult {E_UNEXPECTED};
    HRESULT reset_hresult {E_UNEXPECTED};
    bool attempted {};
  };

  void stop_and_reset_stream(void *raw_context) noexcept {
    auto &context = *static_cast<stream_cleanup_context *>(raw_context);
    context.attempted = true;
    context.stop_hresult = context.stream->Stop();
    context.reset_hresult = context.stream->Reset();
    context.report->stopped = context.stop_hresult == S_OK;
    if (context.samples_per_second != 0) {
      context.report->submitted_duration_ms =
        static_cast<double>(context.report->frame_count) * 1000.0 /
        static_cast<double>(context.samples_per_second);
    }
    context.report->wall_elapsed_ms =
      std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - context.started_at).count();
  }

  struct update_cleanup_context {
    ISpatialAudioObjectRenderStream *stream {};
    HRESULT end_hresult {E_UNEXPECTED};
    bool attempted {};
  };

  void end_audio_update(void *raw_context) noexcept {
    auto &context = *static_cast<update_cleanup_context *>(raw_context);
    context.attempted = true;
    context.end_hresult = context.stream->EndUpdatingAudioObjects();
  }

  void append_diagnostic(run_report &report, const diagnostic value) {
    if (std::ranges::find(report.diagnostics, value) == report.diagnostics.end()) {
      report.diagnostics.push_back(value);
    }
  }

  void record_hresult(
    run_report &report,
    std::string operation,
    const HRESULT hresult) {
    report.hresults.push_back(api_result {
      .operation = std::move(operation),
      .hresult = static_cast<hresult_code>(hresult),
    });
  }

  void record_exception(
    run_report &report,
    std::string operation,
    const winrt::hresult_error &error) {
    record_hresult(report, std::move(operation), error.code());
  }

  void record_unhandled_exception_noexcept(
    run_report &report,
    const HRESULT hresult,
    const char *operation) noexcept {
    try {
      record_hresult(report, operation, hresult);
      append_diagnostic(
        report,
        report.started ? diagnostic::render_update_failed :
                         diagnostic::route_observation_failed);
    } catch (...) {
      // The emergency JSON path below requires no report allocations.
    }
  }

  void write_emergency_report(const run_report &report) noexcept {
    std::fprintf(
      stdout,
      "{\"schema_version\":1,\"tool\":\"atmos-spatial-lock-test\","
      "\"ready\":false,\"emergency_report\":true,"
      "\"physical_receiver_lock_requires_external_observation\":true,"
      "\"endpoint\":{\"present\":false,\"id\":\"\",\"friendly_name\":\"\","
      "\"state\":0,\"display_audio\":false,\"hdmi\":false},"
      "\"negotiated_object_format\":null,\"object_capabilities\":{},"
      "\"route_bookends\":{},\"update_count\":0,\"frame_count\":0,"
      "\"requested_duration_ms\":%u,\"submitted_duration_ms\":0.0,"
      "\"wall_elapsed_ms\":0.0,\"drain_completion_observed\":false,"
      "\"peak_sample\":0.0,\"hresults\":[],"
      "\"diagnostics\":[\"UNHANDLED_EXCEPTION\"],"
      "\"started\":%s,\"stopped\":%s,\"route_stable\":false,"
      "\"audio_generated\":false,\"dynamic_object_activated\":false}\n",
      report.requested_duration_ms,
      report.started ? "true" : "false",
      report.stopped ? "true" : "false");
  }

  int write_report(run_report &report) noexcept {
    const int desired_exit = report.ready ? 0 :
      atmos_spatial_lock::failure_exit_code(report.started);
    try {
      const std::string json = atmos_spatial_lock::serialize_report(report);
      if (std::fwrite(json.data(), 1, json.size(), stdout) == json.size()) {
        return desired_exit;
      }
      return atmos_spatial_lock::failure_exit_code(report.started);
    } catch (...) {
      report.ready = false;
      write_emergency_report(report);
      return atmos_spatial_lock::failure_exit_code(report.started);
    }
  }

  std::string wide_to_utf8(const std::wstring_view raw) {
    if (raw.empty() ||
        raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }
    const auto raw_size = static_cast<int>(raw.size());
    const int output_size = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      raw.data(),
      raw_size,
      nullptr,
      0,
      nullptr,
      nullptr);
    if (output_size <= 0) {
      return {};
    }
    std::string result(static_cast<std::size_t>(output_size), '\0');
    if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          raw.data(),
          raw_size,
          result.data(),
          output_size,
          nullptr,
          nullptr) != output_size) {
      return {};
    }
    return result;
  }

  std::string canonical_guid(const GUID &guid) {
    std::array<wchar_t, 39> text {};
    if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) == 0) {
      return {};
    }
    return wide_to_utf8(text.data());
  }

  std::string canonical_guid(const std::wstring_view raw) {
    const std::wstring null_terminated {raw};
    GUID guid {};
    if (FAILED(CLSIDFromString(null_terminated.c_str(), &guid))) {
      return {};
    }
    return canonical_guid(guid);
  }

  bool get_device_id(
    IMMDevice *device,
    std::string &id,
    run_report &report,
    const std::string_view operation) {
    LPWSTR raw_id {};
    const HRESULT hresult = device->GetId(&raw_id);
    record_hresult(report, std::string {operation}, hresult);
    cotask_ptr<WCHAR> owned_id {raw_id};
    if (hresult != S_OK || raw_id == nullptr) {
      return false;
    }
    id = wide_to_utf8(raw_id);
    return !id.empty();
  }

  bool observe_endpoint(
    IMMDevice *device,
    atmos_spatial_lock::endpoint_observation &endpoint,
    run_report &report,
    const std::string_view bookend) {
    bool complete = true;
    const std::string prefix = "route[" + std::string {bookend} + "].endpoint";
    endpoint.present = get_device_id(
      device,
      endpoint.id,
      report,
      prefix + ".GetId");
    complete = complete && endpoint.present;

    DWORD state {};
    HRESULT hresult = device->GetState(&state);
    record_hresult(report, prefix + ".GetState", hresult);
    if (hresult == S_OK) {
      endpoint.state = state;
    } else {
      complete = false;
    }

    winrt::com_ptr<IPropertyStore> properties;
    hresult = device->OpenPropertyStore(STGM_READ, properties.put());
    record_hresult(report, prefix + ".OpenPropertyStore", hresult);
    if (hresult != S_OK) {
      return false;
    }

    prop_variant friendly_name;
    hresult = properties->GetValue(PKEY_Device_FriendlyName, &friendly_name.value);
    record_hresult(report, prefix + ".PKEY_Device_FriendlyName", hresult);
    if (hresult == S_OK && friendly_name.value.vt == VT_LPWSTR &&
        friendly_name.value.pwszVal != nullptr) {
      endpoint.friendly_name = wide_to_utf8(friendly_name.value.pwszVal);
    }

    prop_variant form_factor;
    hresult = properties->GetValue(PKEY_AudioEndpoint_FormFactor, &form_factor.value);
    record_hresult(report, prefix + ".PKEY_AudioEndpoint_FormFactor", hresult);
    if (hresult == S_OK) {
      endpoint.display_audio =
        form_factor.value.vt == VT_UI4 &&
        form_factor.value.ulVal == static_cast<ULONG>(DigitalAudioDisplayDevice);
    } else {
      complete = false;
    }

    prop_variant jack_subtype;
    hresult = properties->GetValue(PKEY_AudioEndpoint_JackSubType, &jack_subtype.value);
    record_hresult(report, prefix + ".PKEY_AudioEndpoint_JackSubType", hresult);
    if (hresult == S_OK && jack_subtype.value.vt == VT_LPWSTR &&
        jack_subtype.value.pwszVal != nullptr) {
      GUID connector {};
      endpoint.hdmi =
        CLSIDFromString(jack_subtype.value.pwszVal, &connector) == S_OK &&
        IsEqualGUID(connector, k_hdmi_interface);
    } else if (hresult != S_OK) {
      complete = false;
    }
    return complete;
  }

  route_observation observe_route(
    run_report &report,
    const std::string_view bookend) {
    route_observation route;
    bool complete = true;
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hresult = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      enumerator.put_void());
    record_hresult(
      report,
      "route[" + std::string {bookend} + "].CoCreateInstance[MMDeviceEnumerator]",
      hresult);
    if (hresult != S_OK) {
      return route;
    }

    winrt::com_ptr<IMMDevice> selected;
    hresult = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, selected.put());
    record_hresult(
      report,
      "route[" + std::string {bookend} + "].GetDefaultAudioEndpoint[console]",
      hresult);
    if (hresult == S_OK && selected) {
      complete = observe_endpoint(
        selected.get(),
        route.endpoint,
        report,
        bookend) && complete;
    } else {
      complete = false;
    }

    for (std::size_t index = 0; index < k_default_roles.size(); ++index) {
      const auto &role = k_default_roles[index];
      winrt::com_ptr<IMMDevice> device;
      hresult = enumerator->GetDefaultAudioEndpoint(eRender, role.role, device.put());
      record_hresult(
        report,
        "route[" + std::string {bookend} + "].GetDefaultAudioEndpoint[" +
          role.name + "]",
        hresult);
      if (hresult != S_OK || !device ||
          !get_device_id(
            device.get(),
            route.core_default_ids[index],
            report,
            "route[" + std::string {bookend} + "].default[" + role.name + "].GetId")) {
        complete = false;
      }
    }

    using winrt::Windows::Media::Devices::AudioDeviceRole;
    using winrt::Windows::Media::Devices::MediaDevice;
    try {
      route.winrt_default_id = winrt::to_string(
        MediaDevice::GetDefaultAudioRenderId(AudioDeviceRole::Default));
      record_hresult(
        report,
        "route[" + std::string {bookend} + "].MediaDevice::GetDefaultAudioRenderId[Default]",
        S_OK);
    } catch (const winrt::hresult_error &error) {
      record_exception(
        report,
        "route[" + std::string {bookend} + "].MediaDevice::GetDefaultAudioRenderId[Default]",
        error);
      complete = false;
    }
    try {
      route.winrt_communications_id = winrt::to_string(
        MediaDevice::GetDefaultAudioRenderId(AudioDeviceRole::Communications));
      record_hresult(
        report,
        "route[" + std::string {bookend} + "].MediaDevice::GetDefaultAudioRenderId[Communications]",
        S_OK);
    } catch (const winrt::hresult_error &error) {
      record_exception(
        report,
        "route[" + std::string {bookend} + "].MediaDevice::GetDefaultAudioRenderId[Communications]",
        error);
      complete = false;
    }

    using winrt::Windows::Media::Audio::SpatialAudioDeviceConfiguration;
    using winrt::Windows::Media::Audio::SpatialAudioFormatSubtype;
    if (!route.winrt_default_id.empty()) {
      try {
        const auto input_id = winrt::to_hstring(route.winrt_default_id);
        auto configuration = SpatialAudioDeviceConfiguration::GetForDeviceId(input_id);
        record_hresult(
          report,
          "route[" + std::string {bookend} + "].SpatialAudioDeviceConfiguration::GetForDeviceId",
          S_OK);
        if (configuration) {
          route.spatial_configuration_available = true;
          route.spatial_configuration_id = winrt::to_string(configuration.DeviceId());
          route.spatial_audio_supported = configuration.IsSpatialAudioSupported();
          const auto atmos = SpatialAudioFormatSubtype::DolbyAtmosForHomeTheater();
          route.atmos_home_theater_supported =
            configuration.IsSpatialAudioFormatSupported(atmos);
          const auto active = configuration.ActiveSpatialAudioFormat();
          route.active_spatial_format = canonical_guid(
            std::wstring_view {active.c_str(), active.size()});
          record_hresult(
            report,
            "route[" + std::string {bookend} + "].SpatialAudioDeviceConfiguration::Read",
            S_OK);
        } else {
          complete = false;
        }
      } catch (const winrt::hresult_error &error) {
        record_exception(
          report,
          "route[" + std::string {bookend} + "].SpatialAudioDeviceConfiguration",
          error);
        complete = false;
      }
    } else {
      complete = false;
    }

    route.sample_complete = complete;
    return route;
  }

  winrt::com_ptr<IMMDevice> get_default_render_device(run_report &report) {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    HRESULT hresult = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      enumerator.put_void());
    record_hresult(report, "CoCreateInstance[MMDeviceEnumerator,work]", hresult);
    if (hresult != S_OK) {
      return {};
    }
    winrt::com_ptr<IMMDevice> device;
    hresult = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
    record_hresult(report, "GetDefaultAudioEndpoint[console,work]", hresult);
    if (hresult != S_OK) {
      return {};
    }
    return device;
  }

  atmos_spatial_lock::mat10_observation observe_mat10(
    IMMDevice *device,
    run_report &report) {
    atmos_spatial_lock::mat10_observation observation;
    const auto mat10 = atmos_spatial_lock::make_mat10_format();
    const auto *wave = reinterpret_cast<const WAVEFORMATEX *>(&mat10);

    winrt::com_ptr<IAudioClient> support_client;
    HRESULT hresult = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      support_client.put_void());
    record_hresult(report, "MAT10.Activate[IsFormatSupported]", hresult);
    if (hresult == S_OK) {
      hresult = support_client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        wave,
        nullptr);
      observation.format_support_hresult = static_cast<hresult_code>(hresult);
      record_hresult(report, "MAT10.IsFormatSupported[exclusive]", hresult);
    }
    support_client = nullptr;

    winrt::com_ptr<IAudioClient> initialize_client;
    hresult = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      initialize_client.put_void());
    record_hresult(report, "MAT10.Activate[Initialize]", hresult);
    if (hresult == S_OK) {
      hresult = initialize_client->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_NOPERSIST,
        0,
        0,
        wave,
        nullptr);
      observation.initialize_hresult = static_cast<hresult_code>(hresult);
      record_hresult(report, "MAT10.Initialize[exclusive,fresh]", hresult);
    }
    initialize_client = nullptr;
    return observation;
  }

  bool is_float_object_format(const WAVEFORMATEX &format) {
    if (format.nChannels != 1 || format.wBitsPerSample != 32 ||
        format.nBlockAlign != sizeof(float) || format.nSamplesPerSec == 0 ||
        format.nAvgBytesPerSec != format.nSamplesPerSec * sizeof(float)) {
      return false;
    }
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      return true;
    }
    if (format.wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format.cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      return false;
    }
    const auto &extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
    return IsEqualGUID(extensible.SubFormat, k_float_subformat);
  }

  object_format observe_format(const WAVEFORMATEX &format) {
    object_format observation {
      .format_tag = format.wFormatTag,
      .channels = format.nChannels,
      .samples_per_second = format.nSamplesPerSec,
      .average_bytes_per_second = format.nAvgBytesPerSec,
      .block_align = format.nBlockAlign,
      .bits_per_sample = format.wBitsPerSample,
      .extra_size = format.cbSize,
      .float_pcm = is_float_object_format(format),
    };
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize == sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      const auto &extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
      observation.subformat = canonical_guid(extensible.SubFormat);
    } else if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      observation.subformat = canonical_guid(k_float_subformat);
    }
    return observation;
  }

  spatial_preparation observe_object_capabilities(
    IMMDevice *device,
    object_capabilities &capabilities,
    run_report &report) {
    spatial_preparation preparation;
    HRESULT hresult = device->Activate(
      IID_ISpatialAudioClient,
      CLSCTX_INPROC_SERVER,
      nullptr,
      preparation.client.put_void());
    capabilities.spatial_client_hresult = static_cast<hresult_code>(hresult);
    record_hresult(report, "IMMDevice::Activate[ISpatialAudioClient]", hresult);
    if (hresult != S_OK) {
      return preparation;
    }

    hresult = preparation.client->IsSpatialAudioStreamAvailable(
      IID_ISpatialAudioObjectRenderStream,
      nullptr);
    capabilities.stream_available_hresult = static_cast<hresult_code>(hresult);
    record_hresult(
      report,
      "ISpatialAudioClient::IsSpatialAudioStreamAvailable[ISpatialAudioObjectRenderStream]",
      hresult);

    AudioObjectType native_mask {AudioObjectType_None};
    hresult = preparation.client->GetNativeStaticObjectTypeMask(&native_mask);
    record_hresult(report, "ISpatialAudioClient::GetNativeStaticObjectTypeMask", hresult);
    if (hresult == S_OK) {
      capabilities.native_static_object_mask = static_cast<std::uint32_t>(native_mask);
    }

    UINT32 max_dynamic {};
    hresult = preparation.client->GetMaxDynamicObjectCount(&max_dynamic);
    record_hresult(report, "ISpatialAudioClient::GetMaxDynamicObjectCount", hresult);
    if (hresult == S_OK) {
      capabilities.max_dynamic_object_count = max_dynamic;
    }

    winrt::com_ptr<IAudioFormatEnumerator> format_enumerator;
    hresult = preparation.client->GetSupportedAudioObjectFormatEnumerator(
      format_enumerator.put());
    record_hresult(
      report,
      "ISpatialAudioClient::GetSupportedAudioObjectFormatEnumerator",
      hresult);
    if (hresult != S_OK) {
      return preparation;
    }

    UINT32 format_count {};
    hresult = format_enumerator->GetCount(&format_count);
    record_hresult(report, "IAudioFormatEnumerator::GetCount", hresult);
    if (hresult != S_OK) {
      return preparation;
    }
    capabilities.supported_format_count = format_count;

    for (UINT32 index = 0; index < format_count; ++index) {
      WAVEFORMATEX *raw_format {};
      hresult = format_enumerator->GetFormat(index, &raw_format);
      record_hresult(
        report,
        "IAudioFormatEnumerator::GetFormat[" + std::to_string(index) + "]",
        hresult);
      cotask_ptr<WAVEFORMATEX> owned_format {raw_format};
      if (hresult != S_OK || raw_format == nullptr ||
          !is_float_object_format(*raw_format)) {
        continue;
      }

      const std::size_t format_size = raw_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE ?
        sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX);
      std::memcpy(&preparation.format_storage, raw_format, format_size);
      preparation.format_size = format_size;
      capabilities.negotiated_format = observe_format(*preparation.format());
      break;
    }
    if (preparation.format_size == 0) {
      return preparation;
    }

    hresult = preparation.client->IsAudioObjectFormatSupported(preparation.format());
    capabilities.format_support_hresult = static_cast<hresult_code>(hresult);
    record_hresult(report, "ISpatialAudioClient::IsAudioObjectFormatSupported", hresult);

    UINT32 max_frames {};
    hresult = preparation.client->GetMaxFrameCount(
      preparation.format(),
      &max_frames);
    record_hresult(report, "ISpatialAudioClient::GetMaxFrameCount", hresult);
    if (hresult == S_OK) {
      capabilities.max_frame_count = max_frames;
    }
    return preparation;
  }

  bool activate_stream_and_objects(
    spatial_preparation &preparation,
    stream_bundle &bundle,
    run_report &report) {
    bundle.event = unique_handle {CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!bundle.event) {
      record_hresult(
        report,
        "CreateEventW[spatial-buffer]",
        HRESULT_FROM_WIN32(GetLastError()));
      append_diagnostic(report, diagnostic::stream_activation_failed);
      return false;
    }
    record_hresult(report, "CreateEventW[spatial-buffer]", S_OK);

    SpatialAudioObjectRenderStreamActivationParams stream_parameters {
      .ObjectFormat = preparation.format(),
      .StaticObjectTypeMask = static_cast<AudioObjectType>(
        atmos_spatial_lock::required_static_object_mask),
      .MinDynamicObjectCount = 1,
      .MaxDynamicObjectCount = 1,
      .Category = AudioCategory_SoundEffects,
      .EventHandle = bundle.event.get(),
      .NotifyObject = nullptr,
    };
    PROPVARIANT activation_parameters;
    PropVariantInit(&activation_parameters);
    activation_parameters.vt = VT_BLOB;
    activation_parameters.blob.cbSize = sizeof(stream_parameters);
    activation_parameters.blob.pBlobData =
      reinterpret_cast<BYTE *>(&stream_parameters);

    HRESULT hresult = preparation.client->ActivateSpatialAudioStream(
      &activation_parameters,
      IID_ISpatialAudioObjectRenderStream,
      bundle.stream.put_void());
    record_hresult(report, "ISpatialAudioClient::ActivateSpatialAudioStream", hresult);
    if (hresult != S_OK || !bundle.stream) {
      append_diagnostic(report, diagnostic::stream_activation_failed);
      return false;
    }

    UINT32 available_dynamic {};
    hresult = bundle.stream->GetAvailableDynamicObjectCount(&available_dynamic);
    record_hresult(
      report,
      "ISpatialAudioObjectRenderStream::GetAvailableDynamicObjectCount[pre-Start]",
      hresult);
    if (hresult != S_OK || available_dynamic < 1) {
      append_diagnostic(report, diagnostic::dynamic_object_activation_failed);
      return false;
    }

    bundle.objects.reserve(k_static_bed_objects.size() + 1);
    for (const auto type : k_static_bed_objects) {
      winrt::com_ptr<ISpatialAudioObject> object;
      hresult = bundle.stream->ActivateSpatialAudioObject(type, object.put());
      record_hresult(
        report,
        "ISpatialAudioObjectRenderStream::ActivateSpatialAudioObject[static," +
          std::to_string(static_cast<std::uint32_t>(type)) + "]",
        hresult);
      if (hresult != S_OK || !object) {
        append_diagnostic(report, diagnostic::static_object_activation_failed);
        return false;
      }
      bundle.objects.push_back(std::move(object));
    }

    winrt::com_ptr<ISpatialAudioObject> dynamic_object;
    hresult = bundle.stream->ActivateSpatialAudioObject(
      AudioObjectType_Dynamic,
      dynamic_object.put());
    record_hresult(
      report,
      "ISpatialAudioObjectRenderStream::ActivateSpatialAudioObject[dynamic]",
      hresult);
    if (hresult != S_OK || !dynamic_object) {
      append_diagnostic(report, diagnostic::dynamic_object_activation_failed);
      return false;
    }
    bundle.objects.push_back(std::move(dynamic_object));
    report.dynamic_object_activated = true;
    return true;
  }

  bool fill_object_buffer(
    ISpatialAudioObject *object,
    atmos_spatial_lock::detail::object_update_lifecycle &update_lifecycle,
    const UINT32 frame_count,
    const UINT32 frames_to_write,
    const std::uint64_t first_frame,
    const std::uint32_t samples_per_second,
    const atmos_spatial_lock::options &options,
    const std::uint32_t object_index,
    const std::uint32_t object_count,
    const bool final_buffer,
    run_report &report) {
    if (!update_lifecycle.accept(
          atmos_spatial_lock::detail::object_update_action::get_buffer)) {
      record_hresult(
        report,
        "ISpatialAudioObject::GetBuffer[update-order," +
          std::to_string(object_index) + "]",
        E_UNEXPECTED);
      return false;
    }
    BYTE *raw_buffer {};
    UINT32 buffer_bytes {};
    HRESULT hresult = object->GetBuffer(&raw_buffer, &buffer_bytes);
    if (hresult != S_OK) {
      record_hresult(
        report,
        "ISpatialAudioObject::GetBuffer[" + std::to_string(object_index) + "]",
        hresult);
      return false;
    }
    const auto expected_bytes = atmos_spatial_lock::checked_float_buffer_bytes(
      frame_count,
      frames_to_write);
    if (raw_buffer == nullptr || !expected_bytes ||
        buffer_bytes != *expected_bytes) {
      record_hresult(
        report,
        "ISpatialAudioObject::GetBuffer[size," + std::to_string(object_index) + "]",
        E_UNEXPECTED);
      return false;
    }

    std::span<float> samples {
      reinterpret_cast<float *>(raw_buffer),
      frame_count,
    };
    std::ranges::fill(samples, 0.0f);
    const float peak = atmos_spatial_lock::fill_scene_object(
      samples.first(frames_to_write),
      samples_per_second,
      options.frequency_hz,
      options.amplitude,
      first_frame,
      object_index,
      object_count);
    report.peak_sample = std::max(report.peak_sample, peak);
    report.audio_generated = report.audio_generated || peak > 0.0f;

    if (final_buffer) {
      if (!update_lifecycle.accept(
            atmos_spatial_lock::detail::object_update_action::set_end_of_stream)) {
        record_hresult(
          report,
          "ISpatialAudioObject::SetEndOfStream[update-order," +
            std::to_string(object_index) + "]",
          E_UNEXPECTED);
        return false;
      }
      hresult = object->SetEndOfStream(frames_to_write);
      record_hresult(
        report,
        "ISpatialAudioObject::SetEndOfStream[" + std::to_string(object_index) + "]",
        hresult);
      if (hresult != S_OK) {
        static_cast<void>(update_lifecycle.fail_update());
        return false;
      }
    }
    return true;
  }

  void render_scene(
    stream_bundle &bundle,
    const object_capabilities &capabilities,
    const atmos_spatial_lock::options &options,
    run_report &report) {
    if (!capabilities.negotiated_format || !capabilities.max_frame_count) {
      append_diagnostic(report, diagnostic::render_update_failed);
      return;
    }
    const std::uint32_t samples_per_second =
      capabilities.negotiated_format->samples_per_second;
    const std::uint64_t target_frames = static_cast<std::uint64_t>(std::llround(
      static_cast<double>(samples_per_second) * options.duration_ms / 1000.0));
    if (target_frames == 0) {
      append_diagnostic(report, diagnostic::render_update_failed);
      return;
    }

    stream_cleanup_context stream_cleanup {
      .stream = bundle.stream.get(),
      .report = &report,
      .samples_per_second = samples_per_second,
      .started_at = std::chrono::steady_clock::now(),
    };
    atmos_spatial_lock::detail::noexcept_cleanup_guard stream_guard {
      &stream_cleanup,
      stop_and_reset_stream,
    };
    HRESULT hresult = bundle.stream->Start();
    if (hresult == S_OK) {
      stream_guard.arm();
      report.started = true;
    }
    record_hresult(report, "ISpatialAudioObjectRenderStream::Start", hresult);
    if (hresult != S_OK) {
      append_diagnostic(report, diagnostic::stream_start_failed);
      return;
    }

    const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(options.duration_ms + 3000);
    atmos_spatial_lock::detail::object_update_lifecycle update_lifecycle;
    std::uint32_t consecutive_timeouts {};
    while (report.frame_count < target_frames) {
      if (std::chrono::steady_clock::now() >= deadline) {
        record_hresult(
          report,
          "WaitForSingleObject[overall-deadline]",
          HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        append_diagnostic(report, diagnostic::render_update_failed);
        break;
      }

      const DWORD wait_result = WaitForSingleObject(bundle.event.get(), 100);
      if (wait_result == WAIT_TIMEOUT) {
        ++consecutive_timeouts;
        if (consecutive_timeouts >= 10) {
          record_hresult(
            report,
            "WaitForSingleObject[spatial-buffer]",
            HRESULT_FROM_WIN32(ERROR_TIMEOUT));
          append_diagnostic(report, diagnostic::render_update_failed);
          break;
        }
        continue;
      }
      if (wait_result != WAIT_OBJECT_0) {
        const DWORD error = wait_result == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
        record_hresult(
          report,
          "WaitForSingleObject[spatial-buffer]",
          HRESULT_FROM_WIN32(error));
        append_diagnostic(report, diagnostic::render_update_failed);
        break;
      }
      consecutive_timeouts = 0;

      UINT32 available_dynamic {};
      UINT32 frame_count {};
      const bool first_update = report.update_count == 0;
      update_cleanup_context update_cleanup {
        .stream = bundle.stream.get(),
      };
      atmos_spatial_lock::detail::noexcept_cleanup_guard update_guard {
        &update_cleanup,
        end_audio_update,
      };
      hresult = bundle.stream->BeginUpdatingAudioObjects(
        &available_dynamic,
        &frame_count);
      if (hresult == S_OK) {
        update_guard.arm();
        if (!update_lifecycle.begin_update()) {
          record_hresult(
            report,
            "ISpatialAudioObjectRenderStream::BeginUpdatingAudioObjects[update-order]",
            E_UNEXPECTED);
          update_guard.invoke();
          record_hresult(
            report,
            "ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects[update-order]",
            update_cleanup.end_hresult);
          append_diagnostic(report, diagnostic::render_update_failed);
          break;
        }
      }
      if (first_update || hresult != S_OK) {
        record_hresult(
          report,
          "ISpatialAudioObjectRenderStream::BeginUpdatingAudioObjects",
          hresult);
      }
      if (hresult != S_OK) {
        append_diagnostic(report, diagnostic::render_update_failed);
        break;
      }
      if (frame_count == 0 || frame_count > *capabilities.max_frame_count) {
        if (!update_lifecycle.end_update()) {
          record_hresult(
            report,
            "ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects[update-order]",
            E_UNEXPECTED);
        }
        update_guard.invoke();
        record_hresult(
          report,
          "ISpatialAudioObjectRenderStream::BeginUpdatingAudioObjects[frame-count]",
          E_UNEXPECTED);
        record_hresult(
          report,
          "ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects[invalid-frame-count]",
          update_cleanup.end_hresult);
        append_diagnostic(report, diagnostic::render_update_failed);
        break;
      }

      const auto remaining = target_frames - report.frame_count;
      const UINT32 frames_to_write = static_cast<UINT32>(std::min<std::uint64_t>(
        frame_count,
        remaining));
      const bool final_buffer = frames_to_write == remaining;
      bool update_ok = true;

      const double progress = static_cast<double>(report.frame_count) /
                              static_cast<double>(target_frames);
      const float angle = static_cast<float>(progress * 2.0 * std::numbers::pi);
      if (!update_lifecycle.accept(
            atmos_spatial_lock::detail::object_update_action::set_position)) {
        record_hresult(
          report,
          "ISpatialAudioObject::SetPosition[dynamic,update-order]",
          E_UNEXPECTED);
        update_ok = false;
      } else {
        hresult = bundle.objects.back()->SetPosition(
          std::cos(angle),
          0.0f,
          std::sin(angle));
        if (first_update || hresult != S_OK) {
          record_hresult(
            report,
            first_update ? "ISpatialAudioObject::SetPosition[dynamic,initial]" :
                           "ISpatialAudioObject::SetPosition[dynamic]",
            hresult);
        }
        if (hresult != S_OK) {
          static_cast<void>(update_lifecycle.fail_update());
          update_ok = false;
        }
      }
      if (update_ok && first_update) {
        if (!update_lifecycle.accept(
              atmos_spatial_lock::detail::object_update_action::set_volume)) {
          record_hresult(
            report,
            "ISpatialAudioObject::SetVolume[dynamic,update-order]",
            E_UNEXPECTED);
          update_ok = false;
        } else {
          hresult = bundle.objects.back()->SetVolume(1.0f);
          record_hresult(report, "ISpatialAudioObject::SetVolume[dynamic]", hresult);
          if (hresult != S_OK) {
            static_cast<void>(update_lifecycle.fail_update());
            update_ok = false;
          }
        }
      }

      for (std::size_t index = 0;
           update_ok && index < bundle.objects.size();
           ++index) {
        if (!fill_object_buffer(
              bundle.objects[index].get(),
              update_lifecycle,
              frame_count,
              frames_to_write,
              report.frame_count,
              samples_per_second,
              options,
              static_cast<std::uint32_t>(index),
              static_cast<std::uint32_t>(bundle.objects.size()),
              final_buffer,
              report)) {
          update_ok = false;
          break;
        }
      }

      if (!update_lifecycle.end_update()) {
        record_hresult(
          report,
          "ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects[update-order]",
          E_UNEXPECTED);
        update_ok = false;
      }
      update_guard.invoke();
      const bool update_committed =
        update_ok && update_cleanup.end_hresult == S_OK;
      if (update_committed) {
        ++report.update_count;
        report.frame_count += frames_to_write;
      }
      if (first_update || update_cleanup.end_hresult != S_OK) {
        record_hresult(
          report,
          "ISpatialAudioObjectRenderStream::EndUpdatingAudioObjects",
          update_cleanup.end_hresult);
      }
      if (!update_committed) {
        append_diagnostic(report, diagnostic::render_update_failed);
        break;
      }
    }

    stream_guard.invoke();
    record_hresult(
      report,
      "ISpatialAudioObjectRenderStream::Stop",
      stream_cleanup.stop_hresult);
    if (!report.stopped) {
      append_diagnostic(report, diagnostic::stream_stop_failed);
    }
    record_hresult(
      report,
      "ISpatialAudioObjectRenderStream::Reset",
      stream_cleanup.reset_hresult);
    if (stream_cleanup.reset_hresult != S_OK) {
      append_diagnostic(report, diagnostic::render_update_failed);
    }
  }
}  // namespace

namespace atmos_spatial_lock {
  int run_windows(const options &parsed_options) {
    run_report report;
    report.requested_duration_ms = parsed_options.duration_ms;
    try {
      report.initial_route = observe_route(report, "initial");
      report.endpoint = report.initial_route.endpoint;

      auto device = get_default_render_device(report);
      preflight_observation preflight {
        .initial_route = report.initial_route,
        .pre_start_route = report.initial_route,
      };
      spatial_preparation preparation;
      if (device) {
        std::string work_endpoint_id;
        if (!get_device_id(
              device.get(),
              work_endpoint_id,
              report,
              "work_endpoint.GetId") ||
            work_endpoint_id != report.initial_route.endpoint.id) {
          append_diagnostic(report, diagnostic::route_not_stable);
        } else {
          preflight.mat10 = observe_mat10(device.get(), report);
          preparation = observe_object_capabilities(
            device.get(),
            preflight.objects,
            report);
        }
      }
      report.object_capabilities = preflight.objects;
      report.negotiated_format = preflight.objects.negotiated_format;

      // A second complete route sample must still match before any render stream
      // or object is activated.
      preflight.pre_start_route = observe_route(report, "preflight");
      auto gate = evaluate_preflight(preflight);
      for (const auto value : gate.diagnostics) {
        append_diagnostic(report, value);
      }

      stream_bundle bundle;
      if (gate.ready && report.diagnostics.empty() && activate_stream_and_objects(
                          preparation,
                          bundle,
                          report)) {
        // Object activation remains non-rendering. Re-sample the complete route
        // immediately before the sole intentional Start call.
        report.pre_start_route = observe_route(report, "pre_start");
        preflight.pre_start_route = report.pre_start_route;
        gate = evaluate_preflight(preflight);
        for (const auto value : gate.diagnostics) {
          append_diagnostic(report, value);
        }
        if (gate.ready) {
          render_scene(
            bundle,
            preflight.objects,
            parsed_options,
            report);
        }
      } else {
        report.pre_start_route = preflight.pre_start_route;
      }

      report.final_route = observe_route(report, "final");
      report.route_stable =
        routes_stable(report.initial_route, report.pre_start_route) &&
        routes_stable(report.pre_start_route, report.final_route);
      if (!report.final_route.sample_complete) {
        append_diagnostic(report, diagnostic::final_route_observation_failed);
      }
      if (!report.route_stable) {
        append_diagnostic(report, diagnostic::route_not_stable);
      }

      const std::uint64_t requested_frames = report.negotiated_format ?
        static_cast<std::uint64_t>(std::llround(
          static_cast<double>(report.negotiated_format->samples_per_second) *
          parsed_options.duration_ms / 1000.0)) : 0;
      report.ready =
        report.diagnostics.empty() &&
        report.started &&
        report.stopped &&
        report.route_stable &&
        report.audio_generated &&
        report.dynamic_object_activated &&
        requested_frames > 0 &&
        report.frame_count == requested_frames;
    } catch (const winrt::hresult_error &error) {
      record_unhandled_exception_noexcept(
        report,
        error.code(),
        "run_windows[unhandled_winrt_exception]");
    } catch (...) {
      record_unhandled_exception_noexcept(
        report,
        E_FAIL,
        "run_windows[unhandled_exception]");
    }
    return write_report(report);
  }
}  // namespace atmos_spatial_lock

#ifndef ATMOS_SPATIAL_LOCK_TEST_NO_MAIN
int main(const int argc, char *argv[]) {
  if (argc == 2 &&
      (std::string_view {argv[1]} == "--help" ||
       std::string_view {argv[1]} == "-h")) {
    std::cout << atmos_spatial_lock::help_text();
    return 0;
  }
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const auto parsed = atmos_spatial_lock::parse_options(arguments);
  if (!parsed.value) {
    std::cerr << parsed.error << '\n';
    return 64;
  }

  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (const winrt::hresult_error &error) {
    atmos_spatial_lock::run_report report;
    report.requested_duration_ms = parsed.value->duration_ms;
    record_unhandled_exception_noexcept(
      report,
      error.code(),
      "winrt::init_apartment");
    return write_report(report);
  }
  return atmos_spatial_lock::run_windows(*parsed.value);
}
#endif

#define INITGUID

#include "tools/atmos_capability_probe_windows.h"

#include "src/utility.h"

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <winrt/Windows.Media.Audio.h>

#include <array>
#include <limits>
#include <string>

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING

namespace {
  std::string wide_to_utf8(const std::wstring_view raw) {
    if (raw.empty()) {
      return {};
    }
    if (raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }

    const auto raw_size = static_cast<int>(raw.size());
    const int size = WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      raw.data(),
      raw_size,
      nullptr,
      0,
      nullptr,
      nullptr);
    if (size <= 0) {
      return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
      CP_UTF8,
      WC_ERR_INVALID_CHARS,
      raw.data(),
      raw_size,
      result.data(),
      size,
      nullptr,
      nullptr);
    return result;
  }

  std::optional<std::string> string_from_guid(const GUID &guid) {
    std::array<wchar_t, 39> text {};
    if (StringFromGUID2(guid, text.data(), static_cast<int>(text.size())) == 0) {
      return std::nullopt;
    }
    return wide_to_utf8(text.data());
  }

  std::optional<std::wstring> utf8_to_wide(const std::string_view raw) {
    if (raw.empty()) {
      return std::wstring {};
    }
    if (raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return std::nullopt;
    }

    const auto raw_size = static_cast<int>(raw.size());
    const int size = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      raw.data(),
      raw_size,
      nullptr,
      0);
    if (size <= 0) {
      return std::nullopt;
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      raw.data(),
      raw_size,
      result.data(),
      size);
    return result;
  }

  template<class T>
  void release_com(T *value) {
    value->Release();
  }

  template<class T>
  void co_task_free(T *value) {
    CoTaskMemFree(static_cast<void *>(value));
  }

  using device_enumerator_t = util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>>;
  using device_collection_t = util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>>;
  using device_t = util::safe_ptr<IMMDevice, release_com<IMMDevice>>;
  using property_store_t = util::safe_ptr<IPropertyStore, release_com<IPropertyStore>>;
  using audio_client_t = util::safe_ptr<IAudioClient, release_com<IAudioClient>>;
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;

  class prop_variant_t {
  public:
    prop_variant_t() {
      PropVariantInit(&value);
    }

    prop_variant_t(const prop_variant_t &) = delete;
    prop_variant_t &operator=(const prop_variant_t &) = delete;

    ~prop_variant_t() {
      PropVariantClear(&value);
    }

    PROPVARIANT value;
  };

  void append_error(
    atmos_probe::probe_observation &observation,
    std::string operation,
    const HRESULT hresult) {
    observation.errors.push_back(atmos_probe::api_error {
      .operation = std::move(operation),
      .hresult = static_cast<atmos_probe::hresult_code>(hresult),
    });
  }

  atmos_probe::endpoint_observation observe_endpoint(
    IMMDevice *device,
    atmos_probe::probe_observation &observation,
    const std::string_view context) {
    atmos_probe::endpoint_observation endpoint {};

    wstring_t endpoint_id;
    HRESULT hresult = device->GetId(&endpoint_id);
    if (SUCCEEDED(hresult) && endpoint_id) {
      endpoint.id = wide_to_utf8(endpoint_id.get());
    } else if (FAILED(hresult)) {
      append_error(observation, std::string {context} + ".GetId", hresult);
    }

    DWORD state {};
    hresult = device->GetState(&state);
    if (SUCCEEDED(hresult)) {
      endpoint.state = state;
    } else {
      append_error(observation, std::string {context} + ".GetState", hresult);
    }

    property_store_t properties;
    hresult = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(hresult)) {
      append_error(observation, std::string {context} + ".OpenPropertyStore", hresult);
      return endpoint;
    }

    prop_variant_t friendly_name;
    hresult = properties->GetValue(PKEY_Device_FriendlyName, &friendly_name.value);
    if (FAILED(hresult)) {
      append_error(observation, std::string {context} + ".PKEY_Device_FriendlyName", hresult);
    } else if (friendly_name.value.vt == VT_LPWSTR && friendly_name.value.pwszVal != nullptr) {
      endpoint.friendly_name = wide_to_utf8(friendly_name.value.pwszVal);
    }

    prop_variant_t form_factor;
    hresult = properties->GetValue(PKEY_AudioEndpoint_FormFactor, &form_factor.value);
    if (FAILED(hresult)) {
      append_error(observation, std::string {context} + ".PKEY_AudioEndpoint_FormFactor", hresult);
    }
    endpoint.form_factor = atmos_probe::decode_form_factor(form_factor.value);

    prop_variant_t jack_subtype;
    hresult = properties->GetValue(PKEY_AudioEndpoint_JackSubType, &jack_subtype.value);
    if (FAILED(hresult)) {
      append_error(observation, std::string {context} + ".PKEY_AudioEndpoint_JackSubType", hresult);
    }
    endpoint.jack_subtype = atmos_probe::decode_jack_subtype(jack_subtype.value);

    return endpoint;
  }

  void observe_active_endpoints(
    IMMDeviceEnumerator *enumerator,
    atmos_probe::probe_observation &observation) {
    device_collection_t endpoints;
    HRESULT hresult = enumerator->EnumAudioEndpoints(
      eRender,
      DEVICE_STATE_ACTIVE,
      &endpoints);
    if (FAILED(hresult)) {
      observation.probe_complete = false;
      append_error(observation, "IMMDeviceEnumerator::EnumAudioEndpoints", hresult);
      return;
    }

    UINT count {};
    hresult = endpoints->GetCount(&count);
    if (FAILED(hresult)) {
      observation.probe_complete = false;
      append_error(observation, "IMMDeviceCollection::GetCount", hresult);
      return;
    }

    observation.active_endpoints.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      device_t device;
      hresult = endpoints->Item(index, &device);
      if (FAILED(hresult)) {
        observation.probe_complete = false;
        append_error(
          observation,
          "IMMDeviceCollection::Item[" + std::to_string(index) + "]",
          hresult);
        continue;
      }
      observation.active_endpoints.push_back(observe_endpoint(
        device.get(),
        observation,
        "active_endpoints[" + std::to_string(index) + "]"));
    }
  }

  void observe_role_defaults(
    IMMDeviceEnumerator *enumerator,
    atmos_probe::probe_observation &observation) {
    struct role_spec {
      const char *name;
      ERole role;
    };
    constexpr std::array roles {
      role_spec {"console", eConsole},
      role_spec {"multimedia", eMultimedia},
      role_spec {"communications", eCommunications},
    };

    for (std::size_t index = 0; index < roles.size(); ++index) {
      observation.default_endpoints[index].role = roles[index].name;
      device_t device;
      const HRESULT hresult = enumerator->GetDefaultAudioEndpoint(
        eRender,
        roles[index].role,
        &device);
      if (hresult == E_NOTFOUND) {
        continue;
      }
      if (FAILED(hresult)) {
        append_error(
          observation,
          "IMMDeviceEnumerator::GetDefaultAudioEndpoint[" +
            std::string {roles[index].name} + "]",
          hresult);
        continue;
      }
      observation.default_endpoints[index].endpoint = observe_endpoint(
        device.get(),
        observation,
        "default_endpoints[" + std::string {roles[index].name} + "]");
    }
  }

  device_t select_endpoint(
    IMMDeviceEnumerator *enumerator,
    const atmos_probe::probe_options &options,
    atmos_probe::probe_observation &observation) {
    device_t device;
    HRESULT hresult {};
    if (options.endpoint_id) {
      const auto endpoint_id = utf8_to_wide(*options.endpoint_id);
      if (!endpoint_id) {
        append_error(observation, "IMMDeviceEnumerator::GetDevice", E_INVALIDARG);
        return device;
      }
      hresult = enumerator->GetDevice(endpoint_id->c_str(), &device);
    } else {
      hresult = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }

    if (hresult == E_NOTFOUND) {
      return device;
    }
    if (FAILED(hresult)) {
      append_error(
        observation,
        options.endpoint_id ?
          "IMMDeviceEnumerator::GetDevice" :
          "IMMDeviceEnumerator::GetDefaultAudioEndpoint[eConsole]",
        hresult);
      device.reset();
    }
    return device;
  }

  void observe_spatial_audio(
    const atmos_probe::endpoint_observation &endpoint,
    atmos_probe::probe_observation &observation) {
    using winrt::Windows::Media::Audio::SpatialAudioDeviceConfiguration;
    using winrt::Windows::Media::Audio::SpatialAudioFormatSubtype;

    SpatialAudioDeviceConfiguration configuration {nullptr};
    try {
      configuration = SpatialAudioDeviceConfiguration::GetForDeviceId(
        winrt::to_hstring(endpoint.id));
    } catch (const winrt::hresult_error &error) {
      append_error(
        observation,
        "SpatialAudioDeviceConfiguration::GetForDeviceId",
        error.code());
      return;
    }
    if (!configuration) {
      return;
    }
    observation.spatial.configuration_available = true;

    try {
      observation.spatial.spatial_audio_supported =
        configuration.IsSpatialAudioSupported();
    } catch (const winrt::hresult_error &error) {
      append_error(
        observation,
        "SpatialAudioDeviceConfiguration::IsSpatialAudioSupported",
        error.code());
    }

    try {
      const auto atmos_format = SpatialAudioFormatSubtype::DolbyAtmosForHomeTheater();
      observation.spatial.atmos_home_theater_supported =
        configuration.IsSpatialAudioFormatSupported(atmos_format);
    } catch (const winrt::hresult_error &error) {
      append_error(
        observation,
        "SpatialAudioDeviceConfiguration::IsSpatialAudioFormatSupported[DolbyAtmosForHomeTheater]",
        error.code());
    }

    try {
      const auto active = configuration.ActiveSpatialAudioFormat();
      observation.spatial.active_format_raw = winrt::to_string(active);
      observation.spatial.active_format_guid = atmos_probe::canonical_guid(
        std::wstring_view {active.c_str(), active.size()}).value_or("");
    } catch (const winrt::hresult_error &error) {
      append_error(
        observation,
        "SpatialAudioDeviceConfiguration::ActiveSpatialAudioFormat",
        error.code());
    }

    try {
      const auto default_format = configuration.DefaultSpatialAudioFormat();
      observation.spatial.default_format_raw = winrt::to_string(default_format);
      observation.spatial.default_format_guid = atmos_probe::canonical_guid(
        std::wstring_view {default_format.c_str(), default_format.size()}).value_or("");
    } catch (const winrt::hresult_error &error) {
      append_error(
        observation,
        "SpatialAudioDeviceConfiguration::DefaultSpatialAudioFormat",
        error.code());
    }
  }

  void observe_mat_profile(
    IMMDevice *device,
    const atmos_probe::mat_profile profile,
    atmos_probe::mat_observation &mat,
    atmos_probe::probe_observation &observation) {
    const std::string profile_name {atmos_probe::to_string(profile)};
    const auto format = atmos_probe::make_mat_format(profile);
    const auto *wave_format = reinterpret_cast<const WAVEFORMATEX *>(&format);

    audio_client_t support_client;
    HRESULT hresult = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      reinterpret_cast<void **>(&support_client));
    if (FAILED(hresult)) {
      append_error(
        observation,
        "IMMDevice::Activate[IAudioClient," + profile_name + ",IsFormatSupported]",
        hresult);
      return;
    }

    hresult = support_client->IsFormatSupported(
      AUDCLNT_SHAREMODE_EXCLUSIVE,
      wave_format,
      nullptr);
    mat.format_support_hresult = static_cast<atmos_probe::hresult_code>(hresult);
    support_client.reset();
    if (hresult != S_OK) {
      return;
    }

    audio_client_t initialize_client;
    hresult = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      reinterpret_cast<void **>(&initialize_client));
    if (FAILED(hresult)) {
      append_error(
        observation,
        "IMMDevice::Activate[IAudioClient," + profile_name + ",Initialize]",
        hresult);
      return;
    }

    hresult = initialize_client->Initialize(
      AUDCLNT_SHAREMODE_EXCLUSIVE,
      AUDCLNT_STREAMFLAGS_NOPERSIST,
      0,
      0,
      wave_format,
      nullptr);
    mat.initialize_hresult = static_cast<atmos_probe::hresult_code>(hresult);
    initialize_client.reset();
  }
}  // namespace

namespace atmos_probe {
  WAVEFORMATEXTENSIBLE_IEC61937 make_mat_format(const mat_profile profile) {
    WAVEFORMATEXTENSIBLE_IEC61937 format {};
    format.FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.FormatExt.Format.nChannels = 8;
    format.FormatExt.Format.nSamplesPerSec = 192000;
    format.FormatExt.Format.nAvgBytesPerSec = 3072000;
    format.FormatExt.Format.nBlockAlign = 16;
    format.FormatExt.Format.wBitsPerSample = 16;
    format.FormatExt.Format.cbSize = 34;
    format.FormatExt.Samples.wValidBitsPerSample = 16;
    format.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
    format.FormatExt.SubFormat = profile == mat_profile::mat21 ?
                                 k_iec61937_dolby_mat21 :
                                 k_iec61937_dolby_mat20;
    format.dwEncodedSamplesPerSec = 96000;
    format.dwEncodedChannelCount = 8;
    format.dwAverageBytesPerSec = 0;
    return format;
  }

  std::optional<std::string> canonical_guid(const std::wstring_view raw) {
    const std::wstring null_terminated {raw};
    GUID guid {};
    if (FAILED(CLSIDFromString(null_terminated.c_str(), &guid))) {
      return std::nullopt;
    }
    return string_from_guid(guid);
  }

  form_factor_observation decode_form_factor(const PROPVARIANT &value) {
    if (value.vt == VT_EMPTY) {
      return property_missing {};
    }
    if (value.vt != VT_UI4) {
      return property_wrong_type {.variant_type = value.vt};
    }
    if (value.ulVal == static_cast<ULONG>(DigitalAudioDisplayDevice)) {
      return display_audio_form_factor {};
    }
    return other_form_factor {.value = value.ulVal};
  }

  jack_subtype_observation decode_jack_subtype(const PROPVARIANT &value) {
    if (value.vt == VT_EMPTY) {
      return property_missing {};
    }
    if (value.vt != VT_LPWSTR) {
      return property_wrong_type {.variant_type = value.vt};
    }
    if (value.pwszVal == nullptr) {
      return malformed_connector_guid {};
    }

    const std::wstring_view raw {value.pwszVal};
    GUID guid {};
    if (FAILED(CLSIDFromString(value.pwszVal, &guid))) {
      return malformed_connector_guid {.raw = wide_to_utf8(raw)};
    }
    if (IsEqualGUID(guid, k_hdmi_interface)) {
      return hdmi_connector {};
    }
    if (IsEqualGUID(guid, k_displayport_interface)) {
      return displayport_connector {};
    }
    return other_connector_guid {.canonical_guid = *string_from_guid(guid)};
  }

  probe_observation collect_windows_observation(const probe_options &options) {
    probe_observation observation {};
    observation.default_endpoints = {
      role_endpoint_observation {.role = "console"},
      role_endpoint_observation {.role = "multimedia"},
      role_endpoint_observation {.role = "communications"},
    };

    device_enumerator_t enumerator;
    const HRESULT hresult = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      reinterpret_cast<void **>(&enumerator));
    if (FAILED(hresult)) {
      observation.probe_complete = false;
      append_error(
        observation,
        "CoCreateInstance[MMDeviceEnumerator]",
        hresult);
      return observation;
    }

    observe_active_endpoints(enumerator.get(), observation);
    observe_role_defaults(enumerator.get(), observation);

    auto selected_device = select_endpoint(enumerator.get(), options, observation);
    if (!selected_device) {
      return observation;
    }

    observation.selected_endpoint = observe_endpoint(
      selected_device.get(),
      observation,
      "selected_endpoint");
    observe_spatial_audio(*observation.selected_endpoint, observation);
    observe_mat_profile(
      selected_device.get(),
      mat_profile::mat21,
      observation.mat21,
      observation);
    observe_mat_profile(
      selected_device.get(),
      mat_profile::mat20,
      observation.mat20,
      observation);
    return observation;
  }
}  // namespace atmos_probe

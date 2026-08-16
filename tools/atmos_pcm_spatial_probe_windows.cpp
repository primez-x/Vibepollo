#define INITGUID

#include "tools/atmos_pcm_spatial_probe.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <spatialaudioclient.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);

namespace {
  using atmos_pcm_probe::api_error;
  using atmos_pcm_probe::endpoint_observation;
  using atmos_pcm_probe::format_details;
  using atmos_pcm_probe::hresult_code;
  using atmos_pcm_probe::probe_observation;
  using atmos_pcm_probe::probe_options;
  using atmos_pcm_probe::spatial_observation;
  using atmos_pcm_probe::supported_object_format_observation;

  template<class T>
  struct com_releaser {
    void operator()(T *value) const noexcept {
      if (value != nullptr) {
        value->Release();
      }
    }
  };

  template<class T>
  using com_ptr = std::unique_ptr<T, com_releaser<T>>;

  template<class T>
  struct task_mem_releaser {
    void operator()(T *value) const noexcept {
      CoTaskMemFree(static_cast<void *>(value));
    }
  };

  template<class T>
  using task_mem_ptr = std::unique_ptr<T, task_mem_releaser<T>>;

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

  struct role_spec {
    const char *name;
    ERole role;
  };

  constexpr std::array k_roles {
    role_spec {"console", eConsole},
    role_spec {"multimedia", eMultimedia},
    role_spec {"communications", eCommunications},
  };

  std::string wide_to_utf8(const std::wstring_view raw) {
    if (raw.empty() || raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
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
    if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          raw.data(),
          raw_size,
          result.data(),
          size,
          nullptr,
          nullptr) != size) {
      return {};
    }
    return result;
  }

  std::optional<std::wstring> utf8_to_wide(const std::string_view raw) {
    if (raw.empty() || raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return raw.empty() ? std::optional<std::wstring> {std::wstring {}} : std::nullopt;
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
    if (MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          raw.data(),
          raw_size,
          result.data(),
          size) != size) {
      return std::nullopt;
    }
    return result;
  }

  std::string guid_text(const GUID &guid) {
    std::array<wchar_t, 39> raw {};
    if (StringFromGUID2(guid, raw.data(), static_cast<int>(raw.size())) == 0) {
      return {};
    }
    return wide_to_utf8(raw.data());
  }

  void append_error(
    probe_observation &observation,
    std::string operation,
    const HRESULT hresult) {
    observation.probe_complete = false;
    observation.errors.push_back(api_error {
      .operation = std::move(operation),
      .hresult = static_cast<hresult_code>(hresult),
    });
  }

  bool is_expected_spatial_unavailable(const HRESULT hresult) {
    return hresult == E_NOINTERFACE || hresult == E_NOTIMPL;
  }

  std::optional<format_details> describe_format(const WAVEFORMATEX *format) {
    if (format == nullptr) {
      return std::nullopt;
    }
    format_details result {
      .format_tag = format->wFormatTag,
      .channels = format->nChannels,
      .samples_per_second = format->nSamplesPerSec,
      .average_bytes_per_second = format->nAvgBytesPerSec,
      .block_align = format->nBlockAlign,
      .bits_per_sample = format->wBitsPerSample,
      .extra_size = format->cbSize,
    };
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      const auto &extensible = *reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
      result.valid_bits_per_sample = extensible.Samples.wValidBitsPerSample;
      result.channel_mask = extensible.dwChannelMask;
      result.subformat = guid_text(extensible.SubFormat);
    } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      result.valid_bits_per_sample = format->wBitsPerSample;
      result.subformat = guid_text(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    } else if (format->wFormatTag == WAVE_FORMAT_PCM) {
      result.valid_bits_per_sample = format->wBitsPerSample;
      result.subformat = guid_text(KSDATAFORMAT_SUBTYPE_PCM);
    }
    return result;
  }

  std::string endpoint_context(const endpoint_observation &endpoint) {
    return "endpoint[" + endpoint.id + "]";
  }

  bool read_endpoint_id(IMMDevice *device, std::string &result, const std::string &operation,
                        probe_observation &observation) {
    LPWSTR raw_id {};
    const HRESULT hresult = device->GetId(&raw_id);
    task_mem_ptr<WCHAR> id {raw_id};
    if (FAILED(hresult) || raw_id == nullptr) {
      append_error(observation, operation, FAILED(hresult) ? hresult : E_FAIL);
      return false;
    }
    result = wide_to_utf8(raw_id);
    if (result.empty()) {
      append_error(observation, operation, E_FAIL);
      return false;
    }
    return true;
  }

  bool activate_audio_client(
    IMMDevice *device,
    com_ptr<IAudioClient> &client,
    const std::string &operation,
    probe_observation &observation) {
    IAudioClient *raw_client {};
    const HRESULT hresult = device->Activate(
      IID_IAudioClient,
      CLSCTX_INPROC_SERVER,
      nullptr,
      reinterpret_cast<void **>(&raw_client));
    client.reset(raw_client);
    if (FAILED(hresult) || raw_client == nullptr) {
      append_error(observation, operation, FAILED(hresult) ? hresult : E_FAIL);
      return false;
    }
    return true;
  }

  void observe_audio_client(
    IMMDevice *device,
    endpoint_observation &endpoint,
    probe_observation &observation) {
    const auto context = endpoint_context(endpoint);

    {
      com_ptr<IAudioClient> client;
      if (activate_audio_client(
            device,
            client,
            context + ".IAudioClient.Activate[mix]",
            observation)) {
        WAVEFORMATEX *raw_format {};
        const HRESULT hresult = client->GetMixFormat(&raw_format);
        task_mem_ptr<WAVEFORMATEX> mix_format {raw_format};
        endpoint.mix_format_hresult = static_cast<hresult_code>(hresult);
        if (hresult == S_OK && raw_format != nullptr) {
          endpoint.mix_format = describe_format(raw_format);
        } else if (FAILED(hresult) || raw_format == nullptr) {
          append_error(
            observation,
            context + ".IAudioClient.GetMixFormat",
            FAILED(hresult) ? hresult : E_FAIL);
        }
      }
    }

    const auto candidate = atmos_pcm_probe::make_exact_candidate_format();
    const auto *candidate_wave = reinterpret_cast<const WAVEFORMATEX *>(&candidate);
    {
      com_ptr<IAudioClient> client;
      if (activate_audio_client(
            device,
            client,
            context + ".IAudioClient.Activate[format-support]",
            observation)) {
        WAVEFORMATEX *raw_closest {};
        const HRESULT hresult = client->IsFormatSupported(
          AUDCLNT_SHAREMODE_SHARED,
          candidate_wave,
          &raw_closest);
        task_mem_ptr<WAVEFORMATEX> closest {raw_closest};
        endpoint.exact_candidate.format_support_hresult = static_cast<hresult_code>(hresult);
        if (hresult == S_FALSE && raw_closest == nullptr) {
          append_error(
            observation,
            context + ".IAudioClient.IsFormatSupported[closest]",
            E_POINTER);
        } else if (hresult == S_FALSE && raw_closest != nullptr) {
          endpoint.exact_candidate.closest_format = describe_format(raw_closest);
        }
      }
    }

    {
      com_ptr<IAudioClient> client;
      if (activate_audio_client(
            device,
            client,
            context + ".IAudioClient.Activate[loopback]",
            observation)) {
        const HRESULT hresult = client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
          0,
          0,
          candidate_wave,
          nullptr);
        endpoint.exact_candidate.loopback_initialize_hresult =
          static_cast<hresult_code>(hresult);
      }
    }
  }

  void observe_spatial_client(
    IMMDevice *device,
    endpoint_observation &endpoint,
    probe_observation &observation) {
    const auto context = endpoint_context(endpoint);
    com_ptr<ISpatialAudioClient> client;
    ISpatialAudioClient *raw_client {};
    const HRESULT activation_hresult = device->Activate(
      IID_ISpatialAudioClient,
      CLSCTX_INPROC_SERVER,
      nullptr,
      reinterpret_cast<void **>(&raw_client));
    client.reset(raw_client);
    endpoint.spatial.activation_hresult = static_cast<hresult_code>(activation_hresult);
    if (FAILED(activation_hresult) || raw_client == nullptr) {
      if (!is_expected_spatial_unavailable(activation_hresult)) {
        append_error(
          observation,
          context + ".IMMDevice.Activate[ISpatialAudioClient]",
          FAILED(activation_hresult) ? activation_hresult : E_FAIL);
      }
      return;
    }

    AudioObjectType native_mask {AudioObjectType_None};
    const HRESULT mask_hresult = client->GetNativeStaticObjectTypeMask(&native_mask);
    endpoint.spatial.native_static_object_mask_hresult = static_cast<hresult_code>(mask_hresult);
    if (mask_hresult == S_OK) {
      endpoint.spatial.native_static_object_mask = static_cast<std::uint32_t>(native_mask);
      endpoint.spatial.has_required_static_object_mask =
        (static_cast<std::uint32_t>(native_mask) & atmos_pcm_probe::required_static_object_mask) ==
        atmos_pcm_probe::required_static_object_mask;
    } else if (!is_expected_spatial_unavailable(mask_hresult)) {
      append_error(
        observation,
        context + ".ISpatialAudioClient.GetNativeStaticObjectTypeMask",
        mask_hresult);
    }

    UINT32 dynamic_count {};
    const HRESULT dynamic_hresult = client->GetMaxDynamicObjectCount(&dynamic_count);
    endpoint.spatial.max_dynamic_object_count_hresult = static_cast<hresult_code>(dynamic_hresult);
    if (dynamic_hresult == S_OK) {
      endpoint.spatial.max_dynamic_object_count = dynamic_count;
    } else if (!is_expected_spatial_unavailable(dynamic_hresult)) {
      append_error(
        observation,
        context + ".ISpatialAudioClient.GetMaxDynamicObjectCount",
        dynamic_hresult);
    }

    com_ptr<IAudioFormatEnumerator> formats;
    IAudioFormatEnumerator *raw_formats {};
    const HRESULT enumerator_hresult = client->GetSupportedAudioObjectFormatEnumerator(
      &raw_formats);
    formats.reset(raw_formats);
    endpoint.spatial.supported_format_enumerator_hresult =
      static_cast<hresult_code>(enumerator_hresult);
    if (FAILED(enumerator_hresult) || raw_formats == nullptr) {
      if (!is_expected_spatial_unavailable(enumerator_hresult)) {
        append_error(
          observation,
          context + ".ISpatialAudioClient.GetSupportedAudioObjectFormatEnumerator",
          FAILED(enumerator_hresult) ? enumerator_hresult : E_FAIL);
      }
      return;
    }

    UINT32 format_count {};
    const HRESULT count_hresult = formats->GetCount(&format_count);
    endpoint.spatial.supported_format_count_hresult = static_cast<hresult_code>(count_hresult);
    if (FAILED(count_hresult)) {
      append_error(
        observation,
        context + ".IAudioFormatEnumerator.GetCount",
        count_hresult);
      return;
    }
    endpoint.spatial.supported_format_count = format_count;
    endpoint.spatial.supported_formats.reserve(format_count);
    for (UINT32 index = 0; index < format_count; ++index) {
      WAVEFORMATEX *raw_format {};
      const HRESULT format_hresult = formats->GetFormat(index, &raw_format);
      task_mem_ptr<WAVEFORMATEX> format {raw_format};
      supported_object_format_observation format_observation {
        .get_format_hresult = static_cast<hresult_code>(format_hresult),
      };
      if (format_hresult == S_OK && raw_format != nullptr) {
        format_observation.format = describe_format(raw_format);
        if (atmos_pcm_probe::is_exact_candidate_format(raw_format)) {
          endpoint.spatial.has_exact_static_bed_format = true;
        }
      } else if (FAILED(format_hresult) || raw_format == nullptr) {
        append_error(
          observation,
          context + ".IAudioFormatEnumerator.GetFormat[" + std::to_string(index) + "]",
          FAILED(format_hresult) ? format_hresult : E_FAIL);
      }
      endpoint.spatial.supported_formats.push_back(std::move(format_observation));
    }
  }

  endpoint_observation observe_endpoint(
    IMMDevice *device,
    probe_observation &observation,
    const std::array<std::string, 3> &default_ids) {
    endpoint_observation endpoint {};
    if (!read_endpoint_id(device, endpoint.id, "IMMDevice.GetId", observation)) {
      return endpoint;
    }

    const auto context = endpoint_context(endpoint);
    DWORD state {};
    const HRESULT state_hresult = device->GetState(&state);
    if (FAILED(state_hresult)) {
      append_error(observation, context + ".IMMDevice.GetState", state_hresult);
    } else {
      endpoint.state = state;
    }
    endpoint.console_default = endpoint.id == default_ids[0];
    endpoint.multimedia_default = endpoint.id == default_ids[1];
    endpoint.communications_default = endpoint.id == default_ids[2];

    com_ptr<IPropertyStore> properties;
    IPropertyStore *raw_properties {};
    const HRESULT property_hresult = device->OpenPropertyStore(
      STGM_READ,
      &raw_properties);
    properties.reset(raw_properties);
    if (FAILED(property_hresult) || raw_properties == nullptr) {
      append_error(
        observation,
        context + ".IMMDevice.OpenPropertyStore",
        FAILED(property_hresult) ? property_hresult : E_FAIL);
      return endpoint;
    }
    prop_variant friendly_name;
    const HRESULT friendly_hresult = properties->GetValue(
      PKEY_Device_FriendlyName,
      &friendly_name.value);
    if (friendly_hresult == S_OK && friendly_name.value.vt == VT_LPWSTR &&
        friendly_name.value.pwszVal != nullptr) {
      endpoint.friendly_name = wide_to_utf8(friendly_name.value.pwszVal);
    } else {
      append_error(
        observation,
        context + ".IPropertyStore.PKEY_Device_FriendlyName",
        FAILED(friendly_hresult) ? friendly_hresult : E_INVALIDARG);
    }

    observe_audio_client(device, endpoint, observation);
    observe_spatial_client(device, endpoint, observation);
    return endpoint;
  }

  bool read_default_ids(
    IMMDeviceEnumerator *enumerator,
    std::array<std::string, 3> &default_ids,
    probe_observation &observation) {
    bool complete = true;
    for (std::size_t index = 0; index < k_roles.size(); ++index) {
      com_ptr<IMMDevice> device;
      IMMDevice *raw_device {};
      const HRESULT hresult = enumerator->GetDefaultAudioEndpoint(
        eRender,
        k_roles[index].role,
        &raw_device);
      device.reset(raw_device);
      if (hresult == E_NOTFOUND) {
        continue;
      }
      if (FAILED(hresult) || raw_device == nullptr) {
        append_error(
          observation,
          std::string {"IMMDeviceEnumerator.GetDefaultAudioEndpoint["} +
            k_roles[index].name + "]",
          FAILED(hresult) ? hresult : E_FAIL);
        complete = false;
        continue;
      }
      if (!read_endpoint_id(
            raw_device,
            default_ids[index],
            std::string {"IMMDevice.GetId[default-"} + k_roles[index].name + "]",
            observation)) {
        complete = false;
      }
    }
    return complete;
  }

  bool collect_active_endpoints(
    IMMDeviceEnumerator *enumerator,
    const std::array<std::string, 3> &default_ids,
    probe_observation &observation) {
    com_ptr<IMMDeviceCollection> collection;
    IMMDeviceCollection *raw_collection {};
    const HRESULT enum_hresult = enumerator->EnumAudioEndpoints(
      eRender,
      DEVICE_STATE_ACTIVE,
      &raw_collection);
    collection.reset(raw_collection);
    if (FAILED(enum_hresult) || raw_collection == nullptr) {
      append_error(
        observation,
        "IMMDeviceEnumerator.EnumAudioEndpoints",
        FAILED(enum_hresult) ? enum_hresult : E_FAIL);
      return false;
    }
    UINT count {};
    const HRESULT count_hresult = collection->GetCount(&count);
    if (FAILED(count_hresult)) {
      append_error(observation, "IMMDeviceCollection.GetCount", count_hresult);
      return false;
    }
    observation.endpoints.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      com_ptr<IMMDevice> device;
      IMMDevice *raw_device {};
      const HRESULT item_hresult = collection->Item(index, &raw_device);
      device.reset(raw_device);
      if (FAILED(item_hresult) || raw_device == nullptr) {
        append_error(
          observation,
          "IMMDeviceCollection.Item[" + std::to_string(index) + "]",
          FAILED(item_hresult) ? item_hresult : E_FAIL);
        continue;
      }
      observation.endpoints.push_back(
        observe_endpoint(raw_device, observation, default_ids));
    }
    std::sort(
      observation.endpoints.begin(),
      observation.endpoints.end(),
      [](const endpoint_observation &left, const endpoint_observation &right) {
        return left.id < right.id;
      });
    return true;
  }

  bool collect_explicit_endpoint(
    IMMDeviceEnumerator *enumerator,
    const probe_options &options,
    const std::array<std::string, 3> &default_ids,
    probe_observation &observation) {
    const auto endpoint_id = utf8_to_wide(*options.endpoint_id);
    if (!endpoint_id) {
      append_error(observation, "IMMDeviceEnumerator.GetDevice[UTF8]", E_INVALIDARG);
      return false;
    }
    com_ptr<IMMDevice> device;
    IMMDevice *raw_device {};
    const HRESULT hresult = enumerator->GetDevice(endpoint_id->c_str(), &raw_device);
    device.reset(raw_device);
    if (FAILED(hresult) || raw_device == nullptr) {
      append_error(
        observation,
        "IMMDeviceEnumerator.GetDevice",
        FAILED(hresult) ? hresult : E_FAIL);
      return false;
    }
    observation.endpoints.push_back(
      observe_endpoint(raw_device, observation, default_ids));
    return true;
  }
}  // namespace

namespace atmos_pcm_probe {
  probe_observation collect_windows_observation(const probe_options &options) {
    probe_observation observation {};
    const HRESULT apartment_hresult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment_hresult)) {
      append_error(observation, "CoInitializeEx", apartment_hresult);
      return observation;
    }
    struct apartment_cleanup {
      ~apartment_cleanup() {
        CoUninitialize();
      }
    } cleanup;

    com_ptr<IMMDeviceEnumerator> enumerator;
    IMMDeviceEnumerator *raw_enumerator {};
    const HRESULT create_hresult = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_INPROC_SERVER,
      IID_IMMDeviceEnumerator,
      reinterpret_cast<void **>(&raw_enumerator));
    enumerator.reset(raw_enumerator);
    if (FAILED(create_hresult) || raw_enumerator == nullptr) {
      append_error(
        observation,
        "CoCreateInstance[MMDeviceEnumerator]",
        FAILED(create_hresult) ? create_hresult : E_FAIL);
      return observation;
    }

    std::array<std::string, 3> default_ids {};
    read_default_ids(enumerator.get(), default_ids, observation);
    if (options.endpoint_id) {
      collect_explicit_endpoint(
        enumerator.get(),
        options,
        default_ids,
        observation);
    } else {
      collect_active_endpoints(
        enumerator.get(),
        default_ids,
        observation);
    }
    return observation;
  }
}  // namespace atmos_pcm_probe

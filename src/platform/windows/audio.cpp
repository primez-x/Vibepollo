/**
 * @file src/platform/windows/audio.cpp
 * @brief Definitions for Windows audio capture.
 */
#define INITGUID

// standard includes
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

// platform includes
#include <WinSock2.h>
#include <Audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <newdev.h>
#include <roapi.h>
#include <synchapi.h>

// local includes
#include "src/audio_policy.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/audio_policy_process.h"
#include "utf_utils.h"

DEFINE_PROPERTYKEY(PKEY_Device_DeviceDesc, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 2);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);  // DEVPROP_TYPE_STRING
DEFINE_PROPERTYKEY(PKEY_DeviceInterface_FriendlyName, 0x026e516e, 0xb814, 0x414b, 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22, 2);

#if defined(__x86_64) || defined(__x86_64__) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64)
  #define STEAM_DRIVER_SUBDIR L"x64"
#endif

namespace {

  constexpr auto SAMPLE_RATE = 48000;
#ifdef STEAM_DRIVER_SUBDIR
  constexpr auto STEAM_AUDIO_DRIVER_PATH = L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\" STEAM_DRIVER_SUBDIR L"\\SteamStreamingSpeakers.inf";
#endif

  constexpr auto waveformat_mask_stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

  constexpr auto waveformat_mask_surround51_with_backspeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;

  constexpr auto waveformat_mask_surround51_with_sidespeakers = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                                SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                                                SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  constexpr auto waveformat_mask_surround71 = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                              SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
                                              SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

  enum class sample_format_e {
    f32,
    s32,
    s24in32,
    s24,
    s16,
    _size,
  };

  constexpr WAVEFORMATEXTENSIBLE create_waveformat(sample_format_e sample_format, WORD channel_count, DWORD channel_mask) {
    WAVEFORMATEXTENSIBLE waveformat = {};

    switch (sample_format) {
      default:
      case sample_format_e::f32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 32;
        break;

      case sample_format_e::s24in32:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 32;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s24:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 24;
        waveformat.Samples.wValidBitsPerSample = 24;
        break;

      case sample_format_e::s16:
        waveformat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
        waveformat.Format.wBitsPerSample = 16;
        waveformat.Samples.wValidBitsPerSample = 16;
        break;
    }

    static_assert((int) sample_format_e::_size == 5, "Unrecognized sample_format_e");

    waveformat.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    waveformat.Format.nChannels = channel_count;
    waveformat.Format.nSamplesPerSec = SAMPLE_RATE;

    waveformat.Format.nBlockAlign = waveformat.Format.nChannels * waveformat.Format.wBitsPerSample / 8;
    waveformat.Format.nAvgBytesPerSec = waveformat.Format.nSamplesPerSec * waveformat.Format.nBlockAlign;
    waveformat.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);

    waveformat.dwChannelMask = channel_mask;

    return waveformat;
  }

  using virtual_sink_waveformats_t = std::vector<WAVEFORMATEXTENSIBLE>;

  /**
   * @brief List of supported waveformats for an N-channel virtual audio device
   * @tparam channel_count Number of virtual audio channels
   * @returns std::vector<WAVEFORMATEXTENSIBLE>
   * @note The list of virtual formats returned are sorted in preference order and the first valid
   *       format will be used. All bits-per-sample options are listed because we try to match
   *       this to the default audio device. See also: set_format() below.
   */
  template<WORD channel_count>
  virtual_sink_waveformats_t create_virtual_sink_waveformats() {
    if constexpr (channel_count == 2) {
      auto channel_mask = waveformat_mask_stereo;
      // The 32-bit formats are a lower priority for stereo because using one will disable Dolby/DTS
      // spatial audio mode if the user enabled it on the Steam speaker.
      return {
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
      };
    } else if (channel_count == 6) {
      auto channel_mask1 = waveformat_mask_surround51_with_backspeakers;
      auto channel_mask2 = waveformat_mask_surround51_with_sidespeakers;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::f32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask2),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask1),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask2),
      };
    } else if (channel_count == 8) {
      auto channel_mask = waveformat_mask_surround71;
      return {
        create_waveformat(sample_format_e::f32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24in32, channel_count, channel_mask),
        create_waveformat(sample_format_e::s24, channel_count, channel_mask),
        create_waveformat(sample_format_e::s16, channel_count, channel_mask),
      };
    }
  }

  std::string waveformat_to_pretty_string(const WAVEFORMATEXTENSIBLE &waveformat) {
    std::string result = waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT ? "F" :
                         waveformat.SubFormat == KSDATAFORMAT_SUBTYPE_PCM        ? "S" :
                                                                                   "UNKNOWN";

    result += std::format("{} {} ", static_cast<int>(waveformat.Samples.wValidBitsPerSample), static_cast<int>(waveformat.Format.nSamplesPerSec));

    switch (waveformat.dwChannelMask) {
      case waveformat_mask_stereo:
        result += "2.0";
        break;

      case waveformat_mask_surround51_with_backspeakers:
        result += "5.1";
        break;

      case waveformat_mask_surround51_with_sidespeakers:
        result += "5.1 (sidespeakers)";
        break;

      case waveformat_mask_surround71:
        result += "7.1";
        break;

      default:
        result += std::format("{} channels (unrecognized)", static_cast<int>(waveformat.Format.nChannels));
        break;
    }

    return result;
  }

  ::platf::audio_policy::wave_format_extensible_t policy_process_wave_format(
    const WAVEFORMATEXTENSIBLE &waveformat
  ) {
    ::platf::audio_policy::wave_format_extensible_t result {};
    result.format_tag = waveformat.Format.wFormatTag;
    result.channels = waveformat.Format.nChannels;
    result.samples_per_sec = waveformat.Format.nSamplesPerSec;
    result.avg_bytes_per_sec = waveformat.Format.nAvgBytesPerSec;
    result.block_align = waveformat.Format.nBlockAlign;
    result.bits_per_sample = waveformat.Format.wBitsPerSample;
    result.cb_size = waveformat.Format.cbSize;
    result.valid_bits_per_sample = waveformat.Samples.wValidBitsPerSample;
    result.channel_mask = waveformat.dwChannelMask;
    std::memcpy(
      result.sub_format.data(),
      &waveformat.SubFormat,
      result.sub_format.size()
    );
    return result;
  }

}  // namespace

using namespace std::literals;

namespace platf::audio {
  template<class T>
  void Release(T *p) {
    p->Release();
  }

  template<class T>
  void co_task_free(T *p) {
    CoTaskMemFree((LPVOID) p);
  }

  using device_enum_t = util::safe_ptr<IMMDeviceEnumerator, Release<IMMDeviceEnumerator>>;
  using device_t = util::safe_ptr<IMMDevice, Release<IMMDevice>>;
  using collection_t = util::safe_ptr<IMMDeviceCollection, Release<IMMDeviceCollection>>;
  using audio_client_t = util::safe_ptr<IAudioClient, Release<IAudioClient>>;
  using audio_capture_t = util::safe_ptr<IAudioCaptureClient, Release<IAudioCaptureClient>>;
  using wave_format_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;
  using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
  using handle_t = util::safe_ptr_v2<void, BOOL, CloseHandle>;
  using prop_t = util::safe_ptr<IPropertyStore, Release<IPropertyStore>>;

  static void activate_policy_role_lane_runtime();
  static void shutdown_policy_role_lane_runtime();
  static void restart_policy_background_work(
    const std::shared_ptr<::audio::policy::fixed_role_lane_coordinator_t> &runtime,
    std::uint64_t generation
  );

  class co_init_t: public deinit_t {
  public:
    explicit co_init_t(bool owns_policy_role_lanes = false):
        owns_policy_role_lanes_ {owns_policy_role_lanes},
        status_ {CoInitializeEx(
      nullptr,
      COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY
    )} {
      if (owns_policy_role_lanes_) {
        activate_policy_role_lane_runtime();
      }
    }

    ~co_init_t() override {
      if (owns_policy_role_lanes_) {
        shutdown_policy_role_lane_runtime();
      }
      if (SUCCEEDED(status_)) {
        CoUninitialize();
      }
    }

    co_init_t(const co_init_t &) = delete;
    co_init_t &operator=(const co_init_t &) = delete;

  private:
    bool owns_policy_role_lanes_;
    HRESULT status_;
  };

  class prop_var_t {
  public:
    prop_var_t() {
      PropVariantInit(&prop);
    }

    ~prop_var_t() {
      PropVariantClear(&prop);
    }

    PROPVARIANT prop;
  };

  struct format_t {
    WORD channel_count;
    std::string name;
    int capture_waveformat_channel_mask;
    virtual_sink_waveformats_t virtual_sink_waveformats;
  };

  const std::array<const format_t, 3> formats = {
    format_t {
      2,
      "Stereo",
      waveformat_mask_stereo,
      create_virtual_sink_waveformats<2>(),
    },
    format_t {
      6,
      "Surround 5.1",
      waveformat_mask_surround51_with_backspeakers,
      create_virtual_sink_waveformats<6>(),
    },
    format_t {
      8,
      "Surround 7.1",
      waveformat_mask_surround71,
      create_virtual_sink_waveformats<8>(),
    },
  };

  audio_client_t make_audio_client(device_t &device, const format_t &format) {
    audio_client_t audio_client;
    auto status = device->Activate(
      IID_IAudioClient,
      CLSCTX_ALL,
      nullptr,
      (void **) &audio_client
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't activate Device: [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    WAVEFORMATEXTENSIBLE capture_waveformat =
      create_waveformat(sample_format_e::f32, format.channel_count, format.capture_waveformat_channel_mask);

    {
      wave_format_t mixer_waveformat;
      status = audio_client->GetMixFormat(&mixer_waveformat);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't get mix format for audio device: [0x"sv << util::hex(status).to_string_view() << ']';
        return nullptr;
      }

      // Prefer the native channel layout of captured audio device when channel counts match
      if (mixer_waveformat->nChannels == format.channel_count &&
          mixer_waveformat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
          mixer_waveformat->cbSize >= 22) {
        auto waveformatext_pointer = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mixer_waveformat.get());
        capture_waveformat.dwChannelMask = waveformatext_pointer->dwChannelMask;
      }

      BOOST_LOG(info) << "Audio mixer format is "sv << mixer_waveformat->wBitsPerSample << "-bit, "sv
                      << mixer_waveformat->nSamplesPerSec << " Hz, "sv
                      << ((mixer_waveformat->nSamplesPerSec != 48000) ? "will be resampled to 48000 by Windows"sv : "no resampling needed"sv);
    }

    status = audio_client->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,  // Enable automatic resampling to 48 KHz
      0,
      0,
      (LPWAVEFORMATEX) &capture_waveformat,
      nullptr
    );

    if (status) {
      BOOST_LOG(error) << "Couldn't initialize audio client for ["sv << format.name << "]: [0x"sv << util::hex(status).to_string_view() << ']';
      return nullptr;
    }

    BOOST_LOG(info) << "Audio capture format is "sv << logging::bracket(waveformat_to_pretty_string(capture_waveformat));

    return audio_client;
  }

  device_t default_device(device_enum_t &device_enum, ERole role = eConsole) {
    device_t device;
    HRESULT status;
    status = device_enum->GetDefaultAudioEndpoint(
      eRender,
      role,
      &device
    );

    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't get default audio endpoint [0x"sv << util::hex(status).to_string_view() << ']';

      return nullptr;
    }

    return device;
  }

  using role_device_ids_t = std::array<std::wstring, static_cast<std::size_t>(ERole_enum_count)>;

  enum class default_endpoint_read_state_e {
    known,
    no_default,
    unknown,
  };

  struct default_endpoint_read_t {
    default_endpoint_read_state_e state = default_endpoint_read_state_e::unknown;
    std::wstring id;
    HRESULT status = E_UNEXPECTED;

    bool is_known() const {
      return state == default_endpoint_read_state_e::known && !id.empty();
    }
  };

  static ::platf::audio_policy::render_role_e policy_process_role(ERole role) {
    switch (role) {
      case eMultimedia:
        return ::platf::audio_policy::render_role_e::multimedia;
      case eCommunications:
        return ::platf::audio_policy::render_role_e::communications;
      case eConsole:
      default:
        return ::platf::audio_policy::render_role_e::console;
    }
  }

  static default_endpoint_read_t policy_process_readback(
    const ::platf::audio_policy::result_t &result
  ) {
    if (result.process_reaped &&
        (result.stage == ::platf::audio_policy::failure_stage_e::success ||
         result.stage == ::platf::audio_policy::failure_stage_e::precondition) &&
        SUCCEEDED(result.read_hresult) &&
        !result.readback_id.empty()) {
      return {
        default_endpoint_read_state_e::known,
        result.readback_id,
        result.read_hresult,
      };
    }
    if (result.process_reaped &&
        (result.stage == ::platf::audio_policy::failure_stage_e::read ||
         result.stage == ::platf::audio_policy::failure_stage_e::success) &&
        result.read_hresult == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
      return {
        default_endpoint_read_state_e::no_default,
        {},
        result.read_hresult,
      };
    }
    return {
      default_endpoint_read_state_e::unknown,
      {},
      result.read_hresult,
    };
  }

  static HRESULT policy_process_set_status(
    const ::platf::audio_policy::result_t &result
  ) {
    if (!result.process_reaped) {
      return E_UNEXPECTED;
    }
    if (result.stage == ::platf::audio_policy::failure_stage_e::cancelled) {
      return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    if (result.stage == ::platf::audio_policy::failure_stage_e::timeout) {
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (result.stage == ::platf::audio_policy::failure_stage_e::com) {
      return result.com_hresult;
    }
    if (result.stage == ::platf::audio_policy::failure_stage_e::set ||
        result.stage == ::platf::audio_policy::failure_stage_e::precondition ||
        result.stage == ::platf::audio_policy::failure_stage_e::success) {
      return result.set_hresult;
    }
    return HRESULT_FROM_WIN32(
      result.win32_error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : result.win32_error
    );
  }

  using role_device_id_reads_t =
    std::array<default_endpoint_read_t, static_cast<std::size_t>(ERole_enum_count)>;

  struct pending_role_restore_t {
    ERole role;
    std::wstring preferred_id;
    std::wstring expected_current_id;
    std::wstring inflight_target_id;
    std::wstring failed_preferred_target_id;
    bool inflight_is_fallback = false;
    bool ownership_unconfirmed = false;
    std::uint64_t inflight_write_receipt_id = 0;
  };

  using pending_role_restores_t = std::vector<pending_role_restore_t>;

  struct pending_role_restore_handoff_t {
    std::wstring steam_device_id;
    pending_role_restores_t role_restores;
    std::uint64_t assignment_epoch = 0;
  };

  constexpr std::size_t role_index(ERole role) {
    return static_cast<std::size_t>(role);
  }

  static ::audio::policy::fixed_role_lane_runtime_t &
  policy_role_lane_runtime_manager_ref() {
    static ::audio::policy::fixed_role_lane_runtime_t runtime {
      {},
      [](
        const std::shared_ptr<::audio::policy::fixed_role_lane_coordinator_t> &created,
        std::uint64_t generation
      ) {
        restart_policy_background_work(created, generation);
      },
    };
    return runtime;
  }

  static void activate_policy_role_lane_runtime() {
    auto &manager = policy_role_lane_runtime_manager_ref();
    manager.activate();
    // Creation can fail transiently. A later get() retries the factory, and
    // the generation callback replays every retained observer/restore record.
    (void) manager.get();
  }

  static std::shared_ptr<::audio::policy::fixed_role_lane_coordinator_t>
  policy_role_lane_runtime(
    bool *created = nullptr,
    std::uint64_t *runtime_generation = nullptr
  ) {
    return policy_role_lane_runtime_manager_ref().get(
      created,
      runtime_generation
    );
  }

  static void shutdown_policy_role_lane_runtime() {
    policy_role_lane_runtime_manager_ref().release();
  }

  static ::platf::audio_policy::result_t cancelled_policy_process_result() {
    ::platf::audio_policy::result_t result;
    result.stage = ::platf::audio_policy::failure_stage_e::cancelled;
    result.com_hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result.set_hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result.format_hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result.read_hresult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result.process_exit_code = ERROR_CANCELLED;
    result.process_reaped = true;
    result.execution_disposition =
      ::platf::audio_policy::execution_disposition_e::not_started;
    return result;
  }

  static ::platf::audio_policy::result_t internal_policy_process_result() {
    ::platf::audio_policy::result_t result;
    result.stage = ::platf::audio_policy::failure_stage_e::protocol;
    result.com_hresult = E_UNEXPECTED;
    result.set_hresult = E_UNEXPECTED;
    result.format_hresult = E_UNEXPECTED;
    result.read_hresult = E_UNEXPECTED;
    result.process_exit_code = ERROR_UNHANDLED_EXCEPTION;
    result.win32_error = ERROR_UNHANDLED_EXCEPTION;
    // An exception may have crossed the supervisor while a child was live.
    // Keep receipts unresolved rather than claiming that process was reaped.
    result.process_reaped = false;
    result.execution_disposition =
      ::platf::audio_policy::execution_disposition_e::child_resumed_may_have_executed;
    return result;
  }

  static ::platf::audio_policy::result_t invoke_policy_process_on_role_lane(
    ::platf::audio_policy::request_t request,
    std::function<bool()> may_execute = {}
  ) {
    const auto index = static_cast<std::size_t>(request.role);
    auto runtime = policy_role_lane_runtime();
    if (!runtime) {
      return cancelled_policy_process_result();
    }
    if (runtime->is_current_lane(index)) {
      try {
        if (may_execute && !may_execute()) {
          return cancelled_policy_process_result();
        }
        return ::platf::audio_policy::invoke(request);
      } catch (...) {
        return internal_policy_process_result();
      }
    }

    return runtime->invoke_foreground<::platf::audio_policy::result_t>(
      index,
      [request = std::move(request),
       may_execute = std::move(may_execute)](
        std::stop_token lane_stop_token
      ) mutable {
        if (may_execute && !may_execute()) {
          return cancelled_policy_process_result();
        }
        std::stop_source combined_stop;
        std::stop_callback lane_stop_callback(
          lane_stop_token,
          [&combined_stop]() { combined_stop.request_stop(); }
        );
        std::stop_callback request_stop_callback(
          request.stop_token,
          [&combined_stop]() { combined_stop.request_stop(); }
        );
        request.stop_token = combined_stop.get_token();
        return ::platf::audio_policy::invoke(request);
      },
      internal_policy_process_result(),
      cancelled_policy_process_result()
    );
  }

  /**
   * @brief Lightweight IMMNotificationClient that signals a Win32 Event
   * when a non-ignored audio device becomes active or is added.
   * Used by reset_default_device() to wait for device arrival.
   */
  class device_arrival_notification_t: public ::IMMNotificationClient {
  public:
    /**
     * @param ignored_device_id Device ID to ignore in notifications (e.g., Steam Streaming Speakers).
     */
    explicit device_arrival_notification_t(const std::wstring &ignored_device_id):
        ignored_id(ignored_device_id) {
      arrival_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!arrival_event) {
        BOOST_LOG(warning) << "Failed to create device arrival event"sv;
      }
    }

    ~device_arrival_notification_t() {
      if (arrival_event) {
        CloseHandle(arrival_event);
      }
    }

    ULONG STDMETHODCALLTYPE AddRef() { return 1; }
    ULONG STDMETHODCALLTYPE Release() { return 1; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      } else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) { return S_OK; }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      if (arrival_event && !is_ignored(pwstrDeviceId)) {
        SetEvent(arrival_event);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
      if (dwNewState == DEVICE_STATE_ACTIVE && arrival_event && !is_ignored(pwstrDeviceId)) {
        SetEvent(arrival_event);
      }
      return S_OK;
    }

    /**
     * @brief Wait for the arrival event to be signaled.
     * @param timeout_ms Maximum time to wait in milliseconds.
     * @return true if signaled, false on timeout.
     */
    bool wait(HANDLE cancel_event, DWORD timeout_ms) {
      if (!arrival_event) {
        if (cancel_event) {
          WaitForSingleObject(cancel_event, timeout_ms);
        } else {
          Sleep(timeout_ms);
        }
        return false;
      }

      HANDLE wait_handles[2] {arrival_event, cancel_event};
      DWORD handle_count = cancel_event ? 2 : 1;
      auto result = WaitForMultipleObjects(handle_count, wait_handles, FALSE, timeout_ms);
      if (result == WAIT_OBJECT_0) {
        ResetEvent(arrival_event);
        return true;
      }
      return false;
    }

  private:
    bool is_ignored(LPCWSTR device_id) const {
      return device_id && !ignored_id.empty() && ignored_id == device_id;
    }

    HANDLE arrival_event = nullptr;
    std::wstring ignored_id;
  };

  class audio_notification_t: public ::IMMNotificationClient {
  public:
    audio_notification_t() {
    }

    // IUnknown implementation (unused by IMMDeviceEnumerator)
    ULONG STDMETHODCALLTYPE AddRef() {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID **ppvInterface) {
      if (IID_IUnknown == riid) {
        AddRef();
        *ppvInterface = (IUnknown *) this;
        return S_OK;
      } else if (__uuidof(IMMNotificationClient) == riid) {
        AddRef();
        *ppvInterface = (IMMNotificationClient *) this;
        return S_OK;
      } else {
        *ppvInterface = nullptr;
        return E_NOINTERFACE;
      }
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDeviceId) {
      if (flow == eRender) {
        default_render_device_changed_flag.store(true);
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(
      LPCWSTR pwstrDeviceId,
      DWORD dwNewState
    ) {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(
      LPCWSTR pwstrDeviceId,
      const PROPERTYKEY key
    ) {
      return S_OK;
    }

    /**
     * @brief Checks if the default rendering device changed and resets the change flag
     * @return `true` if the device changed since last call
     */
    bool check_default_render_device_changed() {
      return default_render_device_changed_flag.exchange(false);
    }

  private:
    std::atomic_bool default_render_device_changed_flag;
  };

  class mic_wasapi_t: public mic_t {
  public:
    capture_e sample(std::vector<float> &sample_out) override {
      auto sample_size = sample_out.size();

      // Refill the sample buffer if needed
      while (sample_buf_pos - std::begin(sample_buf) < sample_size) {
        auto capture_result = _fill_buffer();
        if (capture_result == capture_e::timeout && continuous_audio) {
          // Write silence to sample_buf
          std::fill_n(sample_buf_pos, sample_size, 0.0f);
          sample_buf_pos += sample_size;
        } else if (capture_result != capture_e::ok) {
          return capture_result;
        }
      }

      // Fill the output buffer with samples
      std::copy_n(std::begin(sample_buf), sample_size, std::begin(sample_out));

      // Move any excess samples to the front of the buffer
      std::move(&sample_buf[sample_size], sample_buf_pos, std::begin(sample_buf));
      sample_buf_pos -= sample_size;

      return capture_e::ok;
    }

    int init(std::uint32_t sample_rate, std::uint32_t frame_size, std::uint32_t channels_out, bool continuous) {
      audio_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
      if (!audio_event) {
        BOOST_LOG(error) << "Couldn't create Event handle"sv;

        return -1;
      }

      HRESULT status;

      status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = device_enum->RegisterEndpointNotificationCallback(&endpt_notification);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't register endpoint notification [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      auto device = default_device(device_enum);
      if (!device) {
        return -1;
      }

      for (const auto &format : formats) {
        if (format.channel_count != channels_out) {
          BOOST_LOG(debug) << "Skipping audio format ["sv << format.name << "] with channel count ["sv
                           << format.channel_count << " != "sv << channels_out << ']';
          continue;
        }

        BOOST_LOG(debug) << "Trying audio format ["sv << format.name << ']';
        audio_client = make_audio_client(device, format);

        if (audio_client) {
          BOOST_LOG(debug) << "Found audio format ["sv << format.name << ']';
          channels = channels_out;
          break;
        }
      }

      if (!audio_client) {
        BOOST_LOG(error) << "Couldn't find supported format for audio"sv;
        return -1;
      }

      REFERENCE_TIME default_latency;
      audio_client->GetDevicePeriod(&default_latency, nullptr);
      default_latency_ms = default_latency / 1000;
      continuous_audio = continuous;

      std::uint32_t frames;
      status = audio_client->GetBufferSize(&frames);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't acquire the number of audio frames [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      // *2 --> needs to fit double
      sample_buf = util::buffer_t<float> {std::max(frames, frame_size) * 2 * channels_out};
      sample_buf_pos = std::begin(sample_buf);

      status = audio_client->GetService(IID_IAudioCaptureClient, (void **) &audio_capture);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't initialize audio capture client [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      status = audio_client->SetEventHandle(audio_event.get());
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't set event handle [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      {
        DWORD task_index = 0;
        mmcss_task_handle = AvSetMmThreadCharacteristics("Pro Audio", &task_index);
        if (!mmcss_task_handle) {
          BOOST_LOG(error) << "Couldn't associate audio capture thread with Pro Audio MMCSS task [0x" << util::hex(GetLastError()).to_string_view() << ']';
        }
      }

      status = audio_client->Start();
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't start recording [0x"sv << util::hex(status).to_string_view() << ']';

        return -1;
      }

      return 0;
    }

    ~mic_wasapi_t() override {
      if (device_enum) {
        device_enum->UnregisterEndpointNotificationCallback(&endpt_notification);
      }

      if (audio_client) {
        audio_client->Stop();
      }

      if (mmcss_task_handle) {
        AvRevertMmThreadCharacteristics(mmcss_task_handle);
      }
    }

  private:
    capture_e _fill_buffer() {
      HRESULT status;

      // Total number of samples
      struct sample_aligned_t {
        std::uint32_t uninitialized;
        float *samples;
      } sample_aligned;

      // number of samples / number of channels
      struct block_aligned_t {
        std::uint32_t audio_sample_size;
      } block_aligned;

      // Check if the default audio device has changed
      if (endpt_notification.check_default_render_device_changed()) {
        // Invoke the audio_control_t's callback if it wants one
        if (default_endpt_changed_cb) {
          (*default_endpt_changed_cb)();
        }

        // Reinitialize to pick up the new default device
        return capture_e::reinit;
      }

      status = WaitForSingleObjectEx(audio_event.get(), default_latency_ms, FALSE);
      switch (status) {
        case WAIT_OBJECT_0:
          break;
        case WAIT_TIMEOUT:
          return capture_e::timeout;
        default:
          BOOST_LOG(error) << "Couldn't wait for audio event: [0x"sv << util::hex(status).to_string_view() << ']';
          return capture_e::error;
      }

      std::uint32_t packet_size {};
      for (
        status = audio_capture->GetNextPacketSize(&packet_size);
        SUCCEEDED(status) && packet_size > 0;
        status = audio_capture->GetNextPacketSize(&packet_size)
      ) {
        DWORD buffer_flags;
        status = audio_capture->GetBuffer(
          (BYTE **) &sample_aligned.samples,
          &block_aligned.audio_sample_size,
          &buffer_flags,
          nullptr,
          nullptr
        );

        switch (status) {
          case S_OK:
            break;
          case AUDCLNT_E_DEVICE_INVALIDATED:
            return capture_e::reinit;
          default:
            BOOST_LOG(error) << "Couldn't capture audio [0x"sv << util::hex(status).to_string_view() << ']';
            return capture_e::error;
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
          BOOST_LOG(debug) << "Audio capture signaled buffer discontinuity";
        }

        sample_aligned.uninitialized = std::end(sample_buf) - sample_buf_pos;
        auto n = std::min(sample_aligned.uninitialized, block_aligned.audio_sample_size * channels);

        if (n < block_aligned.audio_sample_size * channels) {
          BOOST_LOG(warning) << "Audio capture buffer overflow";
        }

        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
          std::fill_n(sample_buf_pos, n, 0);
        } else {
          std::copy_n(sample_aligned.samples, n, sample_buf_pos);
        }

        sample_buf_pos += n;

        audio_capture->ReleaseBuffer(block_aligned.audio_sample_size);
      }

      if (status == AUDCLNT_E_DEVICE_INVALIDATED) {
        return capture_e::reinit;
      }

      if (FAILED(status)) {
        return capture_e::error;
      }

      return capture_e::ok;
    }

  public:
    handle_t audio_event;

    device_enum_t device_enum;
    device_t device;
    audio_client_t audio_client;
    audio_capture_t audio_capture;

    audio_notification_t endpt_notification;
    std::optional<std::function<void()>> default_endpt_changed_cb;

    REFERENCE_TIME default_latency_ms;

    util::buffer_t<float> sample_buf;
    float *sample_buf_pos;
    int channels;
    bool continuous_audio;

    HANDLE mmcss_task_handle = nullptr;
  };

  class audio_control_t: public ::platf::audio_control_t {
  public:
    explicit audio_control_t(bool owns_policy_role_lanes = false):
        owns_policy_role_lanes_ {owns_policy_role_lanes} {
      if (owns_policy_role_lanes_) {
        activate_policy_role_lane_runtime();
      }
    }

    std::optional<sink_t> sink_info() override {
      sink_t sink;

      // Capture each render role before the virtual sink replaces them. The
      // console path below retains its existing pending-restore behavior.
      auto default_device_ids = current_default_device_ids();
      if (default_device_ids[role_index(eConsole)].empty()) {
        return std::nullopt;
      }

      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return std::nullopt;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      const auto configured_non_steam_id = configured_non_steam_render_endpoint(catalog);
      const auto pending_preferred_id = pending_preferred_restore_id();
      const bool any_steam_default = std::any_of(
        default_device_ids.begin(),
        default_device_ids.end(),
        [&](const auto &device_id) {
          return contains_device_id(steam_device_ids, device_id);
        }
      );
      if (!any_steam_default) {
        clear_pending_preferred_restore();
      }

      // Never capture another render half of the Steam full-duplex topology as
      // the host endpoint. Prefer the explicitly configured non-Steam sink when
      // it is active, then the pending endpoint, then any complete-catalog
      // eligible non-Steam fallback. Existing non-Steam role choices remain
      // unchanged, and a Steam console with no eligible replacement fails.
      std::vector<std::wstring> preferred_replacements;
      if (configured_non_steam_id) {
        preferred_replacements.push_back(*configured_non_steam_id);
      }
      if (pending_preferred_id) {
        preferred_replacements.push_back(*pending_preferred_id);
      }
      const auto replacement_id = active_non_steam_render_endpoint(
        catalog,
        preferred_replacements,
        true
      );

      std::vector<std::string> default_ids_utf8;
      default_ids_utf8.reserve(default_device_ids.size());
      for (const auto &device_id : default_device_ids) {
        default_ids_utf8.push_back(utf_utils::to_utf8(device_id.c_str()));
      }
      std::vector<std::string> steam_ids_utf8;
      steam_ids_utf8.reserve(catalog.steam_endpoint_ids.size());
      for (const auto &device_id : catalog.steam_endpoint_ids) {
        steam_ids_utf8.push_back(device_id);
      }
      auto sanitized_ids = ::audio::policy::sanitize_captured_role_ids(
        default_ids_utf8,
        steam_ids_utf8,
        replacement_id ? utf_utils::to_utf8(replacement_id->c_str()) : std::string {}
      );
      if (!sanitized_ids) {
        BOOST_LOG(error) << "No eligible non-Steam render endpoint can replace the Steam console endpoint"sv;
        return std::nullopt;
      }
      for (std::size_t index = 0; index < default_device_ids.size(); ++index) {
        default_device_ids[index] = utf_utils::from_utf8((*sanitized_ids)[index]);
      }

      sink.host = utf_utils::to_utf8(default_device_ids[role_index(eConsole)].c_str());
      // Pre-populate the restore-cache so we have property snapshots even if
      // the device disappears before reset_default_device runs.
      for (const auto &device_id : default_device_ids) {
        if (!device_id.empty()) {
          (void) preferred_device_match_list(device_id);
        }
      }
      captured_default_device_ids = std::move(default_device_ids);

      // Prepare to search for the device_id of the virtual audio sink device,
      // this device can be either user-configured or
      // the Steam Streaming Speakers we use by default.
      match_fields_list_t match_list;
      if (config::audio.virtual_sink.empty()) {
        match_list = match_steam_speakers();
      } else {
        match_list = match_all_fields(utf_utils::from_utf8(config::audio.virtual_sink));
      }

      // Search for the virtual audio sink device currently present in the system.
      auto matched = find_device_id(match_list);
      if (matched) {
        // Prepare to fill virtual audio sink names with device_id.
        auto device_id = utf_utils::to_utf8(matched->second);
        // Also prepend format name (basically channel layout at the moment)
        // because we don't want to extend the platform interface.
        sink.null = std::make_optional(sink_t::null_t {
          "virtual-"s + formats[0].name + device_id,
          "virtual-"s + formats[1].name + device_id,
          "virtual-"s + formats[2].name + device_id,
        });
      } else if (!config::audio.virtual_sink.empty()) {
        BOOST_LOG(warning) << "Couldn't find the specified virtual audio sink " << config::audio.virtual_sink;
      }

      return sink;
    }

    bool is_sink_available(const std::string &sink) override {
      const auto match_list = match_all_fields(utf_utils::from_utf8(sink));
      const auto matched = find_device_id(match_list);
      return static_cast<bool>(matched);
    }

    /**
     * @brief Extract virtual audio sink information possibly encoded in the sink name.
     * @param sink The sink name
     * @return A pair of device_id and format reference if the sink name matches
     *         our naming scheme for virtual audio sinks, `std::nullopt` otherwise.
     */
    std::optional<std::pair<std::wstring, std::reference_wrapper<const format_t>>> extract_virtual_sink_info(const std::string &sink) {
      // Encoding format:
      // [virtual-(format name)]device_id
      std::string current = sink;
      auto prefix = "virtual-"sv;
      if (current.find(prefix) == 0) {
        current = current.substr(prefix.size(), current.size() - prefix.size());

        for (const auto &format : formats) {
          auto &name = format.name;
          if (current.find(name) == 0) {
            auto device_id = utf_utils::from_utf8(current.substr(name.size(), current.size() - name.size()));
            return std::make_pair(device_id, std::reference_wrapper(format));
          }
        }
      }

      return std::nullopt;
    }

    std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous_audio, [[maybe_unused]] bool host_audio_enabled) override {
      auto mic = std::make_unique<mic_wasapi_t>();

      if (mic->init(sample_rate, frame_size, channels, continuous_audio)) {
        return nullptr;
      }

      if (config::audio.keep_default) {
        // If this is a virtual sink, set a callback that will change the sink back if it's changed
        auto virtual_sink_info = extract_virtual_sink_info(assigned_sink);
        if (virtual_sink_info) {
          mic->default_endpt_changed_cb = [this] {
            BOOST_LOG(info) << "Resetting sink to ["sv << assigned_sink << "] after default changed";
            set_sink(assigned_sink);
          };
        }
      }

      return mic;
    }

    /**
     * If the requested sink is a virtual sink, meaning no speakers attached to
     * the host, then we can seamlessly set the format to stereo and surround sound.
     *
     * Any virtual sink detected will be prefixed by:
     *    virtual-(format name)
     * If it doesn't contain that prefix, then the format will not be changed
     */
    std::optional<std::wstring> set_format(const std::string &sink) {
      if (sink.empty()) {
        return std::nullopt;
      }

      auto virtual_sink_info = extract_virtual_sink_info(sink);

      if (!virtual_sink_info) {
        // Sink name does not begin with virtual-(format name), hence it's not a virtual sink
        // and we don't want to change playback format of the corresponding device.
        // Also need to perform matching, sink name is not necessarily device_id in this case.
        auto matched = find_device_id(match_all_fields(utf_utils::from_utf8(sink)));
        if (matched) {
          return matched->second;
        } else {
          BOOST_LOG(error) << "Couldn't find audio sink " << sink;
          return std::nullopt;
        }
      }

      // When switching to a Steam virtual speaker device, try to retain the bit depth of the
      // default audio device. Switching from a 16-bit device to a 24-bit one has been known to
      // cause glitches for some users.
      int wanted_bits_per_sample = 32;
      const auto current_default_read = current_default_device_id(eConsole);
      if (current_default_read.is_known()) {
        audio::device_t current_default_dev;
        if (SUCCEEDED(device_enum->GetDevice(
              current_default_read.id.c_str(),
              &current_default_dev)) &&
            current_default_dev) {
          audio::prop_t prop;
          prop_var_t current_device_format;
          if (SUCCEEDED(current_default_dev->OpenPropertyStore(STGM_READ, &prop)) &&
              prop &&
              SUCCEEDED(prop->GetValue(
                PKEY_AudioEngine_DeviceFormat,
                &current_device_format.prop)) &&
              current_device_format.prop.vt == VT_BLOB &&
              current_device_format.prop.blob.pBlobData &&
              current_device_format.prop.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto *format = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(
              current_device_format.prop.blob.pBlobData
            );
            wanted_bits_per_sample = format->Samples.wValidBitsPerSample;
            BOOST_LOG(info) << "Virtual audio device will use "sv
                            << wanted_bits_per_sample
                            << "-bit to match default device"sv;
          }
        }
      }

      const auto &device_id = virtual_sink_info->first;
      const auto &waveformats =
        virtual_sink_info->second.get().virtual_sink_waveformats;
      for (const auto &waveformat : waveformats) {
        if (wanted_bits_per_sample != waveformat.Samples.wValidBitsPerSample) {
          continue;
        }

        const auto format_result = invoke_policy_process_on_role_lane({
          ::platf::audio_policy::operation_e::set_device_format,
          ::platf::audio_policy::render_role_e::console,
          device_id,
          ::platf::audio_policy::kDefaultTimeout,
          {},
          policy_process_wave_format(waveformat),
        });
        if (format_result.stage ==
              ::platf::audio_policy::failure_stage_e::success &&
            format_result.process_reaped &&
            SUCCEEDED(format_result.format_hresult)) {
          BOOST_LOG(info) << "Changed virtual audio sink format to "
                          << logging::bracket(
                               waveformat_to_pretty_string(waveformat));
          return device_id;
        }
      }

      BOOST_LOG(error) << "Couldn't set virtual audio sink waveformat";
      return std::nullopt;
    }

    int set_sink(const std::string &sink) override {
      auto device_id = set_format(sink);
      if (!device_id) {
        return -1;
      }

      // Cancel immediately before replacing the defaults so a failed format
      // setup leaves the existing recovery worker intact.
      const auto current_default_reads = current_default_device_id_reads();
      if (std::any_of(
            current_default_reads.begin(),
            current_default_reads.end(),
            [](const auto &read) {
              return !read.is_known();
            })) {
        BOOST_LOG(warning) << "Deferring Steam audio activation until every role default can be read"sv;
        return -1;
      }
      const auto current_default_ids = known_default_device_ids(current_default_reads);
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return -1;
      }

      std::vector<std::wstring> preferred_replacements;
      if (auto configured_id = configured_non_steam_render_endpoint(catalog)) {
        preferred_replacements.push_back(*configured_id);
      }
      if (auto pending_id = pending_preferred_restore_id()) {
        preferred_replacements.push_back(*pending_id);
      }
      const auto replacement_id = active_non_steam_render_endpoint(
        catalog,
        preferred_replacements,
        true
      );

      std::vector<std::string> current_ids_utf8;
      current_ids_utf8.reserve(current_default_ids.size());
      for (const auto &current_id : current_default_ids) {
        current_ids_utf8.push_back(utf_utils::to_utf8(current_id.c_str()));
      }
      auto sanitized_current_ids = ::audio::policy::sanitize_captured_role_ids(
        current_ids_utf8,
        catalog.steam_endpoint_ids,
        replacement_id ? utf_utils::to_utf8(replacement_id->c_str()) : std::string {}
      );
      if (!sanitized_current_ids) {
        BOOST_LOG(error) << "No eligible non-Steam render endpoint can be captured before switching to Steam audio"sv;
        return -1;
      }

      role_device_ids_t desired_device_ids;
      desired_device_ids.fill(*device_id);
      auto pending_restore_handoff = begin_policy_assignment(std::move(desired_device_ids));
      const auto assignment_epoch = pending_restore_handoff.assignment_epoch;
      pending_role_restores_t transferred_role_restores;
      if (!pending_restore_handoff.role_restores.empty()) {
        transferred_role_restores = normalize_pending_role_restores(
          std::move(pending_restore_handoff.role_restores),
          current_default_reads
        );
      }

      // The initial setup happens before microphone callbacks exist. Later
      // callbacks reapply the same sink, so capture defaults only on the first
      // assignment. If a previous session was still restoring a role, preserve
      // its preferred endpoint while the worker-owned fallback remains selected.
      if (assigned_device_id.empty()) {
        for (std::size_t index = 0; index < current_default_ids.size(); ++index) {
          captured_default_device_ids[index] = utf_utils::from_utf8((*sanitized_current_ids)[index]);
        }

        for (const auto &role_restore : transferred_role_restores) {
          const auto index = role_index(role_restore.role);
          const auto preferred_utf8 = utf_utils::to_utf8(role_restore.preferred_id.c_str());
          if (::audio::policy::is_eligible_non_steam_fallback(catalog, preferred_utf8)) {
            captured_default_device_ids[index] = role_restore.preferred_id;
          }
        }

        assigned_device_id = *device_id;
      }

      int failure {};
      bool assignment_active = true;
      pending_role_restores_t failed_role_restores;
      for (int x = 0; x < (int) ERole_enum_count; ++x) {
        const auto role = static_cast<ERole>(x);
        auto result = set_session_default_endpoint_for_assignment(assignment_epoch, role, *device_id);
        if (!result) {
          assignment_active = false;
          break;
        }
        const auto status = *result;
        if (status) {
          // Depending on the format of the string, we could get either of these errors
          if (status == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || status == E_INVALIDARG) {
            BOOST_LOG(warning) << "Audio sink not found: "sv << sink;
          } else {
            BOOST_LOG(warning) << "Couldn't set ["sv << sink << "] to role ["sv << x << "]: 0x"sv << util::hex(status).to_string_view();
          }

          ++failure;
          for (const auto &role_restore : transferred_role_restores) {
            if (role_restore.role == role) {
              failed_role_restores.push_back(role_restore);
              break;
            }
          }
        }
      }

      if (assignment_active &&
          !failed_role_restores.empty() &&
          !pending_restore_handoff.steam_device_id.empty()) {
        // This role never transferred to the new stream, so keep its prior
        // recovery alive instead of dropping the preferred endpoint.
        start_pending_role_restore_task(
          pending_restore_handoff.steam_device_id,
          std::move(failed_role_restores),
          assignment_epoch
        );
      }

      // Remember the assigned sink name, so we have it for later if we need to set it
      // back after another application changes it
      if (assignment_active && !failure) {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (policy_assignment_epoch_ref() == assignment_epoch) {
          assigned_sink = sink;
        }
      }

      return failure;
    }

    int restore_sink(const std::string &) override {
      // Preserve the old teardown order: stop a pending retry before writing
      // the captured role defaults. Start with the exact live defaults as the
      // committed assignment; each captured target commits only after a
      // successful policy call and matching readback.
      const auto current_default_reads = current_default_device_id_reads();
      const auto current_default_ids = known_default_device_ids(current_default_reads);
      auto desired_device_ids = current_default_ids;
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        if (!current_default_reads[role_index(role)].is_known() &&
            !captured_default_device_ids[role_index(role)].empty()) {
          desired_device_ids[role_index(role)] =
            captured_default_device_ids[role_index(role)];
        }
      }
      pending_role_restore_handoff = begin_policy_assignment(std::move(desired_device_ids));
      const auto assignment_epoch = pending_role_restore_handoff.assignment_epoch;
      if (pending_role_restore_handoff.steam_device_id.empty()) {
        pending_role_restore_handoff.steam_device_id = assigned_device_id;
      }

      int failure {};
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        const auto &captured_device_id = captured_default_device_ids[role_index(role)];
        if (!current_default_reads[role_index(role)].is_known()) {
          if (!assigned_device_id.empty() &&
              !captured_device_id.empty() &&
              captured_device_id != assigned_device_id) {
            pending_role_restore_handoff.role_restores.push_back({
              role,
              captured_device_id,
              assigned_device_id,
              {},
              {},
              false,
              true,
            });
            ++failure;
          }
          continue;
        }
        if (assigned_device_id.empty() ||
            current_default_ids[role_index(role)] != assigned_device_id) {
          continue;
        }

        if (captured_device_id.empty() || captured_device_id == assigned_device_id) {
          continue;
        }

        auto result = set_default_endpoint_for_assignment_before_publishing(
          assignment_epoch,
          role,
          current_default_ids[role_index(role)],
          captured_device_id
        );
        if (!result) {
          ++failure;
          break;
        }
        const auto status = *result;
        if (status != S_OK) {
          BOOST_LOG(warning) << "Couldn't restore captured audio endpoint for role ["sv << x
                             << "]: 0x"sv << util::hex(status).to_string_view();
          ++failure;
          if (FAILED(status)) {
            pending_role_restore_handoff.role_restores.push_back({
              role,
              captured_device_id,
              current_default_ids[role_index(role)],
            });
          }
        }
      }

      return failure;
    }

    enum class match_field_e {
      device_id,  ///< Match device_id
      device_friendly_name,  ///< Match endpoint friendly name
      adapter_friendly_name,  ///< Match adapter friendly name
      device_description,  ///< Match endpoint description
    };

    using match_fields_list_t = std::vector<std::pair<match_field_e, std::wstring>>;
    using matched_field_t = std::pair<match_field_e, std::wstring>;

    audio_control_t::match_fields_list_t match_steam_speakers() {
      return {
        {match_field_e::adapter_friendly_name, L"Steam Streaming Speakers"}
      };
    }

    audio_control_t::match_fields_list_t match_all_fields(const std::wstring &name) {
      return {
        {match_field_e::device_id, name},  // {0.0.0.00000000}.{29dd7668-45b2-4846-882d-950f55bf7eb8}
        {match_field_e::device_friendly_name, name},  // Digital Audio (S/PDIF) (High Definition Audio Device)
        {match_field_e::device_description, name},  // Digital Audio (S/PDIF)
        {match_field_e::adapter_friendly_name, name},  // High Definition Audio Device
      };
    }

    ::audio::policy::render_endpoint_catalog_t active_render_endpoint_catalog() {
      collection_t collection;
      const auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status) || !collection) {
        BOOST_LOG(error) << "Couldn't enumerate active render endpoints: [0x"sv
                         << util::hex(status).to_string_view() << ']';
        return ::audio::policy::build_render_endpoint_catalog(false, {});
      }

      UINT count = 0;
      const auto count_status = collection->GetCount(&count);
      if (FAILED(count_status)) {
        BOOST_LOG(error) << "Couldn't count active render endpoints: [0x"sv
                         << util::hex(count_status).to_string_view() << ']';
        return ::audio::policy::build_render_endpoint_catalog(false, {});
      }

      bool complete = true;
      std::vector<::audio::policy::render_endpoint_t> endpoints;
      endpoints.reserve(count);
      for (UINT index = 0; index < count; ++index) {
        audio::device_t device;
        if (FAILED(collection->Item(index, &device)) || !device) {
          complete = false;
          continue;
        }

        audio::wstring_t device_id;
        if (FAILED(device->GetId(&device_id)) || !device_id) {
          complete = false;
          continue;
        }

        std::string adapter_name;
        audio::prop_t properties;
        const auto property_status = device->OpenPropertyStore(STGM_READ, &properties);
        if (SUCCEEDED(property_status) && properties) {
          prop_var_t adapter_friendly_name;
          if (SUCCEEDED(properties->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop)) &&
              adapter_friendly_name.prop.vt == VT_LPWSTR &&
              adapter_friendly_name.prop.pwszVal &&
              adapter_friendly_name.prop.pwszVal[0] != L'\0') {
            adapter_name = utf_utils::to_utf8(adapter_friendly_name.prop.pwszVal);
          } else {
            complete = false;
          }
        } else {
          complete = false;
        }

        endpoints.push_back({
          utf_utils::to_utf8(device_id.get()),
          std::move(adapter_name),
          true,
        });
      }
      auto catalog = ::audio::policy::build_render_endpoint_catalog(complete, endpoints);
      if (!catalog.complete) {
        BOOST_LOG(warning) << "Active render endpoint discovery was incomplete; skipping Steam audio recovery policy writes"sv;
      }
      return catalog;
    }

    static std::vector<std::wstring> steam_render_device_ids(
      const ::audio::policy::render_endpoint_catalog_t &catalog
    ) {
      std::vector<std::wstring> ids;
      ids.reserve(catalog.steam_endpoint_ids.size());
      for (const auto &device_id : catalog.steam_endpoint_ids) {
        ids.push_back(utf_utils::from_utf8(device_id));
      }
      return ids;
    }

    static bool contains_device_id(
      const std::vector<std::wstring> &device_ids,
      const std::wstring &device_id
    ) {
      return !device_id.empty() &&
             std::find(device_ids.begin(), device_ids.end(), device_id) != device_ids.end();
    }

    std::vector<std::pair<ERole, std::wstring>> steam_owned_roles_from_snapshot(
      const role_device_ids_t &current_default_ids,
      const std::vector<std::wstring> &steam_device_ids
    ) {
      std::vector<std::string> current_ids;
      current_ids.reserve(current_default_ids.size());
      for (const auto &current_id : current_default_ids) {
        current_ids.push_back(utf_utils::to_utf8(current_id.c_str()));
      }

      std::vector<std::string> steam_ids;
      steam_ids.reserve(steam_device_ids.size());
      for (const auto &steam_id : steam_device_ids) {
        steam_ids.push_back(utf_utils::to_utf8(steam_id.c_str()));
      }

      const auto owned_snapshots = ::audio::policy::steam_owned_roles_from_snapshot(
        current_ids,
        steam_ids
      );
      std::vector<std::pair<ERole, std::wstring>> roles;
      roles.reserve(owned_snapshots.size());
      for (const auto &owned_snapshot : owned_snapshots) {
        roles.emplace_back(
          static_cast<ERole>(owned_snapshot.role_index),
          utf_utils::from_utf8(owned_snapshot.expected_current_id)
        );
      }
      return roles;
    }

    std::optional<std::wstring> active_non_steam_render_endpoint(
      const ::audio::policy::render_endpoint_catalog_t &catalog,
      const std::vector<std::wstring> &preferred_ids,
      bool allow_fallback
    ) {
      std::vector<std::string> preferred_utf8;
      preferred_utf8.reserve(preferred_ids.size());
      for (const auto &preferred_id : preferred_ids) {
        if (!preferred_id.empty()) {
          preferred_utf8.push_back(utf_utils::to_utf8(preferred_id.c_str()));
        }
      }

      auto selected = ::audio::policy::select_eligible_non_steam_render_endpoint(catalog, preferred_utf8);
      if (!selected) {
        return std::nullopt;
      }
      if (!allow_fallback &&
          std::find(preferred_utf8.begin(), preferred_utf8.end(), *selected) == preferred_utf8.end()) {
        return std::nullopt;
      }
      return utf_utils::from_utf8(*selected);
    }

    std::optional<std::wstring> configured_non_steam_render_endpoint(
      const ::audio::policy::render_endpoint_catalog_t &catalog
    ) {
      if (!catalog.complete) {
        return std::nullopt;
      }
      if (config::audio.sink.empty()) {
        return std::nullopt;
      }
      auto configured = find_device_id(match_all_fields(utf_utils::from_utf8(config::audio.sink)));
      if (!configured) {
        return std::nullopt;
      }
      return active_non_steam_render_endpoint(catalog, {configured->second}, false);
    }

    static std::mutex &preferred_restore_cache_mutex_ref() {
      static std::mutex mutex;
      return mutex;
    }

    static std::unordered_map<std::wstring, match_fields_list_t> &preferred_restore_cache_ref() {
      static std::unordered_map<std::wstring, match_fields_list_t> cache;
      return cache;
    }

    static std::wstring &pending_preferred_restore_id_ref() {
      static std::wstring id;
      return id;
    }

    static std::optional<std::wstring> pending_preferred_restore_id() {
      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      const auto &id = pending_preferred_restore_id_ref();
      if (id.empty()) {
        return std::nullopt;
      }

      return id;
    }

    static void remember_pending_preferred_restore(const std::wstring &preferred_id, const std::wstring &steam_device_id) {
      if (preferred_id.empty() || preferred_id == steam_device_id) {
        return;
      }

      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      pending_preferred_restore_id_ref() = preferred_id;
    }

    static void clear_pending_preferred_restore(const std::wstring &preferred_id = {}) {
      std::lock_guard lock {preferred_restore_cache_mutex_ref()};
      auto &pending_id = pending_preferred_restore_id_ref();
      if (preferred_id.empty() || pending_id == preferred_id) {
        pending_id.clear();
      }
    }

    default_endpoint_read_t current_default_device_id(ERole role) {
      if (const auto runtime = policy_role_lane_runtime()) {
        const auto current_lane = runtime->current_lane_index();
        if (current_lane && *current_lane != role_index(role)) {
          return {
            default_endpoint_read_state_e::unknown,
            {},
            E_UNEXPECTED,
          };
        }
      }
      return policy_process_readback(invoke_policy_process_on_role_lane({
        ::platf::audio_policy::operation_e::read,
        policy_process_role(role),
        {},
        ::platf::audio_policy::kDefaultTimeout,
        {},
      }));
    }

    role_device_id_reads_t current_default_device_id_reads() {
      role_device_id_reads_t device_ids;
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        device_ids[role_index(role)] = current_default_device_id(role);
      }
      return device_ids;
    }

    static role_device_ids_t known_default_device_ids(
      const role_device_id_reads_t &reads
    ) {
      role_device_ids_t device_ids;
      for (std::size_t index = 0; index < reads.size(); ++index) {
        if (reads[index].is_known()) {
          device_ids[index] = reads[index].id;
        }
      }
      return device_ids;
    }

    role_device_ids_t current_default_device_ids() {
      return known_default_device_ids(current_default_device_id_reads());
    }

    static pending_role_restores_t normalize_pending_role_restores(
      pending_role_restores_t role_restores,
      const role_device_id_reads_t &current_default_reads
    ) {
      pending_role_restores_t normalized;
      normalized.reserve(role_restores.size());
      for (auto &role_restore : role_restores) {
        const auto &current_read =
          current_default_reads[role_index(role_restore.role)];
        if (!current_read.is_known()) {
          role_restore.ownership_unconfirmed = true;
          normalized.push_back(std::move(role_restore));
          continue;
        }
        const auto &current_id = current_read.id;
        const bool retained_endpoint =
          current_id == role_restore.expected_current_id ||
          current_id == role_restore.preferred_id;
        if (!retained_endpoint) {
          const auto provenance = classify_restore_external_observation(
            role_restore.role,
            current_id
          );
          if (provenance !=
              ::audio::policy::restore_external_observation_action_e::adopt_external) {
            // A receipt target is not evidence of a user/system choice. Keep
            // the restore record so the role observer can repair the causal
            // stale write before the worker reevaluates ownership.
            normalized.push_back(std::move(role_restore));
          }
          // A remaining mismatch is a newer user or system choice. Never
          // replace it with the old target.
          continue;
        }
        if (role_restore.preferred_id.empty()) {
          continue;
        }

        // The live default is now the ownership guard for a restarted worker.
        role_restore.expected_current_id = current_id;
        normalized.push_back(std::move(role_restore));
      }
      return normalized;
    }

    static void append_match_field(match_fields_list_t &match_list, match_field_e field, const wchar_t *value) {
      if (value == nullptr || value[0] == L'\0') {
        return;
      }

      const std::wstring candidate {value};
      for (const auto &[existing_field, existing_value] : match_list) {
        if (existing_field == field && existing_value == candidate) {
          return;
        }
      }

      match_list.emplace_back(field, candidate);
    }

    std::optional<match_fields_list_t> preferred_device_match_list(const std::wstring &preferred_id) {
      {
        std::lock_guard lock {preferred_restore_cache_mutex_ref()};
        auto &cache = preferred_restore_cache_ref();
        const auto it = cache.find(preferred_id);
        if (it != cache.end()) {
          return it->second;
        }
      }

      audio::device_t device;
      if (FAILED(device_enum->GetDevice(preferred_id.c_str(), &device)) || !device) {
        return std::nullopt;
      }

      match_fields_list_t match_list;
      match_list.emplace_back(match_field_e::device_id, preferred_id);

      audio::prop_t prop;
      if (FAILED(device->OpenPropertyStore(STGM_READ, &prop)) || !prop) {
        return match_list;
      }

      prop_var_t device_friendly_name;
      prop_var_t adapter_friendly_name;
      prop_var_t device_desc;

      append_match_field(match_list, match_field_e::device_friendly_name,
        SUCCEEDED(prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop)) ? device_friendly_name.prop.pwszVal : nullptr);
      append_match_field(match_list, match_field_e::device_description,
        SUCCEEDED(prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop)) ? device_desc.prop.pwszVal : nullptr);
      append_match_field(match_list, match_field_e::adapter_friendly_name,
        SUCCEEDED(prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop)) ? adapter_friendly_name.prop.pwszVal : nullptr);

      {
        std::lock_guard lock {preferred_restore_cache_mutex_ref()};
        preferred_restore_cache_ref()[preferred_id] = match_list;
      }

      return match_list;
    }

    /**
     * @brief Search for currently present audio device_id using multiple match fields.
     * @param match_list Pairs of match fields and values
     * @return Optional pair of matched field and device_id
     */
    std::optional<matched_field_t> find_device_id(const match_fields_list_t &match_list) {
      if (match_list.empty()) {
        return std::nullopt;
      }

      collection_t collection;
      auto status = device_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't enumerate: [0x"sv << util::hex(status).to_string_view() << ']';
        return std::nullopt;
      }

      UINT count = 0;
      collection->GetCount(&count);

      std::vector<std::wstring> matched(match_list.size());
      for (auto x = 0; x < count; ++x) {
        audio::device_t device;
        collection->Item(x, &device);

        audio::wstring_t wstring_id;
        device->GetId(&wstring_id);
        std::wstring device_id = wstring_id.get();

        audio::prop_t prop;
        device->OpenPropertyStore(STGM_READ, &prop);

        prop_var_t adapter_friendly_name;
        prop_var_t device_friendly_name;
        prop_var_t device_desc;

        prop->GetValue(PKEY_Device_FriendlyName, &device_friendly_name.prop);
        prop->GetValue(PKEY_DeviceInterface_FriendlyName, &adapter_friendly_name.prop);
        prop->GetValue(PKEY_Device_DeviceDesc, &device_desc.prop);

        for (size_t i = 0; i < match_list.size(); i++) {
          if (matched[i].empty()) {
            const wchar_t *match_value = nullptr;
            switch (match_list[i].first) {
              case match_field_e::device_id:
                match_value = device_id.c_str();
                break;

              case match_field_e::device_friendly_name:
                match_value = device_friendly_name.prop.pwszVal;
                break;

              case match_field_e::adapter_friendly_name:
                match_value = adapter_friendly_name.prop.pwszVal;
                break;

              case match_field_e::device_description:
                match_value = device_desc.prop.pwszVal;
                break;
            }
            if (match_value && std::wcscmp(match_value, match_list[i].second.c_str()) == 0) {
              matched[i] = device_id;
            }
          }
        }
      }

      for (size_t i = 0; i < match_list.size(); i++) {
        if (!matched[i].empty()) {
          return matched_field_t(match_list[i].first, matched[i]);
        }
      }

      return std::nullopt;
    }

    /**
     * @brief Schedules role-aware recovery from every Steam render endpoint.
     * If a preferred eligible non-Steam device is supplied, each Steam-owned role retries
     * that exact endpoint while retaining ownership only of the fallback this
     * worker selected. Console, multimedia, and communications roles already
     * changed to non-Steam endpoints are never overwritten.
     * @param preferred_device The endpoint device_id of the device to restore.
     */
    void reset_default_device(const std::string &preferred_device = {}) override {
      // A stream can assign a different endpoint to the console, multimedia,
      // and communications roles. Always keep its recovery role-scoped so the
      // legacy all-role fallback cannot overwrite a role that was restored.
      if (!assigned_device_id.empty()) {
        reset_failed_default_roles();
        return;
      }

      std::wstring preferred_id;
      if (!preferred_device.empty()) {
        preferred_id = utf_utils::from_utf8(preferred_device);
      }
      reset_default_device_impl(preferred_id);
    }

    /**
     * @brief Non-blocking startup variant of reset_default_device().
     * Schedules the same role-aware recovery without waiting on audio policy.
     */
    void reset_default_device_no_wait() {
      reset_default_device_impl({});
    }

  private:
    bool is_default_device(const std::wstring &device_id, ERole role = eConsole) {
      const auto current_id = current_default_device_id(role);
      return current_id.is_known() && current_id.id == device_id;
    }

    static std::mutex &pending_restore_mutex_ref() {
      static std::mutex mutex;
      return mutex;
    }

    static std::mutex &policy_assignment_transition_mutex_ref() {
      static std::mutex mutex;
      return mutex;
    }

    static bool &policy_assignment_rollover_in_progress_ref() {
      static bool in_progress = false;
      return in_progress;
    }

    using pending_restore_token_t = std::shared_ptr<std::atomic_bool>;

    static pending_restore_token_t &pending_restore_token_ref() {
      static pending_restore_token_t token;
      return token;
    }

    static pending_role_restores_t &pending_role_restores_ref() {
      static pending_role_restores_t role_restores;
      return role_restores;
    }

    static std::wstring &pending_restore_steam_device_id_ref() {
      static std::wstring steam_device_id;
      return steam_device_id;
    }

    static std::uint64_t &policy_assignment_epoch_ref() {
      static std::uint64_t epoch = 0;
      return epoch;
    }

    static role_device_ids_t &policy_assignment_desired_ids_ref() {
      static role_device_ids_t desired_ids;
      return desired_ids;
    }

    static std::array<std::uint64_t, static_cast<std::size_t>(ERole_enum_count)> &
    policy_assignment_role_revisions_ref() {
      static std::array<std::uint64_t, static_cast<std::size_t>(ERole_enum_count)> revisions {};
      return revisions;
    }

    struct policy_role_intent_key_t {
      std::uint64_t assignment_epoch;
      std::uint64_t role_revision;
      std::wstring desired_id;
    };

    static std::optional<policy_role_intent_key_t>
    capture_policy_role_intent(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      const auto index = role_index(role);
      if (policy_assignment_epoch_ref() != assignment_epoch ||
          policy_assignment_desired_ids_ref()[index] != desired_id) {
        return std::nullopt;
      }
      return policy_role_intent_key_t {
        assignment_epoch,
        policy_assignment_role_revisions_ref()[index],
        desired_id,
      };
    }

    static bool policy_role_intent_is_current_locked(
      ERole role,
      const policy_role_intent_key_t &intent
    ) {
      const auto index = role_index(role);
      return ::audio::policy::policy_write_receipt_is_pending(
        intent.assignment_epoch,
        policy_assignment_epoch_ref(),
        intent.role_revision,
        policy_assignment_role_revisions_ref()[index],
        false,
        utf_utils::to_utf8(intent.desired_id.c_str()),
        utf_utils::to_utf8(policy_assignment_desired_ids_ref()[index].c_str())
      );
    }

    struct policy_write_receipt_t {
      std::uint64_t id;
      std::uint64_t assignment_epoch;
      std::uint64_t issuer_role_revision;
      std::wstring target_id;
      std::wstring issuer_active_id;
      default_endpoint_read_state_e prior_state;
      std::wstring previous_live_id;
      HRESULT prior_status;
      std::chrono::steady_clock::time_point issued_at;
      bool is_repair = false;
      std::uint64_t causal_receipt_id = 0;
      bool call_completed = false;
      HRESULT completion_status = E_PENDING;
      bool repair_hazard = false;
    };

    struct causal_repair_chain_t {
      std::uint64_t causal_receipt_id;
      std::wstring stale_target_id;
      std::wstring repair_target_id;
      ::audio::policy::causal_repair_state_t state;
      bool exhaustion_logged = false;
    };

    struct superseded_write_ledger_t {
      std::vector<policy_write_receipt_t> receipts;
      std::vector<std::wstring> tolerated_live_ids;
      std::vector<causal_repair_chain_t> repair_chains;
      std::uint64_t generation = 0;
      std::uint64_t next_receipt_id = 0;
      std::uint64_t observation_deadline_tick = 0;
    };

    struct policy_write_registration_t {
      std::uint64_t receipt_id;
      HRESULT status;
    };

    static constexpr std::uint64_t policy_observation_limit_ticks = 10'000;
    static constexpr std::uint64_t policy_repair_backoff_ticks = 1'000;
    static constexpr unsigned max_policy_repair_attempts = 1;

    static std::uint64_t policy_observation_tick() {
      return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()
        ).count()
      );
    }

    static std::array<superseded_write_ledger_t, static_cast<std::size_t>(ERole_enum_count)> &
    superseded_write_ledgers_ref() {
      static std::array<superseded_write_ledger_t, static_cast<std::size_t>(ERole_enum_count)> ledgers;
      return ledgers;
    }

    static void append_unique_id(
      std::vector<std::wstring> &ids,
      const std::wstring &device_id
    ) {
      if (!device_id.empty() &&
          std::find(ids.begin(), ids.end(), device_id) == ids.end()) {
        ids.push_back(device_id);
      }
    }

    static void clear_policy_write_ledger_locked(superseded_write_ledger_t &ledger) {
      ledger.receipts.clear();
      ledger.tolerated_live_ids.clear();
      ledger.repair_chains.clear();
      ledger.observation_deadline_tick = 0;
    }

    // The caller holds pending_restore_mutex_ref(). Returns true when an
    // observer must be started after releasing the mutex.
    static bool rebind_policy_role_intent_locked(
      ERole role,
      const std::wstring &desired_id,
      bool force_new_revision
    ) {
      const auto index = role_index(role);
      auto &active_id = policy_assignment_desired_ids_ref()[index];
      if (!force_new_revision && active_id == desired_id) {
        return false;
      }
      active_id = desired_id;
      auto &revision = policy_assignment_role_revisions_ref()[index];
      if (++revision == 0) {
        ++revision;
      }

      auto &ledger = superseded_write_ledgers_ref()[index];
      if (ledger.receipts.empty()) {
        return false;
      }
      const auto now_tick = policy_observation_tick();
      // A new owner receives a full observer-retirement window, but it does
      // not create a new causal write. Preserve every root receipt's absolute
      // deadline and repair budget across the handoff.
      ledger.observation_deadline_tick =
        now_tick + policy_observation_limit_ticks;
      return true;
    }

    static policy_write_registration_t register_policy_write_receipt_locked(
      ERole role,
      const policy_role_intent_key_t &issuer_intent,
      const std::wstring &target_id,
      const default_endpoint_read_t &prior_read,
      std::uint64_t causal_receipt_id = 0
    ) {
      if (!policy_role_intent_is_current_locked(role, issuer_intent)) {
        return {0, E_ABORT};
      }
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      constexpr std::size_t max_receipts_per_role = 64;
      const auto capacity = ::audio::policy::decide_policy_receipt_capacity(
        ledger.receipts.size(),
        max_receipts_per_role,
        // Exact-readback receipts are erased immediately. Every retained entry
        // is unresolved or a timed-out hazard and is therefore non-reclaimable.
        std::vector<bool>(ledger.receipts.size(), false)
      );
      if (capacity.action ==
          ::audio::policy::policy_receipt_capacity_action_e::reject_unresolved) {
        BOOST_LOG(error) << "Rejecting audio policy write because the bounded per-role receipt ledger contains only unresolved hazards for role ["sv
                         << static_cast<int>(role) << ']';
        return {0, HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA)};
      }
      if (++ledger.next_receipt_id == 0) {
        ++ledger.next_receipt_id;
      }
      const auto now_tick = policy_observation_tick();
      const bool is_repair = causal_receipt_id != 0;
      auto causal_chain = ledger.repair_chains.end();
      if (is_repair) {
        causal_chain = std::find_if(
          ledger.repair_chains.begin(),
          ledger.repair_chains.end(),
          [&](const auto &chain) {
            return chain.causal_receipt_id == causal_receipt_id;
          }
        );
        if (causal_chain == ledger.repair_chains.end()) {
          return {0, E_ABORT};
        }
      }
      const auto proposed_deadline = now_tick + policy_observation_limit_ticks;
      ledger.observation_deadline_tick =
        ledger.observation_deadline_tick == 0 ?
          proposed_deadline :
          ::audio::policy::causal_repair_deadline_after_generation(
            ledger.observation_deadline_tick,
            proposed_deadline,
            is_repair
          );
      ledger.receipts.push_back({
        ledger.next_receipt_id,
        issuer_intent.assignment_epoch,
        issuer_intent.role_revision,
        target_id,
        issuer_intent.desired_id,
        prior_read.state,
        prior_read.id,
        prior_read.status,
        std::chrono::steady_clock::now(),
      });
      auto &receipt = ledger.receipts.back();
      receipt.is_repair = is_repair;
      receipt.causal_receipt_id = causal_receipt_id;
      if (!is_repair) {
        const causal_repair_chain_t new_chain {
          receipt.id,
          target_id,
          {},
          {false, 0, now_tick, proposed_deadline},
        };
        ledger.repair_chains.push_back(new_chain);
      }
      if (++ledger.generation == 0) {
        ++ledger.generation;
      }
      return {ledger.next_receipt_id, S_OK};
    }

    static void mark_policy_write_receipt_hazard_locked(
      ERole role,
      std::uint64_t receipt_id
    ) {
      if (receipt_id == 0) {
        return;
      }
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      auto &receipts = ledger.receipts;
      const auto receipt = std::find_if(
        receipts.begin(),
        receipts.end(),
        [&](const auto &candidate) {
          return candidate.id == receipt_id;
        }
      );
      if (receipt != receipts.end() && !receipt->repair_hazard) {
        receipt->repair_hazard = true;
        if (++ledger.generation == 0) {
          ++ledger.generation;
        }
      }
    }

    static void confirm_policy_write_receipt_locked(
      ERole role,
      std::uint64_t receipt_id
    ) {
      if (receipt_id == 0) {
        return;
      }
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      const auto confirmed = std::find_if(
        ledger.receipts.begin(),
        ledger.receipts.end(),
        [&](const auto &receipt) {
          return receipt.id == receipt_id;
        }
      );
      if (confirmed == ledger.receipts.end()) {
        return;
      }
      if (!confirmed->call_completed) {
        return;
      }
      const bool confirmed_repair = confirmed->is_repair;
      const auto causal_receipt_id = confirmed->causal_receipt_id;
      if (confirmed_repair) {
        const auto chain = std::find_if(
          ledger.repair_chains.begin(),
          ledger.repair_chains.end(),
          [&](const auto &candidate) {
            return candidate.causal_receipt_id == causal_receipt_id;
          }
        );
        if (chain != ledger.repair_chains.end()) {
          chain->state = ::audio::policy::complete_causal_repair(
            chain->state,
            policy_observation_tick(),
            true,
            true,
            policy_repair_backoff_ticks
          );
        }
      } else {
        std::erase_if(ledger.repair_chains, [&](const auto &chain) {
          return chain.causal_receipt_id == receipt_id;
        });
      }
      const auto erased = std::erase_if(ledger.receipts, [&](const auto &receipt) {
        return receipt.id == receipt_id;
      });
      if (erased != 0) {
        // Exact readback commits a new phase. Historical tolerated IDs must not
        // hide a later user re-selection, and remaining stale receipts receive
        // a fresh settling window against the newly committed role owner.
        ledger.tolerated_live_ids.clear();
        if (++ledger.generation == 0) {
          ++ledger.generation;
        }
        if (ledger.receipts.empty()) {
          ledger.repair_chains.clear();
          ledger.observation_deadline_tick = 0;
        }
      }
    }

    static void erase_unissued_policy_write_receipt_locked(
      ERole role,
      std::uint64_t receipt_id
    ) {
      if (receipt_id == 0) {
        return;
      }
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      const auto receipt = std::find_if(
        ledger.receipts.begin(),
        ledger.receipts.end(),
        [&](const auto &candidate) {
          return candidate.id == receipt_id;
        }
      );
      if (receipt == ledger.receipts.end()) {
        return;
      }
      if (receipt->is_repair) {
        const auto chain = std::find_if(
          ledger.repair_chains.begin(),
          ledger.repair_chains.end(),
          [&](const auto &candidate) {
            return candidate.causal_receipt_id == receipt->causal_receipt_id;
          }
        );
        if (chain != ledger.repair_chains.end()) {
          chain->state = ::audio::policy::retract_unissued_causal_repair(
            chain->state,
            policy_observation_tick()
          );
          chain->repair_target_id.clear();
        }
      } else {
        std::erase_if(ledger.repair_chains, [&](const auto &chain) {
          return chain.causal_receipt_id == receipt_id;
        });
      }
      ledger.receipts.erase(receipt);
      if (++ledger.generation == 0) {
        ++ledger.generation;
      }
      if (ledger.receipts.empty()) {
        clear_policy_write_ledger_locked(ledger);
      }
    }

    static void deactivate_pending_restore_worker_locked() {
      auto &token = pending_restore_token_ref();
      if (token) {
        token->store(false, std::memory_order_release);
        token.reset();
      }
      pending_role_restores_ref().clear();
      pending_restore_steam_device_id_ref().clear();
    }

    static bool pending_restore_worker_owns_state_locked(const pending_restore_token_t &token, std::uint64_t assignment_epoch) {
      return token &&
             token->load(std::memory_order_acquire) &&
             pending_restore_token_ref() == token &&
             policy_assignment_epoch_ref() == assignment_epoch;
    }

    static bool pending_restore_worker_can_write(
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (stop_token.stop_requested() || !token || !token->load(std::memory_order_acquire)) {
        return false;
      }

      std::scoped_lock lock(pending_restore_mutex_ref());
      return pending_restore_worker_owns_state_locked(token, assignment_epoch);
    }

    static bool pending_restore_worker_is_active(
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return pending_restore_worker_owns_state_locked(token, assignment_epoch);
    }

    static pending_role_restore_handoff_t begin_policy_assignment(role_device_ids_t desired_ids) {
      std::scoped_lock transition_lock(policy_assignment_transition_mutex_ref());
      pending_role_restore_handoff_t handoff;
      std::array<bool, static_cast<std::size_t>(ERole_enum_count)> start_observers {};
      bool requires_runtime_rotation = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        handoff.steam_device_id = std::move(pending_restore_steam_device_id_ref());
        handoff.role_restores = std::move(pending_role_restores_ref());
        deactivate_pending_restore_worker_locked();

        auto &assignment_epoch = policy_assignment_epoch_ref();
        requires_runtime_rotation =
          assignment_epoch == std::numeric_limits<std::uint64_t>::max();
        if (requires_runtime_rotation) {
          // The replacement runtime callback must not replay the exhausted
          // assignment. The new owner is installed only after the predecessor
          // runtime has fully joined and the successor generation exists.
          policy_assignment_rollover_in_progress_ref() = true;
        } else {
          assignment_epoch = *::audio::policy::advance_policy_assignment_epoch(
            assignment_epoch,
            {}
          );
          handoff.assignment_epoch = assignment_epoch;
        }

        // Assignment ownership is part of the observer key even when the
        // handoff itself issues no write. Re-anchor the observer to the new
        // owner/window so a paused predecessor cannot retire it; causal root
        // deadlines and repair budgets remain immutable.
        if (!requires_runtime_rotation) {
          for (std::size_t index = 0; index < superseded_write_ledgers_ref().size(); ++index) {
            const auto role = static_cast<ERole>(index);
            start_observers[index] = rebind_policy_role_intent_locked(
              role,
              desired_ids[index],
              true
            );
            auto &ledger = superseded_write_ledgers_ref()[index];
            if (ledger.receipts.empty()) {
              continue;
            }
            if (++ledger.generation == 0) {
              ++ledger.generation;
            }
          }
        }
      }
      std::uint64_t runtime_generation = 0;
      std::shared_ptr<::audio::policy::fixed_role_lane_coordinator_t> runtime;
      if (requires_runtime_rotation) {
        const auto next_epoch = ::audio::policy::advance_policy_assignment_epoch(
          std::numeric_limits<std::uint64_t>::max(),
          [&]() {
            return policy_role_lane_runtime_manager_ref().rotate(
              &runtime_generation
            );
          }
        );
        if (!next_epoch) {
          std::scoped_lock lock(pending_restore_mutex_ref());
          policy_assignment_rollover_in_progress_ref() = false;
          if (!handoff.role_restores.empty()) {
            pending_restore_token_ref() =
              std::make_shared<std::atomic_bool>(true);
            pending_restore_steam_device_id_ref() =
              std::move(handoff.steam_device_id);
            pending_role_restores_ref() = std::move(handoff.role_restores);
          }
          BOOST_LOG(error) << "Couldn't rotate the bounded audio-policy runtime at assignment-epoch exhaustion; retained the prior assignment without issuing a new write"sv;
          return {};
        }
        runtime = policy_role_lane_runtime(nullptr, &runtime_generation);
        {
          std::scoped_lock lock(pending_restore_mutex_ref());
          policy_assignment_epoch_ref() = *next_epoch;
          handoff.assignment_epoch = *next_epoch;
          policy_assignment_rollover_in_progress_ref() = false;
          for (std::size_t index = 0; index < superseded_write_ledgers_ref().size(); ++index) {
            const auto role = static_cast<ERole>(index);
            start_observers[index] = rebind_policy_role_intent_locked(
              role,
              desired_ids[index],
              true
            );
            auto &ledger = superseded_write_ledgers_ref()[index];
            if (!ledger.receipts.empty() && ++ledger.generation == 0) {
              ++ledger.generation;
            }
          }
        }
      } else {
        runtime = policy_role_lane_runtime(nullptr, &runtime_generation);
      }
      if (runtime) {
        runtime->clear_restore_steps_before({
          runtime_generation,
          handoff.assignment_epoch,
        });
      }
      for (std::size_t index = 0; index < start_observers.size(); ++index) {
        if (start_observers[index]) {
          start_policy_write_observer(static_cast<ERole>(index));
        }
      }
      return handoff;
    }

    static bool policy_assignment_role_is_current(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return policy_assignment_epoch_ref() == assignment_epoch &&
             policy_assignment_desired_ids_ref()[role_index(role)] == desired_id;
    }

    static bool policy_assignment_is_current(std::uint64_t assignment_epoch) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return policy_assignment_epoch_ref() == assignment_epoch;
    }

    static bool publish_policy_assignment_role(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      bool start_observer = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (policy_assignment_epoch_ref() != assignment_epoch) {
          return false;
        }
        start_observer = rebind_policy_role_intent_locked(
          role,
          desired_id,
          false
        );
      }
      if (start_observer) {
        start_policy_write_observer(role);
      }
      return true;
    }

    static bool publish_policy_assignment_role_if_current(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &committed_expected_id,
      std::uint64_t committed_role_revision,
      const std::wstring &desired_id
    ) {
      bool start_observer = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        const auto &active_id = policy_assignment_desired_ids_ref()[role_index(role)];
        if (policy_assignment_epoch_ref() != assignment_epoch ||
            policy_assignment_role_revisions_ref()[role_index(role)] !=
              committed_role_revision ||
            active_id != committed_expected_id) {
          return false;
        }
        start_observer = rebind_policy_role_intent_locked(
          role,
          desired_id,
          false
        );
      }
      if (start_observer) {
        start_policy_write_observer(role);
      }
      return true;
    }

    static bool remember_pending_preferred_restore_for_assignment(
      const std::wstring &preferred_id,
      const std::wstring &steam_device_id,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (policy_assignment_epoch_ref() != assignment_epoch) {
        return false;
      }
      remember_pending_preferred_restore(preferred_id, steam_device_id);
      return true;
    }

    static void clear_pending_preferred_restore_for_assignment(
      std::uint64_t assignment_epoch,
      const std::wstring &preferred_id = {}
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (policy_assignment_epoch_ref() == assignment_epoch) {
        clear_pending_preferred_restore(preferred_id);
      }
    }

    struct issued_policy_write_t {
      HRESULT status;
      std::uint64_t receipt_id;
      bool issuer_assignment_still_current;
      default_endpoint_read_t readback;
    };

    static ::audio::policy::policy_observer_owner_key_t
    policy_observer_owner_key_locked(ERole role) {
      const auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      return {
        ledger.generation,
        policy_assignment_epoch_ref(),
        policy_assignment_role_revisions_ref()[role_index(role)],
        utf_utils::to_utf8(
          policy_assignment_desired_ids_ref()[role_index(role)].c_str()
        ),
      };
    }

    struct policy_write_observer_state_t {
      ::audio::policy::policy_observer_owner_key_t owner {};
      std::uint64_t observation_deadline_tick = 0;
      int stable_observations = 0;
      std::uint64_t observed_assignment_epoch = 0;
      std::wstring observed_desired_id;
      std::wstring observed_live_id;
    };

    static audio_control_t *role_lane_audio_control() {
      thread_local co_init_t co_init;
      thread_local audio_control_t control;
      thread_local bool initialized = false;
      if (!initialized) {
        initialized = control.init() == 0;
      }
      return initialized ? &control : nullptr;
    }

    static bool start_policy_write_observer(
      ERole role,
      const std::shared_ptr<
        ::audio::policy::fixed_role_lane_coordinator_t
      > &runtime
    ) {
      auto state = std::make_shared<policy_write_observer_state_t>();
      const auto accepted = runtime && runtime->set_observer_step(
        role_index(role),
        [role, state](std::stop_token stop_token) {
          if (stop_token.stop_requested()) {
            return false;
          }
          auto *observer = role_lane_audio_control();
          if (!observer) {
            {
              std::scoped_lock lock(pending_restore_mutex_ref());
              const auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
              if (ledger.receipts.empty()) {
                return false;
              }
              const auto current_owner = policy_observer_owner_key_locked(role);
              if (state->owner.generation == 0 ||
                  ::audio::policy::policy_observer_owner_changed(
                    state->owner,
                    current_owner)) {
                state->owner = current_owner;
                state->observation_deadline_tick = ledger.observation_deadline_tick;
              }
              if (policy_observation_tick() >= state->observation_deadline_tick) {
                BOOST_LOG(error) << "Audio policy observer initialization remained unavailable through the bounded causal deadline for role ["sv
                                 << static_cast<int>(role)
                                 << "]; retaining unresolved receipts and rejecting writes if capacity is exhausted"sv;
                return false;
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return true;
          }
          return observer->observe_policy_write_receipts_step(role, *state);
        }
      );
      if (!accepted) {
        BOOST_LOG(warning) << "Failed to start audio policy write observer for role ["sv
                           << static_cast<int>(role) << ']';
      }
      return accepted;
    }

    static void start_policy_write_observer(ERole role) {
      bool replayed_on_creation = false;
      auto runtime = policy_role_lane_runtime(&replayed_on_creation);
      if (replayed_on_creation) {
        return;
      }
      start_policy_write_observer(role, runtime);
    }

    static policy_write_registration_t register_policy_write_receipt(
      ERole role,
      const policy_role_intent_key_t &issuer_intent,
      const std::wstring &target_id,
      const default_endpoint_read_t &prior_read,
      std::uint64_t causal_receipt_id = 0
    ) {
      policy_write_registration_t registration;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        registration = register_policy_write_receipt_locked(
          role,
          issuer_intent,
          target_id,
          prior_read,
          causal_receipt_id
        );
      }
      if (registration.receipt_id != 0) {
        start_policy_write_observer(role);
      }
      return registration;
    }

    static void mark_policy_write_receipt_hazard(
      ERole role,
      std::uint64_t receipt_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      mark_policy_write_receipt_hazard_locked(role, receipt_id);
    }

    static void confirm_policy_write_receipt(
      ERole role,
      std::uint64_t receipt_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      confirm_policy_write_receipt_locked(role, receipt_id);
    }

    static void erase_unissued_policy_write_receipt(
      ERole role,
      std::uint64_t receipt_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      erase_unissued_policy_write_receipt_locked(role, receipt_id);
    }

    static std::optional<std::uint64_t> pending_policy_write_receipt_for_target(
      ERole role,
      std::uint64_t assignment_epoch,
      const std::wstring &target_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      const auto &receipts =
        superseded_write_ledgers_ref()[role_index(role)].receipts;
      const auto receipt = std::find_if(
        receipts.begin(),
        receipts.end(),
        [&](const auto &candidate) {
          return candidate.assignment_epoch == assignment_epoch &&
                 candidate.target_id == target_id &&
                 ::audio::policy::policy_write_receipt_is_pending(
                   candidate.assignment_epoch,
                   policy_assignment_epoch_ref(),
                   candidate.issuer_role_revision,
                   policy_assignment_role_revisions_ref()[role_index(role)],
                   candidate.repair_hazard,
                   utf_utils::to_utf8(candidate.issuer_active_id.c_str()),
                   utf_utils::to_utf8(
                     policy_assignment_desired_ids_ref()[role_index(role)].c_str()
                   )
                 );
        }
      );
      return receipt == receipts.end() ?
        std::nullopt :
        std::optional<std::uint64_t> {receipt->id};
    }

    static bool policy_write_receipt_can_execute(
      ERole role,
      std::uint64_t receipt_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      const auto &receipts =
        superseded_write_ledgers_ref()[role_index(role)].receipts;
      const auto receipt = std::find_if(
        receipts.begin(),
        receipts.end(),
        [&](const auto &candidate) {
          return candidate.id == receipt_id;
        }
      );
      if (receipt == receipts.end() || receipt->call_completed) {
        return false;
      }
      return ::audio::policy::policy_write_receipt_is_pending(
        receipt->assignment_epoch,
        policy_assignment_epoch_ref(),
        receipt->issuer_role_revision,
        policy_assignment_role_revisions_ref()[role_index(role)],
        receipt->repair_hazard,
        utf_utils::to_utf8(receipt->issuer_active_id.c_str()),
        utf_utils::to_utf8(
          policy_assignment_desired_ids_ref()[role_index(role)].c_str()
        )
      );
    }

    static std::optional<std::uint64_t> claim_causal_policy_repair(
      ERole role,
      const std::wstring &stale_target_id,
      const std::wstring &repair_target_id,
      const policy_role_intent_key_t &issuer_intent
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!policy_role_intent_is_current_locked(role, issuer_intent)) {
        return std::nullopt;
      }
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      const auto now_tick = policy_observation_tick();
      for (auto &chain : ledger.repair_chains) {
        if (chain.stale_target_id != stale_target_id || !chain.state.outstanding) {
          continue;
        }
        const auto expiration = ::audio::policy::plan_causal_repair(
          chain.state,
          now_tick,
          true,
          max_policy_repair_attempts
        );
        if (expiration.action == ::audio::policy::causal_repair_action_e::retire) {
          chain.state = expiration.state;
          if (!chain.exhaustion_logged) {
            chain.exhaustion_logged = true;
            BOOST_LOG(warning) << "Retiring bounded audio policy repair attempts for role ["sv
                               << static_cast<int>(role) << ']';
          }
        }
      }
      std::vector<::audio::policy::causal_repair_chain_candidate_t> candidates;
      candidates.reserve(ledger.repair_chains.size());
      for (const auto &chain : ledger.repair_chains) {
        candidates.push_back({
          chain.causal_receipt_id,
          utf_utils::to_utf8(chain.stale_target_id.c_str()),
          chain.state,
        });
      }
      const auto selection = ::audio::policy::select_causal_repair_chain(
        candidates,
        utf_utils::to_utf8(stale_target_id.c_str()),
        now_tick,
        max_policy_repair_attempts
      );
      if (!selection.causal_receipt_id) {
        return std::nullopt;
      }
      const auto chain = std::find_if(
        ledger.repair_chains.begin(),
        ledger.repair_chains.end(),
        [&](const auto &candidate) {
          return candidate.causal_receipt_id == *selection.causal_receipt_id;
        }
      );
      if (chain == ledger.repair_chains.end()) {
        return std::nullopt;
      }
      chain->state = selection.transition.state;
      if (selection.transition.action == ::audio::policy::causal_repair_action_e::retire) {
        if (!chain->exhaustion_logged) {
          chain->exhaustion_logged = true;
          BOOST_LOG(warning) << "Retiring bounded audio policy repair attempts for role ["sv
                             << static_cast<int>(role) << ']';
        }
        return std::nullopt;
      }
      if (selection.transition.action != ::audio::policy::causal_repair_action_e::issue) {
        return std::nullopt;
      }
      chain->repair_target_id = repair_target_id;
      return chain->causal_receipt_id;
    }

    static void complete_causal_policy_repair(
      ERole role,
      std::uint64_t causal_receipt_id,
      bool set_succeeded,
      bool exact_target_readback
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      auto &chains = superseded_write_ledgers_ref()[role_index(role)].repair_chains;
      const auto chain = std::find_if(
        chains.begin(),
        chains.end(),
        [&](const auto &candidate) {
          return candidate.causal_receipt_id == causal_receipt_id;
        }
      );
      if (chain != chains.end()) {
        chain->state = ::audio::policy::complete_causal_repair(
          chain->state,
          policy_observation_tick(),
          set_succeeded,
          exact_target_readback,
          policy_repair_backoff_ticks
        );
      }
    }

    static bool confirm_delayed_causal_policy_repair_locked(
      ERole role,
      const std::wstring &live_id
    ) {
      auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
      bool completed_chain = false;
      std::vector<std::uint64_t> completed_causal_receipt_ids;
      for (auto &chain : ledger.repair_chains) {
        if (chain.state.outstanding && chain.repair_target_id == live_id) {
          chain.state = ::audio::policy::complete_causal_repair(
            chain.state,
            policy_observation_tick(),
            true,
            true,
            policy_repair_backoff_ticks
          );
          completed_causal_receipt_ids.push_back(chain.causal_receipt_id);
          completed_chain = true;
        }
      }
      const auto erased = std::erase_if(ledger.receipts, [&](const auto &receipt) {
        return receipt.is_repair && receipt.call_completed &&
               std::find(
                 completed_causal_receipt_ids.begin(),
                 completed_causal_receipt_ids.end(),
                 receipt.causal_receipt_id
               ) != completed_causal_receipt_ids.end();
      });
      if (erased == 0 && !completed_chain) {
        return false;
      }
      ledger.tolerated_live_ids.clear();
      if (++ledger.generation == 0) {
        ++ledger.generation;
      }
      return true;
    }

    issued_policy_write_t issue_policy_default_write(
      const policy_role_intent_key_t &issuer_intent,
      ERole role,
      const std::wstring &target_id,
      const default_endpoint_read_t &prior_read,
      std::uint64_t causal_receipt_id = 0,
      const std::stop_token &stop_token = {}
    ) {
      if (!prior_read.is_known()) {
        return {
          HRESULT_FROM_WIN32(ERROR_RETRY),
          0,
          false,
          prior_read,
        };
      }
      const auto registration = register_policy_write_receipt(
        role,
        issuer_intent,
        target_id,
        prior_read,
        causal_receipt_id
      );
      if (registration.receipt_id == 0) {
        return {
          registration.status,
          0,
          false,
          {},
        };
      }
      const auto receipt_id = registration.receipt_id;

      // The receipt exists before the supervised helper can issue the policy
      // write, regardless of its HRESULT or when Windows applies the effect.
      const ::platf::audio_policy::request_t process_request {
        ::platf::audio_policy::operation_e::set_and_readback,
        policy_process_role(role),
        target_id,
        ::platf::audio_policy::kDefaultTimeout,
        stop_token,
        std::nullopt,
        prior_read.id,
      };
      const auto process_result = invoke_policy_process_on_role_lane(
        process_request,
        [role, receipt_id]() {
          return policy_write_receipt_can_execute(role, receipt_id);
        });
      const auto status = policy_process_set_status(process_result);
      const auto readback = policy_process_readback(process_result);

      const bool may_have_executed =
        process_result.execution_disposition !=
          ::platf::audio_policy::execution_disposition_e::not_started;
      const bool helper_completed_and_reaped =
        process_result.process_reaped &&
        process_result.execution_disposition ==
          ::platf::audio_policy::execution_disposition_e::completed;
      const bool precondition_proved_no_write =
        helper_completed_and_reaped &&
        process_result.stage ==
          ::platf::audio_policy::failure_stage_e::precondition &&
        process_result.set_hresult ==
          ::platf::audio_policy::kPreconditionMismatchHresult &&
        process_result.read_hresult == S_OK &&
        !process_result.readback_id.empty();
      const bool pre_read_proved_no_write =
        ::audio::policy::authenticated_policy_pre_read_proves_no_write({
          process_result.process_reaped,
          process_result.execution_disposition ==
            ::platf::audio_policy::execution_disposition_e::completed,
          process_result.stage ==
            ::platf::audio_policy::failure_stage_e::pre_read,
          process_result.set_hresult == S_OK,
          FAILED(process_result.read_hresult),
          process_result.readback_id.empty(),
        });
      if (::audio::policy::classify_policy_write_execution(
            may_have_executed,
            precondition_proved_no_write,
            pre_read_proved_no_write) ==
          ::audio::policy::policy_write_execution_action_e::erase_unissued) {
        erase_unissued_policy_write_receipt(role, receipt_id);
        return {
          status,
          0,
          policy_assignment_role_is_current(
            issuer_intent.assignment_epoch,
            role,
            issuer_intent.desired_id
          ),
          readback,
        };
      }

      bool issuer_assignment_still_current = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
        const auto receipt = std::find_if(
          ledger.receipts.begin(),
          ledger.receipts.end(),
          [&](const auto &candidate) {
            return candidate.id == receipt_id;
          }
        );
        if (receipt != ledger.receipts.end()) {
          receipt->call_completed = process_result.process_reaped;
          receipt->completion_status = status;
          issuer_assignment_still_current =
            policy_assignment_epoch_ref() == receipt->assignment_epoch &&
            policy_assignment_role_revisions_ref()[role_index(role)] ==
              receipt->issuer_role_revision &&
            policy_assignment_desired_ids_ref()[role_index(role)] ==
              receipt->issuer_active_id;
          if (FAILED(status) || !issuer_assignment_still_current) {
            receipt->repair_hazard = true;
          }
          // The pre-call receipt can be observed while the policy API blocks.
          // HRESULT annotation advances only the ownership version; the root
          // chain's absolute observation deadline remains immutable.
          if (++ledger.generation == 0) {
            ++ledger.generation;
          }
        }
      }
      if (causal_receipt_id != 0) {
        complete_causal_policy_repair(
          role,
          causal_receipt_id,
          SUCCEEDED(status),
          false
        );
      }
      return {status, receipt_id, issuer_assignment_still_current, readback};
    }

    bool observe_policy_write_receipts_step(
      ERole role,
      policy_write_observer_state_t &state
    ) {
      constexpr int required_stable_observations = 50;
      constexpr auto observation_backoff = std::chrono::milliseconds(100);
      std::uint64_t assignment_epoch;
      std::wstring desired_id;
      std::vector<std::wstring> stale_target_ids;
      std::vector<std::wstring> tolerated_live_ids;
      std::vector<std::wstring> pending_target_ids;
      bool all_calls_completed = true;
      ::audio::policy::policy_observer_owner_key_t current_owner;
      std::uint64_t current_deadline_tick;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        const auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
        if (ledger.receipts.empty()) {
          return false;
        }
        current_owner = policy_observer_owner_key_locked(role);
        current_deadline_tick = ledger.observation_deadline_tick;
        if (state.owner.generation == 0) {
          state.owner = current_owner;
          state.observation_deadline_tick = current_deadline_tick;
        }
        assignment_epoch = policy_assignment_epoch_ref();
        desired_id = policy_assignment_desired_ids_ref()[role_index(role)];
        tolerated_live_ids = ledger.tolerated_live_ids;
        for (const auto &receipt : ledger.receipts) {
          all_calls_completed = all_calls_completed && receipt.call_completed;
          const bool pending_for_active_epoch =
            ::audio::policy::policy_write_receipt_is_pending(
              receipt.assignment_epoch,
              assignment_epoch,
              receipt.issuer_role_revision,
              policy_assignment_role_revisions_ref()[role_index(role)],
              receipt.repair_hazard,
              utf_utils::to_utf8(receipt.issuer_active_id.c_str()),
              utf_utils::to_utf8(desired_id.c_str())
            );
          if (pending_for_active_epoch) {
            append_unique_id(pending_target_ids, receipt.target_id);
            append_unique_id(tolerated_live_ids, receipt.target_id);
            append_unique_id(tolerated_live_ids, receipt.previous_live_id);
          } else {
            append_unique_id(stale_target_ids, receipt.target_id);
          }
        }
        std::erase_if(stale_target_ids, [&](const auto &target_id) {
          return std::find(
                   pending_target_ids.begin(),
                   pending_target_ids.end(),
                   target_id
                 ) != pending_target_ids.end();
        });
      }

      if (::audio::policy::policy_observer_owner_changed(
            state.owner,
            current_owner)) {
        state.owner = current_owner;
        state.observed_assignment_epoch = 0;
        state.observed_desired_id.clear();
        state.observed_live_id.clear();
        state.stable_observations = 0;
        state.observation_deadline_tick = current_deadline_tick;
        return true;
      }
      if (policy_observation_tick() >= state.observation_deadline_tick) {
        std::scoped_lock lock(pending_restore_mutex_ref());
        const auto current_locked_owner = policy_observer_owner_key_locked(role);
        if (!::audio::policy::policy_observer_owner_changed(
              state.owner,
              current_locked_owner)) {
          BOOST_LOG(error) << "Audio policy causal observation expired for role ["sv
                           << static_cast<int>(role)
                           << "]; retaining unresolved receipts and rejecting writes if capacity is exhausted"sv;
          return false;
        }
        return true;
      }

      const auto live_read = current_default_device_id(role);
      const auto &live_id = live_read.id;
      if (live_read.is_known() && live_id == desired_id) {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (confirm_delayed_causal_policy_repair_locked(role, live_id)) {
          return true;
        }
      }
      const bool observation_state_unchanged =
        state.observed_assignment_epoch == assignment_epoch &&
        state.observed_desired_id == desired_id &&
        state.observed_live_id == live_id;
      if (!observation_state_unchanged) {
        state.observed_assignment_epoch = assignment_epoch;
        state.observed_desired_id = desired_id;
        state.observed_live_id = live_id;
      }
      std::vector<std::string> stale_ids_utf8;
      std::vector<std::string> tolerated_ids_utf8;
      stale_ids_utf8.reserve(stale_target_ids.size());
      tolerated_ids_utf8.reserve(tolerated_live_ids.size());
      for (const auto &device_id : stale_target_ids) {
        stale_ids_utf8.push_back(utf_utils::to_utf8(device_id.c_str()));
      }
      for (const auto &device_id : tolerated_live_ids) {
        tolerated_ids_utf8.push_back(utf_utils::to_utf8(device_id.c_str()));
      }
      const auto transition = ::audio::policy::observe_superseded_write(
        stale_ids_utf8,
        tolerated_ids_utf8,
        utf_utils::to_utf8(live_id.c_str()),
        utf_utils::to_utf8(desired_id.c_str()),
        assignment_epoch != 0 && !desired_id.empty(),
        false
      );

      if (transition.action ==
          ::audio::policy::superseded_write_observation_action_e::cancel) {
        state.stable_observations = 0;
        std::this_thread::sleep_for(observation_backoff);
        return true;
      }
      if (transition.action ==
          ::audio::policy::superseded_write_observation_action_e::adopt_external) {
        if (publish_policy_assignment_role_if_current(
              assignment_epoch,
              role,
              desired_id,
              current_owner.role_revision,
              live_id)) {
          std::scoped_lock lock(pending_restore_mutex_ref());
          auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
          if (policy_assignment_epoch_ref() == assignment_epoch &&
              policy_assignment_desired_ids_ref()[role_index(role)] == live_id) {
            for (auto &receipt : ledger.receipts) {
              receipt.repair_hazard = true;
            }
            ledger.tolerated_live_ids.clear();
            append_unique_id(ledger.tolerated_live_ids, live_id);
          }
        }
        return true;
      }
      const policy_role_intent_key_t repair_intent {
        assignment_epoch,
        current_owner.role_revision,
        desired_id,
      };
      std::optional<std::uint64_t> causal_repair_claim;
      if (transition.action ==
            ::audio::policy::superseded_write_observation_action_e::repair_active &&
          policy_assignment_role_is_current(assignment_epoch, role, desired_id) &&
          is_default_device(live_id, role) &&
          policy_assignment_role_is_current(assignment_epoch, role, desired_id) &&
          (causal_repair_claim = claim_causal_policy_repair(
             role,
             live_id,
             desired_id,
             repair_intent))) {
        const auto repair = issue_policy_default_write(
          repair_intent,
          role,
          desired_id,
          live_read,
          *causal_repair_claim
        );
        if (repair.receipt_id == 0) {
          complete_causal_policy_repair(
            role,
            *causal_repair_claim,
            false,
            false
          );
        }
        const auto &repair_readback = repair.readback;
        if (repair.issuer_assignment_still_current &&
            repair_readback.is_known() &&
            repair_readback.id == desired_id &&
            policy_assignment_role_is_current(assignment_epoch, role, desired_id)) {
          complete_causal_policy_repair(
            role,
            *causal_repair_claim,
            true,
            true
          );
          confirm_policy_write_receipt(role, repair.receipt_id);
        }
        if (FAILED(repair.status)) {
          BOOST_LOG(warning) << "Couldn't compare-and-repair the current audio endpoint for role ["sv
                             << static_cast<int>(role) << "]: 0x"sv
                             << util::hex(repair.status).to_string_view();
        }
        return true;
      }

      const auto progress = ::audio::policy::advance_superseded_write_observer(
        false,
        false,
        live_read.is_known(),
        observation_state_unchanged,
        transition.action ==
            ::audio::policy::superseded_write_observation_action_e::keep_observing &&
          all_calls_completed,
        state.stable_observations,
        required_stable_observations
      );
      state.stable_observations = progress.stable_observations;
      if (progress.action ==
          ::audio::policy::superseded_write_observer_progress_action_e::retire_generation) {
        std::scoped_lock lock(pending_restore_mutex_ref());
        auto &ledger = superseded_write_ledgers_ref()[role_index(role)];
        const auto current_locked_owner = policy_observer_owner_key_locked(role);
        if (!::audio::policy::policy_observer_owner_changed(
              state.owner,
              current_locked_owner)) {
          clear_policy_write_ledger_locked(ledger);
          return false;
        }
        return true;
      }
      std::this_thread::sleep_for(observation_backoff);
      return true;
    }

    // Session activation is intentionally product-owned before the writes: a
    // newer assignment must be able to reassert the virtual sink while an older
    // worker exits. Restoration paths use the commit-after-readback helper.
    std::optional<HRESULT> set_session_default_endpoint_for_assignment(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      if (!publish_policy_assignment_role(assignment_epoch, role, desired_id)) {
        return std::nullopt;
      }
      const auto issuer_intent = capture_policy_role_intent(
        assignment_epoch,
        role,
        desired_id
      );
      if (!issuer_intent) {
        return std::nullopt;
      }

      const auto previous_live = current_default_device_id(role);
      if (!previous_live.is_known()) {
        return HRESULT_FROM_WIN32(ERROR_RETRY);
      }
      const auto issued = issue_policy_default_write(
        *issuer_intent,
        role,
        desired_id,
        previous_live
      );
      const auto &readback = issued.readback;
      const bool assignment_still_current =
        issued.issuer_assignment_still_current &&
        policy_assignment_role_is_current(assignment_epoch, role, desired_id);
      const bool exact_target_readback_confirmed =
        SUCCEEDED(issued.status) &&
        assignment_still_current &&
        readback.is_known() &&
        readback.id == desired_id;
      if (!::audio::policy::activation_policy_write_requires_settling(
            SUCCEEDED(issued.status),
            assignment_still_current,
            exact_target_readback_confirmed)) {
        confirm_policy_write_receipt(role, issued.receipt_id);
      }
      if (!assignment_still_current ||
          !policy_assignment_role_is_current(assignment_epoch, role, desired_id)) {
        return std::nullopt;
      }
      return issued.status;
    }

    std::optional<HRESULT> set_default_endpoint_for_assignment_before_publishing(
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &committed_expected_id,
      const std::wstring &inflight_target_id
    ) {
      if (!policy_assignment_role_is_current(assignment_epoch, role, committed_expected_id)) {
        return std::nullopt;
      }
      const auto issuer_intent = capture_policy_role_intent(
        assignment_epoch,
        role,
        committed_expected_id
      );
      if (!issuer_intent) {
        return std::nullopt;
      }
      const auto live_read = current_default_device_id(role);
      const auto live_id = live_read.is_known() ?
        std::optional<std::string> {utf_utils::to_utf8(live_read.id.c_str())} :
        std::nullopt;
      const auto live_observation =
        ::audio::policy::classify_default_endpoint_observation(
          utf_utils::to_utf8(committed_expected_id.c_str()),
          live_id
        );
      if (live_observation ==
          ::audio::policy::default_endpoint_observation_e::unavailable) {
        return HRESULT_FROM_WIN32(ERROR_RETRY);
      }
      if (live_observation ==
          ::audio::policy::default_endpoint_observation_e::different_nonempty) {
        const auto completion = ::audio::policy::complete_fallback_role_transition(
          utf_utils::to_utf8(committed_expected_id.c_str()),
          utf_utils::to_utf8(inflight_target_id.c_str()),
          true,
          live_id
        );
        if (completion.action == ::audio::policy::fallback_commit_action_e::retry_committed) {
          return HRESULT_FROM_WIN32(ERROR_RETRY);
        }
        if (completion.action ==
              ::audio::policy::fallback_commit_action_e::release_external &&
            classify_restore_external_observation(role, live_read.id) !=
              ::audio::policy::restore_external_observation_action_e::adopt_external) {
          return HRESULT_FROM_WIN32(ERROR_RETRY);
        }
        if (!publish_policy_assignment_role_if_current(
              assignment_epoch,
              role,
              committed_expected_id,
              issuer_intent->role_revision,
              live_read.id)) {
          return std::nullopt;
        }
        if (completion.action == ::audio::policy::fallback_commit_action_e::commit_fallback) {
          if (const auto pending_receipt = pending_policy_write_receipt_for_target(
                role,
                assignment_epoch,
                live_read.id)) {
            confirm_policy_write_receipt(role, *pending_receipt);
          }
        }
        return completion.action == ::audio::policy::fallback_commit_action_e::commit_fallback ?
          S_OK :
          S_FALSE;
      }

      const auto issued = issue_policy_default_write(
        *issuer_intent,
        role,
        inflight_target_id,
        live_read
      );
      const bool assignment_still_current =
        issued.issuer_assignment_still_current &&
        policy_assignment_role_is_current(
          assignment_epoch,
          role,
          committed_expected_id
        );
      if (!assignment_still_current ||
          !policy_assignment_role_is_current(
            assignment_epoch,
            role,
            committed_expected_id)) {
        return std::nullopt;
      }
      if (FAILED(issued.status)) {
        return issued.status;
      }

      const auto &readback = issued.readback;
      const auto readback_id = readback.is_known() ?
        std::optional<std::string> {utf_utils::to_utf8(readback.id.c_str())} :
        std::nullopt;
      const auto completion = ::audio::policy::complete_fallback_role_transition(
        utf_utils::to_utf8(committed_expected_id.c_str()),
        utf_utils::to_utf8(inflight_target_id.c_str()),
        true,
        readback_id
      );
      if (completion.action == ::audio::policy::fallback_commit_action_e::retry_committed) {
        if (!policy_assignment_role_is_current(
              assignment_epoch,
              role,
              committed_expected_id)) {
          mark_policy_write_receipt_hazard(role, issued.receipt_id);
          return std::nullopt;
        }
        return HRESULT_FROM_WIN32(ERROR_RETRY);
      }
      if (completion.action == ::audio::policy::fallback_commit_action_e::release_external) {
        if (classify_restore_external_observation(role, readback.id) !=
            ::audio::policy::restore_external_observation_action_e::adopt_external) {
          return HRESULT_FROM_WIN32(ERROR_RETRY);
        }
        mark_policy_write_receipt_hazard(role, issued.receipt_id);
      }
      if (!publish_policy_assignment_role_if_current(
            assignment_epoch,
            role,
            committed_expected_id,
            issuer_intent->role_revision,
            readback.id)) {
        mark_policy_write_receipt_hazard(role, issued.receipt_id);
        return std::nullopt;
      }
      if (completion.action == ::audio::policy::fallback_commit_action_e::commit_fallback) {
        confirm_policy_write_receipt(role, issued.receipt_id);
      }
      return completion.action == ::audio::policy::fallback_commit_action_e::commit_fallback ?
        S_OK :
        S_FALSE;
    }

    bool publish_policy_assignment_role_for_worker(
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &desired_id
    ) {
      bool start_observer = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
          return false;
        }
        start_observer = rebind_policy_role_intent_locked(
          role,
          desired_id,
          false
        );
      }
      if (start_observer) {
        start_policy_write_observer(role);
      }
      return true;
    }

    static ::audio::policy::restore_external_observation_action_e
    classify_restore_external_observation_locked(
      ERole role,
      const std::wstring &external_id
    ) {
      const auto index = role_index(role);
      const auto &active_id = policy_assignment_desired_ids_ref()[index];
      std::vector<std::string> pending_target_ids;
      std::vector<std::string> stale_target_ids;
      const auto &ledger = superseded_write_ledgers_ref()[index];
      for (const auto &receipt : ledger.receipts) {
        const bool pending = ::audio::policy::policy_write_receipt_is_pending(
          receipt.assignment_epoch,
          policy_assignment_epoch_ref(),
          receipt.issuer_role_revision,
          policy_assignment_role_revisions_ref()[index],
          receipt.repair_hazard,
          utf_utils::to_utf8(receipt.issuer_active_id.c_str()),
          utf_utils::to_utf8(active_id.c_str())
        );
        auto &targets = pending ? pending_target_ids : stale_target_ids;
        const auto target_id = utf_utils::to_utf8(receipt.target_id.c_str());
        if (std::find(targets.begin(), targets.end(), target_id) == targets.end()) {
          targets.push_back(target_id);
        }
      }
      return ::audio::policy::classify_restore_external_observation(
        utf_utils::to_utf8(external_id.c_str()),
        pending_target_ids,
        stale_target_ids
      );
    }

    static ::audio::policy::restore_external_observation_action_e
    classify_restore_external_observation(
      ERole role,
      const std::wstring &external_id
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      return classify_restore_external_observation_locked(role, external_id);
    }

    enum class worker_external_adoption_action_e {
      adopted,
      retry_stale_receipt,
      inactive,
    };

    struct worker_external_adoption_result_t {
      worker_external_adoption_action_e action;
      std::wstring active_id;
    };

    worker_external_adoption_result_t adopt_external_policy_assignment_role_for_worker(
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &committed_expected_id,
      const std::wstring &inflight_target_id,
      const std::wstring &external_id
    ) {
      bool start_observer = false;
      worker_external_adoption_result_t result {
        worker_external_adoption_action_e::inactive,
        {},
      };
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
          return result;
        }
        const auto index = role_index(role);
        const auto &active_id = policy_assignment_desired_ids_ref()[index];
        const auto provenance =
          classify_restore_external_observation_locked(role, external_id);
        if (provenance !=
            ::audio::policy::restore_external_observation_action_e::adopt_external) {
          result.action = worker_external_adoption_action_e::retry_stale_receipt;
          result.active_id = active_id;
          return result;
        }
        if (active_id == committed_expected_id || active_id == inflight_target_id) {
          start_observer = rebind_policy_role_intent_locked(
            role,
            external_id,
            false
          );
        }
        result.action = worker_external_adoption_action_e::adopted;
        result.active_id = policy_assignment_desired_ids_ref()[index];
      }
      if (start_observer) {
        start_policy_write_observer(role);
      }
      return result;
    }

    struct worker_role_write_result_t {
      ::audio::policy::worker_role_write_action_e action;
      HRESULT status;
      std::uint64_t receipt_id;
      bool read_unavailable;
    };

    worker_role_write_result_t set_default_endpoint_for_worker_before_publishing(
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      ERole role,
      const std::wstring &committed_expected_id,
      const std::wstring &desired_id
    ) {
      bool worker_active =
        pending_restore_worker_can_write(stop_token, token, assignment_epoch);
      bool role_assignment_still_current =
        worker_active &&
        policy_assignment_role_is_current(
          assignment_epoch,
          role,
          committed_expected_id
        );
      auto action = ::audio::policy::worker_role_write_action(
        worker_active,
        role_assignment_still_current
      );
      if (action != ::audio::policy::worker_role_write_action_e::use_policy_status) {
        return {action, E_ABORT, 0, false};
      }

      const auto issuer_intent = capture_policy_role_intent(
        assignment_epoch,
        role,
        committed_expected_id
      );
      if (!issuer_intent) {
        worker_active = pending_restore_worker_can_write(
          stop_token,
          token,
          assignment_epoch
        );
        return {
          ::audio::policy::worker_role_write_action(worker_active, false),
          E_ABORT,
          0,
          false,
        };
      }

      const auto prior_read = current_default_device_id(role);
      const auto prior_id = prior_read.is_known() ?
        std::optional<std::string> {utf_utils::to_utf8(prior_read.id.c_str())} :
        std::nullopt;
      const auto observation =
        ::audio::policy::classify_default_endpoint_observation(
          utf_utils::to_utf8(committed_expected_id.c_str()),
          prior_id
        );
      if (observation ==
          ::audio::policy::default_endpoint_observation_e::unavailable) {
        return {
          ::audio::policy::worker_role_write_action_e::use_policy_status,
          HRESULT_FROM_WIN32(ERROR_RETRY),
          0,
          true,
        };
      }
      if (observation ==
          ::audio::policy::default_endpoint_observation_e::different_nonempty) {
        const auto active_id = adopt_external_policy_assignment_role_for_worker(
          token,
          assignment_epoch,
          role,
          committed_expected_id,
          desired_id,
          prior_read.id
        );
        const bool retry_stale = active_id.action ==
          worker_external_adoption_action_e::retry_stale_receipt;
        return {
          active_id.action == worker_external_adoption_action_e::adopted ?
            ::audio::policy::worker_role_write_action_e::release_external :
            (retry_stale ?
               ::audio::policy::worker_role_write_action_e::use_policy_status :
               ::audio::policy::worker_role_write_action_e::stop_worker),
          retry_stale ? HRESULT_FROM_WIN32(ERROR_RETRY) : E_ABORT,
          0,
          retry_stale,
        };
      }

      const auto issued = issue_policy_default_write(
        *issuer_intent,
        role,
        desired_id,
        prior_read,
        {},
        stop_token
      );
      worker_active = pending_restore_worker_can_write(
        stop_token,
        token,
        assignment_epoch
      );
      role_assignment_still_current =
        worker_active &&
        issued.issuer_assignment_still_current &&
        policy_assignment_role_is_current(
          assignment_epoch,
          role,
          committed_expected_id
        );
      if (!role_assignment_still_current) {
        mark_policy_write_receipt_hazard(role, issued.receipt_id);
      }
      worker_active = pending_restore_worker_can_write(
        stop_token,
        token,
        assignment_epoch
      );
      role_assignment_still_current =
        worker_active &&
        issued.issuer_assignment_still_current &&
        policy_assignment_role_is_current(
          assignment_epoch,
          role,
          committed_expected_id
        );
      action = ::audio::policy::worker_role_write_action(
        worker_active,
        role_assignment_still_current
      );
      return {action, issued.status, issued.receipt_id, false};
    }

    static bool schedule_pending_role_restore_tasks_on_runtime(
      const std::shared_ptr<
        ::audio::policy::fixed_role_lane_coordinator_t
      > &runtime,
      std::uint64_t runtime_generation,
      const std::wstring &steam_device_id,
      pending_role_restores_t role_restores,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (!runtime) {
        BOOST_LOG(warning) << "Audio restore lanes are quiescing; retaining published restore work for the next runtime generation"sv;
        return false;
      }
      bool all_scheduled = true;
      for (auto &role_restore : role_restores) {
        pending_role_restores_t lane_restores;
        lane_restores.push_back(std::move(role_restore));
        const auto role = lane_restores.front().role;
        const auto accepted = runtime->set_restore_step(
          role_index(role),
          {runtime_generation, assignment_epoch},
          [steam_device_id,
           lane_restores = std::move(lane_restores),
           token,
           assignment_epoch,
           retry_fallback_reset = true](std::stop_token stop_token) mutable {
            if (!pending_restore_worker_can_write(
                  stop_token,
                  token,
                  assignment_epoch)) {
              return false;
            }
            auto *restore_control = role_lane_audio_control();
            const auto bootstrap_action = ::audio::policy::worker_bootstrap_action(
              pending_restore_worker_can_write(
                stop_token,
                token,
                assignment_epoch),
              restore_control != nullptr
            );
            if (bootstrap_action == ::audio::policy::worker_bootstrap_action_e::stop) {
              return false;
            }
            if (bootstrap_action ==
                ::audio::policy::worker_bootstrap_action_e::retry_after_backoff) {
              std::this_thread::sleep_for(std::chrono::seconds(1));
              return true;
            }
            return restore_control->run_pending_role_restore_task(
              stop_token,
              steam_device_id,
              lane_restores,
              token,
              assignment_epoch,
              retry_fallback_reset
            );
          }
        );
        if (accepted !=
            ::audio::policy::restore_slot_install_result_e::installed &&
            accepted !=
            ::audio::policy::restore_slot_install_result_e::unchanged_equal) {
          BOOST_LOG(error) << "Failed to schedule bounded audio restore lane for role ["sv
                           << static_cast<int>(role)
                           << "]; retaining the published record for runtime restart"sv;
          all_scheduled = false;
        }
      }
      return all_scheduled;
    }

    static void start_pending_role_restore_task(
      const std::wstring &steam_device_id,
      pending_role_restores_t role_restores,
      std::uint64_t assignment_epoch
    ) {
      auto published_role_restores = role_restores;
      std::optional<std::wstring> console_preferred_id;
      for (const auto &role_restore : role_restores) {
        if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
          console_preferred_id = role_restore.preferred_id;
          break;
        }
      }

      auto token = std::make_shared<std::atomic_bool>(true);
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (policy_assignment_epoch_ref() != assignment_epoch) {
          token->store(false, std::memory_order_release);
          return;
        }
        deactivate_pending_restore_worker_locked();
        pending_restore_token_ref() = token;
        pending_restore_steam_device_id_ref() = steam_device_id;
        pending_role_restores_ref() = std::move(published_role_restores);
        if (console_preferred_id) {
          remember_pending_preferred_restore(*console_preferred_id, steam_device_id);
        } else {
          clear_pending_preferred_restore();
        }
      }

      bool replayed_on_creation = false;
      std::uint64_t runtime_generation = 0;
      auto runtime = policy_role_lane_runtime(
        &replayed_on_creation,
        &runtime_generation
      );
      if (replayed_on_creation) {
        return;
      }
      schedule_pending_role_restore_tasks_on_runtime(
        runtime,
        runtime_generation,
        steam_device_id,
        std::move(role_restores),
        token,
        assignment_epoch
      );
    }

    void reset_failed_default_roles() {
      auto inherited_handoff = std::move(pending_role_restore_handoff);
      pending_role_restore_handoff = {};
      const auto current_default_reads = current_default_device_id_reads();
      const auto current_default_ids = known_default_device_ids(current_default_reads);
      if (inherited_handoff.assignment_epoch == 0) {
        inherited_handoff = begin_policy_assignment(current_default_ids);
      } else if (!policy_assignment_is_current(inherited_handoff.assignment_epoch)) {
        return;
      }

      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        auto role_restores = normalize_pending_role_restores(
          std::move(inherited_handoff.role_restores),
          current_default_reads
        );
        for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
          const auto role = static_cast<ERole>(x);
          const bool already_queued = std::any_of(
            role_restores.begin(),
            role_restores.end(),
            [&](const auto &role_restore) {
              return role_restore.role == role;
            }
          );
          if (!already_queued) {
            const auto &captured_device_id = captured_default_device_ids[role_index(role)];
            const auto &current_read = current_default_reads[role_index(role)];
            if (!current_read.is_known()) {
              if (!captured_device_id.empty() && !assigned_device_id.empty()) {
                role_restores.push_back({
                  role,
                  captured_device_id,
                  assigned_device_id,
                  {},
                  {},
                  false,
                  true,
                });
              }
              continue;
            }
            const auto &current_id = current_read.id;
            const bool assignment_proves_steam_ownership =
              !assigned_device_id.empty() && current_id == assigned_device_id;
            role_restores.push_back({
              role,
              captured_device_id == current_id ? std::wstring {} : captured_device_id,
              current_id,
              {},
              {},
              false,
              !assignment_proves_steam_ownership,
            });
          }
        }
        if (!role_restores.empty()) {
          start_pending_role_restore_task(
            inherited_handoff.steam_device_id.empty() ?
              assigned_device_id :
              inherited_handoff.steam_device_id,
            std::move(role_restores),
            inherited_handoff.assignment_epoch
          );
        }
        return;
      }
      std::wstring steam_device_id = inherited_handoff.steam_device_id;
      for (const auto &endpoint : catalog.endpoints) {
        if (endpoint.active && endpoint.adapter_name == "Steam Streaming Speakers") {
          steam_device_id = utf_utils::from_utf8(endpoint.id);
          break;
        }
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      if (steam_device_ids.empty()) {
        auto retained_role_restores = normalize_pending_role_restores(
          std::move(inherited_handoff.role_restores),
          current_default_reads
        );
        if (!retained_role_restores.empty()) {
          start_pending_role_restore_task(
            inherited_handoff.steam_device_id,
            std::move(retained_role_restores),
            inherited_handoff.assignment_epoch
          );
          return;
        }
        clear_pending_preferred_restore_for_assignment(
          inherited_handoff.assignment_epoch
        );
        return;
      }
      if (steam_device_id.empty()) {
        steam_device_id = steam_device_ids.front();
      }

      pending_role_restores_t role_restores;
      if (contains_device_id(steam_device_ids, assigned_device_id)) {
        const auto owned_roles = steam_owned_roles_from_snapshot(
          current_default_ids,
          steam_device_ids
        );
        for (const auto &[role, expected_current_id] : owned_roles) {
          const auto &captured_device_id = captured_default_device_ids[role_index(role)];
          role_restores.push_back({
            role,
            contains_device_id(steam_device_ids, captured_device_id) ? std::wstring {} : captured_device_id,
            expected_current_id,
          });
        }
      }

      auto inherited_role_restores = normalize_pending_role_restores(
        std::move(inherited_handoff.role_restores),
        current_default_reads
      );
      for (auto &inherited_restore : inherited_role_restores) {
        bool already_queued = false;
        for (const auto &role_restore : role_restores) {
          if (role_restore.role == inherited_restore.role) {
            already_queued = true;
            break;
          }
        }
        if (!already_queued) {
          role_restores.push_back(std::move(inherited_restore));
        }
      }

      if (role_restores.empty()) {
        clear_pending_preferred_restore_for_assignment(
          inherited_handoff.assignment_epoch
        );
        return;
      }

      // Audio policy calls can block in the Windows audio service, so keep
      // every role-specific restore off the session thread.
      start_pending_role_restore_task(
        steam_device_id,
        std::move(role_restores),
        inherited_handoff.assignment_epoch
      );
    }

    void reset_default_device_impl(const std::wstring &preferred_id) {
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        const auto current_default_reads = current_default_device_id_reads();
        const auto current_default_ids = known_default_device_ids(current_default_reads);
        auto assignment_handoff = begin_policy_assignment(current_default_ids);
        pending_role_restores_t role_restores;
        for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
          const auto role = static_cast<ERole>(x);
          const auto &current_read = current_default_reads[role_index(role)];
          role_restores.push_back({
            role,
            preferred_id,
            current_read.is_known() ? current_read.id : std::wstring {},
            {},
            {},
            false,
            true,
          });
        }
        if (!role_restores.empty()) {
          // No policy write is permitted until a later complete catalog proves
          // each queued snapshot belongs to the Steam render topology.
          start_pending_role_restore_task(
            {},
            std::move(role_restores),
            assignment_handoff.assignment_epoch
          );
        }
        return;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      if (steam_device_ids.empty()) {
        return;
      }
      std::wstring steam_device_id = steam_device_ids.front();
      for (const auto &endpoint : catalog.endpoints) {
        if (endpoint.active && endpoint.adapter_name == "Steam Streaming Speakers") {
          steam_device_id = utf_utils::from_utf8(endpoint.id);
          break;
        }
      }

      const auto current_default_reads = current_default_device_id_reads();
      const auto current_default_ids = known_default_device_ids(current_default_reads);
      auto assignment_handoff = begin_policy_assignment(current_default_ids);
      const auto assignment_epoch = assignment_handoff.assignment_epoch;

      // Repair every role owned by either render half of the Steam full-duplex
      // topology. A role already moved to a non-Steam endpoint belongs to the
      // user and is never included in this assignment.
      const auto steam_roles = steam_owned_roles_from_snapshot(
        current_default_ids,
        steam_device_ids
      );
      const bool any_unavailable_role = std::any_of(
        current_default_reads.begin(),
        current_default_reads.end(),
        [](const auto &read) {
          return !read.is_known();
        }
      );
      if (steam_roles.empty() && !any_unavailable_role) {
        clear_pending_preferred_restore_for_assignment(assignment_epoch);
        return;
      }

      std::vector<std::wstring> preferred_candidates;
      if (auto configured_id = configured_non_steam_render_endpoint(catalog)) {
        preferred_candidates.push_back(*configured_id);
      }
      if (!preferred_id.empty() && !contains_device_id(steam_device_ids, preferred_id)) {
        preferred_candidates.push_back(preferred_id);
      }
      if (auto pending_id = pending_preferred_restore_id();
          pending_id && !contains_device_id(steam_device_ids, *pending_id)) {
        preferred_candidates.push_back(*pending_id);
      }

      std::wstring desired_preferred_id;
      if (!preferred_candidates.empty()) {
        desired_preferred_id = preferred_candidates.front();
      }

      // Keep policy writes and retry waits off the caller. Each queued role is
      // bound to the exact Steam ID from the single snapshot above; only a
      // successful write plus matching readback can commit a new endpoint.
      pending_role_restores_t role_restores;
      role_restores.reserve(steam_roles.size());
      for (const auto &[role, expected_current_id] : steam_roles) {
        role_restores.push_back({
          role,
          desired_preferred_id,
          expected_current_id,
        });
      }
      for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
        const auto role = static_cast<ERole>(x);
        if (current_default_reads[role_index(role)].is_known()) {
          continue;
        }
        role_restores.push_back({
          role,
          desired_preferred_id,
          {},
          {},
          {},
          false,
          true,
        });
      }
      if (role_restores.empty()) {
        clear_pending_preferred_restore_for_assignment(assignment_epoch);
        return;
      }
      start_pending_role_restore_task(
        steam_device_id,
        std::move(role_restores),
        assignment_epoch
      );
    }

    enum class reset_result_e {
      success,       ///< A non-Steam device was set as default
      no_device,     ///< No non-Steam device is available yet (retriable)
      transient,     ///< Retry after a bounded backoff without waiting for arrival
      inactive,      ///< A superseded background worker must not write
    };

    enum class role_restore_result_e {
      restored,
      fallback_committed,
      released_external,
      retry_catalog,
      wait_topology,
      retry_policy,
      preferred_policy_failure,
      superseded,
    };

    static void clear_pending_preferred_restore_for_worker(
      const std::wstring &preferred_id,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (preferred_id.empty()) {
        return;
      }

      std::scoped_lock lock(pending_restore_mutex_ref());
      if (pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        clear_pending_preferred_restore(preferred_id);
      }
    }

    static bool update_pending_role_restore_for_worker(
      const pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      bool publish_expected_endpoint = true
    ) {
      bool start_observer = false;
      bool updated = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
          return false;
        }

        for (auto &published_restore : pending_role_restores_ref()) {
          if (published_restore.role == role_restore.role) {
            published_restore = role_restore;
            if (publish_expected_endpoint) {
              start_observer = rebind_policy_role_intent_locked(
                role_restore.role,
                role_restore.expected_current_id,
                false
              );
            }
            updated = true;
            break;
          }
        }
      }
      if (start_observer) {
        start_policy_write_observer(role_restore.role);
      }
      return updated;
    }

    static bool commit_pending_role_restore_for_worker(
      const pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      const std::wstring &committed_expected_id,
      const std::wstring &inflight_target_id
    ) {
      bool start_observer = false;
      bool committed = false;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
          return false;
        }

        const auto &active_id =
          policy_assignment_desired_ids_ref()[role_index(role_restore.role)];
        if (active_id != committed_expected_id && active_id != inflight_target_id) {
          return false;
        }
        for (auto &published_restore : pending_role_restores_ref()) {
          if (published_restore.role == role_restore.role) {
            published_restore = role_restore;
            start_observer = rebind_policy_role_intent_locked(
              role_restore.role,
              role_restore.expected_current_id,
              false
            );
            committed = true;
            break;
          }
        }
      }
      if (start_observer) {
        start_policy_write_observer(role_restore.role);
      }
      return committed;
    }

    static void clear_pending_role_restore_for_worker(
      const pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return;
      }

      auto &published_restores = pending_role_restores_ref();
      for (auto it = published_restores.begin(); it != published_restores.end(); ++it) {
        if (it->role == role_restore.role) {
          published_restores.erase(it);
          break;
        }
      }
      if (published_restores.empty()) {
        pending_restore_steam_device_id_ref().clear();
      }

      if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
        clear_pending_preferred_restore(role_restore.preferred_id);
      }
    }

    static void clear_pending_role_restores_for_worker(
      const pending_role_restores_t &role_restores,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      std::scoped_lock lock(pending_restore_mutex_ref());
      if (!pending_restore_worker_owns_state_locked(token, assignment_epoch)) {
        return;
      }

      pending_role_restores_ref().clear();
      pending_restore_steam_device_id_ref().clear();
      for (const auto &role_restore : role_restores) {
        if (role_restore.role == eConsole && !role_restore.preferred_id.empty()) {
          clear_pending_preferred_restore(role_restore.preferred_id);
        }
      }
    }

    role_restore_result_e complete_inflight_role_restore(
      pending_role_restore_t &role_restore,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      const auto committed_expected_id = role_restore.expected_current_id;
      const auto inflight_target_id = role_restore.inflight_target_id;
      const auto receipt_id = role_restore.inflight_write_receipt_id;
      const auto readback = current_default_device_id(role_restore.role);
      const auto readback_id = readback.is_known() ?
        std::optional<std::string> {utf_utils::to_utf8(readback.id.c_str())} :
        std::nullopt;
      const auto completion = ::audio::policy::complete_fallback_role_transition(
        utf_utils::to_utf8(committed_expected_id.c_str()),
        utf_utils::to_utf8(inflight_target_id.c_str()),
        true,
        readback_id
      );
      if (completion.action == ::audio::policy::fallback_commit_action_e::retry_committed) {
        if (!policy_assignment_role_is_current(
              assignment_epoch,
              role_restore.role,
              committed_expected_id)) {
          mark_policy_write_receipt_hazard(role_restore.role, receipt_id);
          const bool worker_active =
            pending_restore_worker_is_active(token, assignment_epoch);
          const bool role_assignment_still_current =
            worker_active &&
            policy_assignment_role_is_current(
              assignment_epoch,
              role_restore.role,
              committed_expected_id
            );
          const auto action = ::audio::policy::worker_role_write_action(
            worker_active,
            role_assignment_still_current
          );
          if (action == ::audio::policy::worker_role_write_action_e::stop_worker) {
            return role_restore_result_e::superseded;
          }
          if (action == ::audio::policy::worker_role_write_action_e::release_external) {
            role_restore.expected_current_id.clear();
            role_restore.inflight_target_id.clear();
            role_restore.inflight_is_fallback = false;
            role_restore.inflight_write_receipt_id = 0;
            clear_pending_role_restore_for_worker(
              role_restore,
              token,
              assignment_epoch
            );
            return role_restore_result_e::released_external;
          }
        }
        update_pending_role_restore_for_worker(
          role_restore,
          token,
          assignment_epoch,
          false
        );
        return role_restore_result_e::retry_policy;
      }
      if (completion.action == ::audio::policy::fallback_commit_action_e::release_external) {
        const auto active_id = adopt_external_policy_assignment_role_for_worker(
          token,
          assignment_epoch,
          role_restore.role,
          committed_expected_id,
          inflight_target_id,
          readback.id
        );
        mark_policy_write_receipt_hazard(role_restore.role, receipt_id);
        if (active_id.action == worker_external_adoption_action_e::inactive) {
          return role_restore_result_e::superseded;
        }
        if (active_id.action ==
            worker_external_adoption_action_e::retry_stale_receipt) {
          update_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch,
            false
          );
          return role_restore_result_e::retry_policy;
        }
        role_restore.expected_current_id.clear();
        role_restore.inflight_target_id.clear();
        role_restore.inflight_is_fallback = false;
        role_restore.inflight_write_receipt_id = 0;
        clear_pending_role_restore_for_worker(
          role_restore,
          token,
          assignment_epoch
        );
        return role_restore_result_e::released_external;
      }

      const bool committed_fallback = role_restore.inflight_is_fallback;
      role_restore.expected_current_id = inflight_target_id;
      role_restore.inflight_target_id.clear();
      role_restore.inflight_is_fallback = false;
      role_restore.inflight_write_receipt_id = 0;
      if (!commit_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch,
            committed_expected_id,
            inflight_target_id)) {
        mark_policy_write_receipt_hazard(role_restore.role, receipt_id);
        clear_pending_role_restore_for_worker(
          role_restore,
          token,
          assignment_epoch
        );
        return pending_restore_worker_is_active(token, assignment_epoch) ?
          role_restore_result_e::released_external :
          role_restore_result_e::superseded;
      }
      confirm_policy_write_receipt(role_restore.role, receipt_id);
      return committed_fallback ?
        role_restore_result_e::fallback_committed :
        role_restore_result_e::restored;
    }

    role_restore_result_e try_restore_pending_role(
      pending_role_restore_t &role_restore,
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return role_restore_result_e::superseded;
      }

      if (!role_restore.inflight_target_id.empty()) {
        return complete_inflight_role_restore(role_restore, token, assignment_epoch);
      }

      const auto commit_pending_exact_readback =
        [&](const std::wstring &live_id) -> std::optional<role_restore_result_e> {
          const auto receipt_id = pending_policy_write_receipt_for_target(
            role_restore.role,
            assignment_epoch,
            live_id
          );
          if (!receipt_id) {
            return std::nullopt;
          }
          const auto committed_expected_id = role_restore.expected_current_id;
          const bool restored_preferred = live_id == role_restore.preferred_id;
          role_restore.expected_current_id = live_id;
          if (!commit_pending_role_restore_for_worker(
                role_restore,
                token,
                assignment_epoch,
                committed_expected_id,
                live_id)) {
            mark_policy_write_receipt_hazard(role_restore.role, *receipt_id);
            return pending_restore_worker_is_active(token, assignment_epoch) ?
              role_restore_result_e::released_external :
              role_restore_result_e::superseded;
          }
          confirm_policy_write_receipt(role_restore.role, *receipt_id);
          return restored_preferred ?
            role_restore_result_e::restored :
            role_restore_result_e::fallback_committed;
        };

      const auto initial_live = current_default_device_id(role_restore.role);
      if (!initial_live.is_known()) {
        return role_restore_result_e::retry_catalog;
      }
      if (initial_live.id != role_restore.expected_current_id) {
        if (const auto pending_commit =
              commit_pending_exact_readback(initial_live.id)) {
          return *pending_commit;
        }
        const auto adoption = adopt_external_policy_assignment_role_for_worker(
          token,
          assignment_epoch,
          role_restore.role,
          role_restore.expected_current_id,
          {},
          initial_live.id
        );
        if (adoption.action == worker_external_adoption_action_e::inactive) {
          return role_restore_result_e::superseded;
        }
        if (adoption.action ==
            worker_external_adoption_action_e::retry_stale_receipt) {
          return role_restore_result_e::retry_policy;
        }
        return role_restore_result_e::released_external;
      }
      if (role_restore.preferred_id.empty()) {
        return role_restore_result_e::wait_topology;
      }

      auto match_list = preferred_device_match_list(role_restore.preferred_id);
      if (!match_list) {
        return role_restore_result_e::wait_topology;
      }

      auto matched = find_device_id(*match_list);
      if (!matched) {
        return role_restore_result_e::wait_topology;
      }
      const auto catalog = active_render_endpoint_catalog();
      if (!catalog.complete) {
        return role_restore_result_e::retry_catalog;
      }
      if (!::audio::policy::is_eligible_non_steam_fallback(
            catalog,
            utf_utils::to_utf8(matched->second.c_str()))) {
        return role_restore_result_e::wait_topology;
      }

      // The user may have selected another device while the preferred endpoint
      // was being re-enumerated. Never write over that newer choice.
      const auto guarded_live = current_default_device_id(role_restore.role);
      if (!guarded_live.is_known()) {
        return role_restore_result_e::retry_catalog;
      }
      if (guarded_live.id != role_restore.expected_current_id) {
        if (const auto pending_commit =
              commit_pending_exact_readback(guarded_live.id)) {
          return *pending_commit;
        }
        const auto adoption = adopt_external_policy_assignment_role_for_worker(
          token,
          assignment_epoch,
          role_restore.role,
          role_restore.expected_current_id,
          {},
          guarded_live.id
        );
        if (adoption.action == worker_external_adoption_action_e::inactive) {
          return role_restore_result_e::superseded;
        }
        if (adoption.action ==
            worker_external_adoption_action_e::retry_stale_receipt) {
          return role_restore_result_e::retry_policy;
        }
        return role_restore_result_e::released_external;
      }
      role_restore.inflight_target_id = matched->second;
      role_restore.inflight_is_fallback = false;
      const auto result = set_default_endpoint_for_worker_before_publishing(
        stop_token,
        token,
        assignment_epoch,
        role_restore.role,
        role_restore.expected_current_id,
        matched->second
      );
      if (result.action == ::audio::policy::worker_role_write_action_e::stop_worker) {
        return role_restore_result_e::superseded;
      }
      if (result.action == ::audio::policy::worker_role_write_action_e::release_external) {
        role_restore.expected_current_id.clear();
        role_restore.inflight_target_id.clear();
        role_restore.inflight_is_fallback = false;
        role_restore.inflight_write_receipt_id = 0;
        clear_pending_role_restore_for_worker(
          role_restore,
          token,
          assignment_epoch
        );
        return role_restore_result_e::released_external;
      }
      if (result.read_unavailable) {
        role_restore.inflight_target_id.clear();
        role_restore.inflight_is_fallback = false;
        role_restore.inflight_write_receipt_id = 0;
        return role_restore_result_e::retry_catalog;
      }
      role_restore.inflight_write_receipt_id = result.receipt_id;
      const auto status = result.status;
      if (FAILED(status)) {
        role_restore.inflight_target_id.clear();
        role_restore.inflight_write_receipt_id = 0;
        role_restore.failed_preferred_target_id = matched->second;
        if (!update_pending_role_restore_for_worker(
              role_restore,
              token,
              assignment_epoch,
              false)) {
          return role_restore_result_e::superseded;
        }
        BOOST_LOG(warning) << "Couldn't restore captured audio endpoint for role ["sv
                           << static_cast<int>(role_restore.role) << "]: 0x"sv
                           << util::hex(status).to_string_view();
        return role_restore_result_e::preferred_policy_failure;
      }

      role_restore.failed_preferred_target_id.clear();
      const auto completion_result = complete_inflight_role_restore(
        role_restore,
        token,
        assignment_epoch
      );
      if (completion_result != role_restore_result_e::restored) {
        return completion_result;
      }
      if (role_restore.expected_current_id != role_restore.preferred_id) {
        BOOST_LOG(info) << "Restored captured audio endpoint via re-enumerated device for role ["sv
                        << static_cast<int>(role_restore.role) << ']';
      } else {
        BOOST_LOG(info) << "Restored captured audio endpoint for role ["sv
                        << static_cast<int>(role_restore.role) << ']';
      }
      return role_restore_result_e::restored;
    }

    reset_result_e try_reset_pending_roles_from_steam(
      const std::vector<ERole> &fallback_requested_roles,
      pending_role_restores_t &role_restores,
      const std::stop_token &stop_token,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch
    ) {
      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return reset_result_e::inactive;
      }

      const auto catalog = active_render_endpoint_catalog();
      const auto catalog_action = ::audio::policy::fallback_catalog_action(catalog);
      if (catalog_action == ::audio::policy::fallback_catalog_action_e::poll_after_backoff) {
        return reset_result_e::transient;
      }
      if (catalog_action == ::audio::policy::fallback_catalog_action_e::wait_for_arrival) {
        return reset_result_e::no_device;
      }
      const auto steam_device_ids = steam_render_device_ids(catalog);
      std::vector<std::size_t> steam_role_indexes;
      bool retry_after_backoff = false;
      for (std::size_t i = 0; i < role_restores.size(); ++i) {
        const auto &role_restore = role_restores[i];
        const bool fallback_requested =
          std::find(
            fallback_requested_roles.begin(),
            fallback_requested_roles.end(),
            role_restore.role
          ) != fallback_requested_roles.end();
        if (::audio::policy::should_attempt_role_fallback(
              fallback_requested,
              !role_restore.inflight_target_id.empty()) &&
            contains_device_id(steam_device_ids, role_restore.expected_current_id)) {
          const auto current_read = current_default_device_id(role_restore.role);
          if (!current_read.is_known()) {
            retry_after_backoff = true;
          } else if (current_read.id == role_restore.expected_current_id) {
            steam_role_indexes.push_back(i);
          }
        }
      }
      if (steam_role_indexes.empty()) {
        return retry_after_backoff ? reset_result_e::transient : reset_result_e::success;
      }

      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
        return reset_result_e::inactive;
      }

      for (const auto index : steam_role_indexes) {
        if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch)) {
          return reset_result_e::inactive;
        }

        auto &role_restore = role_restores[index];
        std::vector<std::string> excluded_ids;
        if (!role_restore.failed_preferred_target_id.empty()) {
          excluded_ids.push_back(
            utf_utils::to_utf8(role_restore.failed_preferred_target_id.c_str())
          );
        }
        const auto selected_fallback = ::audio::policy::select_eligible_non_steam_render_endpoint(
          catalog,
          {},
          excluded_ids
        );
        if (!selected_fallback) {
          retry_after_backoff = true;
          continue;
        }
        const auto selected_fallback_id = utf_utils::from_utf8(*selected_fallback);
        const auto live_read = current_default_device_id(role_restore.role);
        if (!live_read.is_known()) {
          retry_after_backoff = true;
          continue;
        }
        const auto &live_id = live_read.id;
        const auto transition = ::audio::policy::plan_fallback_role_transition(
          catalog,
          utf_utils::to_utf8(role_restore.expected_current_id.c_str()),
          utf_utils::to_utf8(live_id.c_str()),
          *selected_fallback
        );
        if (transition.action == ::audio::policy::fallback_role_action_e::keep_steam_for_retry) {
          retry_after_backoff = true;
          continue;
        }

        if (transition.action == ::audio::policy::fallback_role_action_e::adopt_and_retire) {
          const auto adoption = adopt_external_policy_assignment_role_for_worker(
            token,
            assignment_epoch,
            role_restore.role,
            role_restore.expected_current_id,
            {},
            live_id
          );
          if (adoption.action == worker_external_adoption_action_e::inactive) {
            return reset_result_e::inactive;
          }
          if (adoption.action ==
              worker_external_adoption_action_e::retry_stale_receipt) {
            retry_after_backoff = true;
            continue;
          }
          role_restore.expected_current_id.clear();
          clear_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch
          );
          continue;
        }

        // The endpoint can change after the snapshot above. The exact Steam ID
        // is the final provenance guard; a different live endpoint is adopted
        // and retires this record instead of being claimed as our fallback.
        const auto guarded_live = current_default_device_id(role_restore.role);
        if (!guarded_live.is_known()) {
          retry_after_backoff = true;
          continue;
        }
        if (guarded_live.id != role_restore.expected_current_id) {
          const auto adoption = adopt_external_policy_assignment_role_for_worker(
            token,
            assignment_epoch,
            role_restore.role,
            role_restore.expected_current_id,
            {},
            guarded_live.id
          );
          if (adoption.action == worker_external_adoption_action_e::inactive) {
            return reset_result_e::inactive;
          }
          if (adoption.action ==
              worker_external_adoption_action_e::retry_stale_receipt) {
            retry_after_backoff = true;
            continue;
          }
          role_restore.expected_current_id.clear();
          clear_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch
          );
          continue;
        }

        role_restore.inflight_target_id = selected_fallback_id;
        role_restore.inflight_is_fallback = true;
        const auto result = set_default_endpoint_for_worker_before_publishing(
          stop_token,
          token,
          assignment_epoch,
          role_restore.role,
          role_restore.expected_current_id,
          selected_fallback_id
        );
        if (result.action == ::audio::policy::worker_role_write_action_e::stop_worker) {
          return reset_result_e::inactive;
        }
        if (result.action == ::audio::policy::worker_role_write_action_e::release_external) {
          role_restore.expected_current_id.clear();
          role_restore.inflight_target_id.clear();
          role_restore.inflight_is_fallback = false;
          role_restore.inflight_write_receipt_id = 0;
          clear_pending_role_restore_for_worker(
            role_restore,
            token,
            assignment_epoch
          );
          continue;
        }
        if (result.read_unavailable) {
          role_restore.inflight_target_id.clear();
          role_restore.inflight_is_fallback = false;
          role_restore.inflight_write_receipt_id = 0;
          retry_after_backoff = true;
          continue;
        }
        role_restore.inflight_write_receipt_id = result.receipt_id;
        const auto status = result.status;
        if (FAILED(status)) {
          role_restore.inflight_target_id.clear();
          role_restore.inflight_is_fallback = false;
          role_restore.inflight_write_receipt_id = 0;
          BOOST_LOG(warning) << "Couldn't set new default audio endpoint for role ["sv
                             << static_cast<int>(role_restore.role) << "]: 0x"sv
                             << util::hex(status).to_string_view();
          retry_after_backoff = true;
          continue;
        }

        // Only a successful write followed by an exact target readback commits
        // fallback ownership. A different readback is external and retires the
        // role; an unavailable readback leaves the old committed Steam guard
        // intact for a bounded retry.
        const auto completion_result = complete_inflight_role_restore(
          role_restore,
          token,
          assignment_epoch
        );
        if (completion_result == role_restore_result_e::superseded) {
          return reset_result_e::inactive;
        }
        if (completion_result == role_restore_result_e::retry_policy) {
          retry_after_backoff = true;
        }
      }

      return retry_after_backoff ? reset_result_e::transient : reset_result_e::success;
    }

    bool run_pending_role_restore_task(
      std::stop_token stop_token,
      const std::wstring &steam_device_id,
      pending_role_restores_t &role_restores,
      const pending_restore_token_t &token,
      std::uint64_t assignment_epoch,
      bool &retry_fallback_reset
    ) {
      if (::audio::policy::granted_restore_phase_action(
            stop_token.stop_requested(),
            pending_restore_worker_can_write(
              stop_token,
              token,
              assignment_epoch
            )) == ::audio::policy::granted_restore_phase_action_e::cancel) {
        return false;
      }
      device_arrival_notification_t arrival_notifier(steam_device_id);
      HANDLE cancel_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (!cancel_event) {
        BOOST_LOG(warning) << "Failed to create background restore cancellation event"sv;
      }
      auto cancel_event_guard = util::fail_guard([&]() {
        if (cancel_event) {
          CloseHandle(cancel_event);
        }
      });
      std::optional<std::stop_callback<std::function<void()>>> stop_callback;
      if (cancel_event) {
        stop_callback.emplace(stop_token, [cancel_event]() {
          SetEvent(cancel_event);
        });
      }

      auto reg_status = device_enum->RegisterEndpointNotificationCallback(&arrival_notifier);
      const bool have_notifications = SUCCEEDED(reg_status);
      if (!have_notifications) {
        BOOST_LOG(warning) << "Failed to register device arrival notification for background restore: "sv
                           << util::hex(reg_status).to_string_view();
      }
      auto unreg_guard = util::fail_guard([&]() {
        if (have_notifications) {
          device_enum->UnregisterEndpointNotificationCallback(&arrival_notifier);
        }
      });

      if (!pending_restore_worker_can_write(stop_token, token, assignment_epoch) ||
          role_restores.empty()) {
        return false;
      }
        const auto catalog = active_render_endpoint_catalog();
        if (!catalog.complete) {
          // Discovery failures are transient and do not necessarily emit an
          // endpoint-arrival notification. Keep polling after this bounded
          // wait even when notifications are registered successfully.
          retry_fallback_reset = true;
          arrival_notifier.wait(cancel_event, 1000);
          return pending_restore_worker_can_write(
            stop_token,
            token,
            assignment_epoch
          );
        }
        const auto steam_device_ids = steam_render_device_ids(catalog);
        for (auto it = role_restores.begin(); it != role_restores.end();) {
          if (!it->ownership_unconfirmed) {
            ++it;
            continue;
          }
          const auto live_read = current_default_device_id(it->role);
          const auto live_id = live_read.is_known() ?
            std::optional<std::string> {utf_utils::to_utf8(live_read.id.c_str())} :
            std::nullopt;
          const auto ownership_action = ::audio::policy::classify_pending_role_ownership(
            catalog,
            utf_utils::to_utf8(it->expected_current_id.c_str()),
            live_id
          );
          if (ownership_action == ::audio::policy::pending_role_ownership_action_e::poll_catalog) {
            retry_fallback_reset = true;
            ++it;
            continue;
          }
          if (ownership_action == ::audio::policy::pending_role_ownership_action_e::confirm_steam_owned) {
            it->expected_current_id = live_read.id;
            it->ownership_unconfirmed = false;
            if (!update_pending_role_restore_for_worker(
                  *it,
                  token,
                  assignment_epoch)) {
              return false;
            }
            ++it;
            continue;
          }
          const auto adoption = adopt_external_policy_assignment_role_for_worker(
            token,
            assignment_epoch,
            it->role,
            it->expected_current_id,
            {},
            live_read.id
          );
          if (adoption.action == worker_external_adoption_action_e::inactive) {
            return false;
          }
          if (adoption.action ==
              worker_external_adoption_action_e::retry_stale_receipt) {
            retry_fallback_reset = true;
            ++it;
            continue;
          }
          clear_pending_role_restore_for_worker(*it, token, assignment_epoch);
          it = role_restores.erase(it);
        }
        if (role_restores.empty()) {
          return false;
        }
        std::vector<ERole> fallback_requested_roles;
        for (auto it = role_restores.begin(); it != role_restores.end();) {
          const auto result = try_restore_pending_role(
            *it,
            stop_token,
            token,
            assignment_epoch
          );
          if (result == role_restore_result_e::restored ||
              result == role_restore_result_e::released_external) {
            clear_pending_role_restore_for_worker(
              *it,
              token,
              assignment_epoch
            );
            it = role_restores.erase(it);
            continue;
          }
          if (result == role_restore_result_e::superseded) {
            return false;
          }
          if (result == role_restore_result_e::retry_catalog ||
              result == role_restore_result_e::retry_policy) {
            retry_fallback_reset = true;
            ++it;
            continue;
          }
          if (result == role_restore_result_e::preferred_policy_failure) {
            retry_fallback_reset = true;
            if (contains_device_id(steam_device_ids, it->expected_current_id)) {
              fallback_requested_roles.push_back(it->role);
            }
            ++it;
            continue;
          }
          if (result == role_restore_result_e::wait_topology) {
            if (contains_device_id(steam_device_ids, it->expected_current_id)) {
              fallback_requested_roles.push_back(it->role);
            }
          }
          ++it;
        }

        if (!fallback_requested_roles.empty() && retry_fallback_reset) {
          const auto fallback_result = try_reset_pending_roles_from_steam(
            fallback_requested_roles,
            role_restores,
            stop_token,
            token,
            assignment_epoch
          );
          if (fallback_result == reset_result_e::inactive) {
            return false;
          }
          if (fallback_result == reset_result_e::transient) {
            retry_fallback_reset = true;
          }
          if (fallback_result == reset_result_e::no_device) {
            // A complete catalog proves there is no eligible non-Steam target.
            // Suppress retries until an endpoint-arrival notification instead
            // of issuing the same impossible assignment every second.
            retry_fallback_reset = false;
          }
        }

        // Roles without a captured endpoint need only the immediate fallback.
        // Any role with a captured endpoint stays queued until that endpoint
        // returns, but only while its expected fallback remains selected.
        for (auto it = role_restores.begin(); it != role_restores.end();) {
          if (!it->ownership_unconfirmed &&
              (it->expected_current_id.empty() ||
               (it->preferred_id.empty() &&
                !contains_device_id(steam_device_ids, it->expected_current_id)))) {
            clear_pending_role_restore_for_worker(
              *it,
              token,
              assignment_epoch
            );
            it = role_restores.erase(it);
          } else {
            ++it;
          }
        }
        if (role_restores.empty()) {
          return false;
        }

        // If notification registration failed, use the timed wait as a polling
        // backoff so a fallback endpoint that appears later is still retried.
        if (arrival_notifier.wait(cancel_event, 1000) || !have_notifications) {
          retry_fallback_reset = true;
        }
      return pending_restore_worker_can_write(
               stop_token,
               token,
               assignment_epoch
             ) && !role_restores.empty();
    }

  public:

    static void restart_policy_background_work_for_runtime(
      const std::shared_ptr<
        ::audio::policy::fixed_role_lane_coordinator_t
      > &runtime,
      std::uint64_t runtime_generation
    ) {
      std::vector<ERole> roles;
      pending_role_restores_t role_restores;
      pending_restore_token_t restore_token;
      std::wstring steam_device_id;
      std::uint64_t assignment_epoch = 0;
      {
        std::scoped_lock lock(pending_restore_mutex_ref());
        if (policy_assignment_rollover_in_progress_ref()) {
          return;
        }
        const auto &ledgers = superseded_write_ledgers_ref();
        for (std::size_t index = 0; index < ledgers.size(); ++index) {
          if (!ledgers[index].receipts.empty()) {
            roles.push_back(static_cast<ERole>(index));
          }
        }
        const auto &token = pending_restore_token_ref();
        if (token && token->load(std::memory_order_acquire) &&
            !pending_role_restores_ref().empty()) {
          restore_token = token;
          role_restores = pending_role_restores_ref();
          steam_device_id = pending_restore_steam_device_id_ref();
          assignment_epoch = policy_assignment_epoch_ref();
        }
      }
      bool replay_succeeded = true;
      for (const auto role : roles) {
        replay_succeeded = start_policy_write_observer(role, runtime) &&
                           replay_succeeded;
      }
      if (restore_token && !role_restores.empty() && assignment_epoch != 0) {
        replay_succeeded = schedule_pending_role_restore_tasks_on_runtime(
                             runtime,
                             runtime_generation,
                             steam_device_id,
                             std::move(role_restores),
                             restore_token,
                             assignment_epoch
                           ) && replay_succeeded;
      }
      if (!replay_succeeded) {
        throw std::runtime_error(
          "failed to replay retained audio-policy background work"
        );
      }
      BOOST_LOG(debug) << "Replayed retained audio policy background work for role-lane generation ["sv
                       << runtime_generation << ']';
    }

    /**
     * @brief Installs the Steam Streaming Speakers driver, if present.
     * @return `true` if installation was successful.
     */
    bool install_steam_audio_drivers() {
#ifdef STEAM_DRIVER_SUBDIR
      // MinGW's libnewdev.a is missing DiInstallDriverW() even though the headers have it,
      // so we have to load it at runtime. It's Vista or later, so it will always be available.
      auto newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!newdev) {
        BOOST_LOG(error) << "newdev.dll failed to load"sv;
        return false;
      }
      auto fg = util::fail_guard([newdev]() {
        FreeLibrary(newdev);
      });

      auto fn_DiInstallDriverW = (decltype(DiInstallDriverW) *) GetProcAddress(newdev, "DiInstallDriverW");
      if (!fn_DiInstallDriverW) {
        BOOST_LOG(error) << "DiInstallDriverW() is missing"sv;
        return false;
      }

      // Capture each role separately because installing the driver may replace
      // only some of the current policy endpoints.
      const auto old_default_reads = current_default_device_id_reads();
      const auto old_default_ids = known_default_device_ids(old_default_reads);

      // Install the Steam Streaming Speakers driver
      WCHAR driver_path[MAX_PATH] = {};
      ExpandEnvironmentStringsW(STEAM_AUDIO_DRIVER_PATH, driver_path, ARRAYSIZE(driver_path));
      if (fn_DiInstallDriverW(nullptr, driver_path, 0, nullptr)) {
        BOOST_LOG(info) << "Successfully installed Steam Streaming Speakers"sv;

        // Wait for 5 seconds to allow the audio subsystem to reconfigure things before
        // modifying the default audio device or enumerating devices again.
        Sleep(5000);

        // Restore every role Windows moved to either render half of the Steam
        // topology through the readback-verified worker. Incomplete discovery
        // queues provisional snapshots but permits no writes until a later
        // complete catalog proves the live role is still Steam-owned.
        const auto catalog = active_render_endpoint_catalog();
        const auto current_default_reads = current_default_device_id_reads();
        const auto current_default_ids = known_default_device_ids(current_default_reads);
        auto assignment_handoff = begin_policy_assignment(current_default_ids);
        const auto assignment_epoch = assignment_handoff.assignment_epoch;
        pending_role_restores_t role_restores;
        std::wstring steam_device_id;

        std::vector<std::string> current_ids_utf8;
        std::vector<std::string> old_ids_utf8;
        current_ids_utf8.reserve(current_default_ids.size());
        old_ids_utf8.reserve(old_default_ids.size());
        for (std::size_t index = 0; index < current_default_ids.size(); ++index) {
          current_ids_utf8.push_back(utf_utils::to_utf8(current_default_ids[index].c_str()));
          old_ids_utf8.push_back(utf_utils::to_utf8(old_default_ids[index].c_str()));
        }

        if (!catalog.complete) {
          for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
            const auto role = static_cast<ERole>(x);
            const auto &old_id = old_default_ids[role_index(role)];
            const auto &current_read = current_default_reads[role_index(role)];
            if (old_id.empty() &&
                !::audio::policy::should_queue_post_install_unknown_role(
                  current_read.is_known(),
                  {})) {
              continue;
            }
            const auto current_id = current_read.is_known() ?
              current_read.id : std::wstring {};
            role_restores.push_back({
              role,
              old_id == current_id ? std::wstring {} : old_id,
              current_id,
              {},
              {},
              false,
              true,
            });
          }
        } else {
          const auto steam_device_ids = steam_render_device_ids(catalog);
          if (!steam_device_ids.empty()) {
            steam_device_id = steam_device_ids.front();
          }
          for (const auto &endpoint : catalog.endpoints) {
            if (endpoint.active && endpoint.adapter_name == "Steam Streaming Speakers") {
              steam_device_id = utf_utils::from_utf8(endpoint.id);
              break;
            }
          }

          const auto restores = ::audio::policy::plan_steam_role_restores(
            catalog,
            current_ids_utf8,
            old_ids_utf8
          );
          const auto owned_roles = ::audio::policy::steam_owned_roles_from_snapshot(
            current_ids_utf8,
            catalog.steam_endpoint_ids
          );
          for (const auto &owned_role : owned_roles) {
            std::wstring preferred_id;
            const auto planned = std::find_if(
              restores.begin(),
              restores.end(),
              [&](const auto &restore) {
                return restore.role_index == owned_role.role_index;
              }
            );
            if (planned != restores.end()) {
              preferred_id = utf_utils::from_utf8(planned->target_id);
            }
            role_restores.push_back({
              static_cast<ERole>(owned_role.role_index),
              std::move(preferred_id),
              utf_utils::from_utf8(owned_role.expected_current_id),
            });
          }
          for (int x = 0; x < static_cast<int>(ERole_enum_count); ++x) {
            const auto role = static_cast<ERole>(x);
            if (current_default_reads[role_index(role)].is_known()) {
              continue;
            }
            const auto &old_id = old_default_ids[role_index(role)];
            if (!::audio::policy::should_queue_post_install_unknown_role(
                  false,
                  utf_utils::to_utf8(old_id.c_str()))) {
              continue;
            }
            role_restores.push_back({
              role,
              old_id,
              {},
              {},
              {},
              false,
              true,
            });
          }
        }

        if (!role_restores.empty()) {
          start_pending_role_restore_task(
            steam_device_id,
            std::move(role_restores),
            assignment_epoch
          );
        }

        return true;
      } else {
        auto err = GetLastError();
        switch (err) {
          case ERROR_ACCESS_DENIED:
            BOOST_LOG(warning) << "Administrator privileges are required to install Steam Streaming Speakers"sv;
            break;
          case ERROR_FILE_NOT_FOUND:
          case ERROR_PATH_NOT_FOUND:
            BOOST_LOG(info) << "Steam audio drivers not found. This is expected if you don't have Steam installed."sv;
            break;
          default:
            BOOST_LOG(warning) << "Failed to install Steam audio drivers: "sv << err;
            break;
        }

        return false;
      }
#else
      BOOST_LOG(warning) << "Unable to install Steam Streaming Speakers on unknown architecture"sv;
      return false;
#endif
    }

    int init() {
      const auto status = CoCreateInstance(
        CLSID_MMDeviceEnumerator,
        nullptr,
        CLSCTX_ALL,
        IID_IMMDeviceEnumerator,
        (void **) &device_enum
      );

      if (FAILED(status)) {
        BOOST_LOG(error) << "Couldn't create Device Enumerator: [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }

      return 0;
    }

    ~audio_control_t() override {
      if (owns_policy_role_lanes_) {
        shutdown_policy_role_lane_runtime();
      }
    }

    audio::device_enum_t device_enum;
    role_device_ids_t captured_default_device_ids;
    pending_role_restore_handoff_t pending_role_restore_handoff;
    std::string assigned_sink;
    std::wstring assigned_device_id;

  private:
    bool owns_policy_role_lanes_ = false;
  };

  static void restart_policy_background_work(
    const std::shared_ptr<::audio::policy::fixed_role_lane_coordinator_t> &runtime,
    std::uint64_t generation
  ) {
    audio_control_t::restart_policy_background_work_for_runtime(
      runtime,
      generation
    );
  }
}  // namespace platf::audio

namespace platf {

  // It's not big enough to justify it's own source file :/
  namespace dxgi {
    int init();
  }

  std::unique_ptr<audio_control_t> audio_control() {
    auto control = std::make_unique<audio::audio_control_t>(true);

    if (control->init()) {
      return nullptr;
    }

    // Install Steam Streaming Speakers if needed. We do this during audio_control() to ensure
    // the sink information returned includes the new Steam Streaming Speakers device.
    if (config::audio.install_steam_drivers && !control->find_device_id(control->match_steam_speakers())) {
      // This is best effort. Don't fail if it doesn't work.
      control->install_steam_audio_drivers();
    }

    return control;
  }

  std::unique_ptr<deinit_t> init() {
    if (dxgi::init()) {
      return nullptr;
    }

    // Initialize COM
    auto co_init = std::make_unique<platf::audio::co_init_t>(true);

    // If Steam Streaming Speakers are currently the default audio device,
    // change the default to something else (if another device is available).
    audio::audio_control_t audio_ctrl;
    if (audio_ctrl.init() == 0) {
      audio_ctrl.reset_default_device_no_wait();
    }

    return co_init;
  }
}  // namespace platf

/**
 * @file tools/audio_policy_helper.cpp
 * @brief One-shot COM process for Windows default render endpoint policy.
 *
 * This executable intentionally has no service/runtime dependencies. It reads
 * one bounded request from stdin, performs one operation, writes one bounded
 * response to stdout, closes both handles, and exits.
 */

#define INITGUID

#include "src/platform/windows/PolicyConfig.h"
#include "src/platform/windows/audio_policy_helper_protocol.h"

#include <Audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using platf::audio_policy::protocol::helper_status_e;
  using platf::audio_policy::protocol::operation_e;
  using platf::audio_policy::protocol::render_role_e;
  using platf::audio_policy::protocol::request_header_t;
  using platf::audio_policy::protocol::request_message_t;
  using platf::audio_policy::protocol::response_message_t;
  using platf::audio_policy::protocol::response_header_t;
  using platf::audio_policy::protocol::wave_format_extensible_t;

#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
  unsigned int g_test_failure_mode = 0u;
  unsigned int g_test_read_count = 0u;
  unsigned int g_test_set_count = 0u;
#endif

  template<typename Interface>
  class com_ptr_t {
  public:
    com_ptr_t() = default;
    ~com_ptr_t() {
      reset();
    }

    com_ptr_t(const com_ptr_t &) = delete;
    com_ptr_t &operator=(const com_ptr_t &) = delete;

    Interface **put() {
      reset();
      return &value_;
    }

    Interface *get() const {
      return value_;
    }

    Interface *operator->() const {
      return value_;
    }

    void reset() {
      if (value_ != nullptr) {
        value_->Release();
        value_ = nullptr;
      }
    }

  private:
    Interface *value_ {nullptr};
  };

  bool read_exact(HANDLE handle, void *buffer, const DWORD size) {
    auto *cursor = static_cast<std::uint8_t *>(buffer);
    DWORD remaining = size;
    while (remaining != 0u) {
      DWORD transferred = 0u;
      if (!ReadFile(handle, cursor, remaining, &transferred, nullptr) || transferred == 0u) {
        return false;
      }
      cursor += transferred;
      remaining -= transferred;
    }
    return true;
  }

  bool no_extra_bytes(HANDLE handle) {
    std::uint8_t byte = 0u;
    DWORD transferred = 0u;
    if (ReadFile(handle, &byte, 1u, &transferred, nullptr)) {
      return transferred == 0u;
    }
    return GetLastError() == ERROR_BROKEN_PIPE;
  }

  bool write_exact(HANDLE handle, const std::vector<std::uint8_t> &bytes) {
    const auto *cursor = bytes.data();
    DWORD remaining = static_cast<DWORD>(bytes.size());
    while (remaining != 0u) {
      DWORD transferred = 0u;
      if (!WriteFile(handle, cursor, remaining, &transferred, nullptr) || transferred == 0u) {
        return false;
      }
      cursor += transferred;
      remaining -= transferred;
    }
    return true;
  }

  bool read_request(HANDLE handle, request_message_t &request) {
    request_header_t header {};
    if (!read_exact(handle, &header, static_cast<DWORD>(sizeof(header)))) {
      return false;
    }

    if (header.endpoint_bytes > platf::audio_policy::protocol::kMaxEndpointIdBytes ||
        header.expected_bytes > platf::audio_policy::protocol::kMaxEndpointIdBytes ||
        header.format_bytes > sizeof(wave_format_extensible_t) ||
        header.endpoint_bytes != header.endpoint_code_units * sizeof(wchar_t) ||
        header.expected_bytes != header.expected_code_units * sizeof(wchar_t) ||
        header.endpoint_bytes % sizeof(wchar_t) != 0u) {
      return false;
    }

    std::vector<std::uint8_t> encoded(
      sizeof(header) + header.endpoint_bytes + header.expected_bytes + header.format_bytes);
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (header.endpoint_bytes != 0u &&
        !read_exact(
          handle,
          encoded.data() + sizeof(header),
          header.endpoint_bytes)) {
      return false;
    }
    if (header.expected_bytes != 0u &&
        !read_exact(
          handle,
          encoded.data() + sizeof(header) + header.endpoint_bytes,
          header.expected_bytes)) {
      return false;
    }
    if (header.format_bytes != 0u &&
        !read_exact(
          handle,
          encoded.data() + sizeof(header) + header.endpoint_bytes + header.expected_bytes,
          header.format_bytes)) {
      return false;
    }
    if (!no_extra_bytes(handle)) {
      return false;
    }
    return platf::audio_policy::protocol::decode_request(encoded, request);
  }

  bool write_response(HANDLE handle, const response_message_t &response) {
    std::vector<std::uint8_t> encoded;
    if (!platf::audio_policy::protocol::encode_response(response, encoded)) {
      return false;
    }
    return write_exact(handle, encoded);
  }

  response_message_t invalid_request_response() {
    response_message_t response {};
    response.status = helper_status_e::invalid_request;
    response.com_hresult = E_INVALIDARG;
    response.set_hresult = E_INVALIDARG;
    response.format_hresult = E_INVALIDARG;
    response.read_hresult = E_INVALIDARG;
    response.operation = operation_e::read;
    response.execution =
      platf::audio_policy::protocol::execution_disposition_e::not_started;
    return response;
  }

  HRESULT read_default_endpoint(
    const render_role_e role,
    std::wstring &readback_id,
    bool *com_failure = nullptr) {
    readback_id.clear();
    if (com_failure != nullptr) {
      *com_failure = false;
    }
#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
    ++g_test_read_count;
    if (g_test_failure_mode == 1u ||
        (g_test_failure_mode == 2u && g_test_read_count >= 2u)) {
      if (com_failure != nullptr) {
        *com_failure = true;
      }
      return E_FAIL;
    }
    if (g_test_failure_mode == 2u && g_test_read_count == 1u) {
      readback_id = L"{expected-current}";
      return S_OK;
    }
#endif
    com_ptr_t<IMMDeviceEnumerator> device_enumerator;
    auto status = CoCreateInstance(
      CLSID_MMDeviceEnumerator,
      nullptr,
      CLSCTX_ALL,
      IID_IMMDeviceEnumerator,
      reinterpret_cast<void **>(device_enumerator.put()));
    if (FAILED(status)) {
      if (com_failure != nullptr) {
        *com_failure = true;
      }
      return status;
    }

    com_ptr_t<IMMDevice> device;
    status = device_enumerator->GetDefaultAudioEndpoint(
      eRender,
      static_cast<ERole>(role),
      device.put());
    if (FAILED(status)) {
      return status;
    }

    LPWSTR raw_id = nullptr;
    status = device->GetId(&raw_id);
    if (FAILED(status)) {
      return status;
    }
    if (raw_id == nullptr || raw_id[0] == L'\0') {
      if (raw_id != nullptr) {
        CoTaskMemFree(raw_id);
      }
      return E_UNEXPECTED;
    }
    readback_id.assign(raw_id);
    CoTaskMemFree(raw_id);
    return readback_id.empty() ? E_UNEXPECTED : S_OK;
  }

  // Keep the policy write behind a small auditable helper. The service binary
  // does not link or call IPolicyConfig::SetDefaultEndpoint.
  HRESULT set_default_endpoint(
    IPolicyConfig *policy,
    const std::wstring &endpoint_id,
    const render_role_e role) {
#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
    ++g_test_set_count;
    if (g_test_failure_mode == 2u) {
      return S_OK;
    }
#endif
    return policy->SetDefaultEndpoint(endpoint_id.c_str(), static_cast<ERole>(role));
  }

  HRESULT create_policy_client(IPolicyConfig **policy) {
#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
    if (g_test_failure_mode == 2u) {
      *policy = nullptr;
      return S_OK;
    }
#endif
    return CoCreateInstance(
      CLSID_CPolicyConfigClient,
      nullptr,
      CLSCTX_ALL,
      IID_IPolicyConfig,
      reinterpret_cast<void **>(policy));
  }

  WAVEFORMATEXTENSIBLE reconstruct_wave_format(
    const wave_format_extensible_t &source) {
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = source.format_tag;
    format.Format.nChannels = source.channels;
    format.Format.nSamplesPerSec = source.samples_per_sec;
    format.Format.nAvgBytesPerSec = source.avg_bytes_per_sec;
    format.Format.nBlockAlign = source.block_align;
    format.Format.wBitsPerSample = source.bits_per_sample;
    format.Format.cbSize = source.cb_size;
    format.Samples.wValidBitsPerSample = source.valid_bits_per_sample;
    format.dwChannelMask = source.channel_mask;
    std::memcpy(&format.SubFormat, source.sub_format.data(), sizeof(format.SubFormat));
    return format;
  }

  // Keep SetDeviceFormat behind a small auditable helper. The service binary
  // does not link or call IPolicyConfig::SetDeviceFormat.
  HRESULT set_device_format(
    IPolicyConfig *policy,
    const std::wstring &endpoint_id,
    const wave_format_extensible_t &source) {
    auto desired = reconstruct_wave_format(source);
    WAVEFORMATEXTENSIBLE zero_initialized_default_format {};
    return policy->SetDeviceFormat(
      endpoint_id.c_str(),
      reinterpret_cast<WAVEFORMATEX *>(&desired),
      reinterpret_cast<WAVEFORMATEX *>(&zero_initialized_default_format));
  }

  response_message_t perform_request(const request_message_t &request) {
    response_message_t response {};
    response.operation = request.operation;
    response.status = helper_status_e::success;
    response.com_hresult = S_OK;
    response.set_hresult =
      request.operation == operation_e::read ? E_UNEXPECTED : S_OK;
    response.read_hresult = S_OK;
    response.format_hresult =
      request.operation == operation_e::set_device_format ? S_OK : E_UNEXPECTED;

    com_ptr_t<IPolicyConfig> policy;
    if (request.operation == operation_e::set_device_format) {
      response.set_hresult = E_UNEXPECTED;
      response.read_hresult = E_UNEXPECTED;
      response.format_hresult = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        reinterpret_cast<void **>(policy.put()));
      if (FAILED(response.format_hresult)) {
        response.status = helper_status_e::com_failure;
        response.com_hresult = response.format_hresult;
        response.format_hresult = response.com_hresult;
      } else {
        response.format_hresult = set_device_format(
          policy.get(),
          request.endpoint_id,
          *request.device_format);
        if (FAILED(response.format_hresult)) {
          response.status = helper_status_e::format_failure;
        }
      }
      return response;
    }

    if (request.operation == operation_e::set_and_readback) {
      response.com_hresult = create_policy_client(policy.put());
      if (FAILED(response.com_hresult)) {
        // Policy-client creation is before the write and therefore proves
        // that SetDefaultEndpoint was not issued.
        response.status = helper_status_e::pre_read_failure;
        response.set_hresult = S_OK;
        response.read_hresult = response.com_hresult;
        return response;
      } else {
        std::wstring current_id;
        bool pre_read_com_failure = false;
        const auto pre_read_hresult = read_default_endpoint(
          request.role,
          current_id,
          &pre_read_com_failure);
        if (FAILED(pre_read_hresult)) {
          response.read_hresult = pre_read_hresult;
          response.set_hresult = S_OK;
          response.status = helper_status_e::pre_read_failure;
          if (pre_read_com_failure) {
            response.com_hresult = pre_read_hresult;
          } else {
            response.com_hresult = S_OK;
          }
          return response;
        }
        if (current_id != request.expected_current_id) {
          response.status = helper_status_e::precondition_mismatch;
          response.com_hresult = S_OK;
          response.set_hresult =
            platf::audio_policy::protocol::kPreconditionMismatchHresult;
          response.read_hresult = S_OK;
          response.readback_id = std::move(current_id);
          return response;
        }

        response.set_hresult = set_default_endpoint(
          policy.get(),
          request.endpoint_id,
          request.role);
        if (FAILED(response.set_hresult)) {
          response.status = helper_status_e::set_failure;
        }
      }
    }

    bool read_com_failure = false;
    response.read_hresult = read_default_endpoint(
      request.role,
      response.readback_id,
      &read_com_failure);
    if (FAILED(response.read_hresult)) {
      if (request.operation == operation_e::read) {
        response.status = read_com_failure
          ? helper_status_e::com_failure
          : helper_status_e::read_failure;
        if (read_com_failure) {
          response.com_hresult = response.read_hresult;
        }
      } else if (response.status == helper_status_e::success) {
        // SetDefaultEndpoint has already returned; a failed fresh readback
        // must retain the possibility that the write took effect.
        response.status = helper_status_e::post_set_readback_failure;
        response.com_hresult = S_OK;
      } else if (response.status == helper_status_e::set_failure) {
        // The write call was issued and failed. Preserve the set failure while
        // retaining an exact readback HRESULT if one is available.
        response.com_hresult = S_OK;
      }
    }
    return response;
  }

  HRESULT initialize_com_for_request() {
#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
    if (g_test_failure_mode == 3u) {
      return E_FAIL;
    }
#endif
    return CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  }

  response_message_t com_initialization_failure_response(
    const operation_e operation,
    const HRESULT com_status) {
    response_message_t response {};
    response.operation = operation;
    response.status = helper_status_e::com_failure;
    response.com_hresult = com_status;
    response.set_hresult = E_UNEXPECTED;
    response.format_hresult = E_UNEXPECTED;
    response.read_hresult = E_UNEXPECTED;
    if (operation == operation_e::set_and_readback) {
      // COM failed before policy-client creation or the compare-and-set
      // pre-read, so SetDefaultEndpoint was provably never issued.
      response.status = helper_status_e::pre_read_failure;
      response.set_hresult = S_OK;
      response.read_hresult = com_status;
    } else if (operation == operation_e::read) {
      response.read_hresult = com_status;
    } else if (operation == operation_e::set_device_format) {
      response.format_hresult = com_status;
    }
    return response;
  }

  response_message_t run_request_with_com(const request_message_t &request) {
    const auto com_status = initialize_com_for_request();
    if (FAILED(com_status)) {
      return com_initialization_failure_response(request.operation, com_status);
    }

    const auto response = perform_request(request);
    CoUninitialize();
    return response;
  }

}  // namespace

#if defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
extern "C" void sunshine_audio_policy_helper_test_set_failure_mode(
  const unsigned int mode) {
  g_test_failure_mode = mode;
  g_test_read_count = 0u;
  g_test_set_count = 0u;
}

extern "C" unsigned int sunshine_audio_policy_helper_test_get_set_count() {
  return g_test_set_count;
}

extern "C" bool sunshine_audio_policy_helper_test_perform_request(
  const request_message_t *request,
  response_message_t *response) {
  if (request == nullptr || response == nullptr) {
    return false;
  }
  *response = perform_request(*request);
  std::vector<std::uint8_t> encoded;
  return platf::audio_policy::protocol::encode_response(*response, encoded);
}

extern "C" bool sunshine_audio_policy_helper_test_run_request_with_com(
  const request_message_t *request,
  response_message_t *response) {
  if (request == nullptr || response == nullptr) {
    return false;
  }
  *response = run_request_with_com(*request);
  std::vector<std::uint8_t> encoded;
  return platf::audio_policy::protocol::encode_response(*response, encoded);
}
#endif

#if !defined(SUNSHINE_AUDIO_POLICY_HELPER_TESTING)
int main(const int argc, char **argv) {
  if (argc != 2 || std::string_view {argv[1]} != "--protocol=2") {
    return 2;
  }

  const auto request_handle = GetStdHandle(STD_INPUT_HANDLE);
  const auto response_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (request_handle == nullptr || request_handle == INVALID_HANDLE_VALUE ||
      response_handle == nullptr || response_handle == INVALID_HANDLE_VALUE) {
    return 3;
  }

  request_message_t request {};
  if (!read_request(request_handle, request)) {
    const auto response = invalid_request_response();
    return write_response(response_handle, response) ? 4 : 5;
  }

  const auto response = run_request_with_com(request);
  const auto wrote = write_response(response_handle, response);
  return wrote ? 0 : 8;
}
#endif

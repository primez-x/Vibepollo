/**
 * @file tests/helpers/audio_policy_blocking_helper.cpp
 * @brief Deterministic child used by the audio policy supervisor tests.
 */

#include "src/platform/windows/audio_policy_helper_protocol.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

  using namespace platf::audio_policy::protocol;

  bool read_exact(HANDLE handle, void *buffer, DWORD size) {
    auto *cursor = static_cast<std::uint8_t *>(buffer);
    while (size != 0u) {
      DWORD transferred = 0u;
      if (!ReadFile(handle, cursor, size, &transferred, nullptr) || transferred == 0u) {
        return false;
      }
      cursor += transferred;
      size -= transferred;
    }
    return true;
  }

  bool read_request(HANDLE handle, request_message_t &request) {
    request_header_t header {};
    if (!read_exact(handle, &header, static_cast<DWORD>(sizeof(header))) ||
        header.endpoint_bytes > kMaxEndpointIdBytes ||
        header.expected_bytes > kMaxEndpointIdBytes ||
        header.format_bytes > sizeof(wave_format_extensible_t) ||
        header.endpoint_bytes != header.endpoint_code_units * sizeof(wchar_t) ||
        header.expected_bytes != header.expected_code_units * sizeof(wchar_t)) {
      return false;
    }
    std::vector<std::uint8_t> encoded(
      sizeof(header) + header.endpoint_bytes + header.expected_bytes + header.format_bytes);
    std::memcpy(encoded.data(), &header, sizeof(header));
    if (header.endpoint_bytes != 0u &&
        !read_exact(handle, encoded.data() + sizeof(header), header.endpoint_bytes)) {
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
    return decode_request(encoded, request);
  }

  bool write_exact(HANDLE handle, const std::vector<std::uint8_t> &encoded) {
    const auto *cursor = encoded.data();
    DWORD remaining = static_cast<DWORD>(encoded.size());
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

  int emit_response(HANDLE handle, const response_message_t &response, const bool short_output, const bool extra_output) {
    std::vector<std::uint8_t> encoded;
    if (!encode_response(response, encoded)) {
      return 3;
    }
    if (short_output) {
      encoded.resize(sizeof(response_header_t) - 1u);
    } else if (extra_output) {
      encoded.push_back(0xA5u);
    }
    return write_exact(handle, encoded) ? 0 : 4;
  }

}  // namespace

int main(const int argc, char **argv) {
  if (argc != 2 || std::string_view {argv[1]} != "--protocol=2") {
    return 2;
  }

  const auto request_handle = GetStdHandle(STD_INPUT_HANDLE);
  const auto response_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  request_message_t request {};
  if (request_handle == nullptr || response_handle == nullptr ||
      !read_request(request_handle, request)) {
    return 3;
  }

  if (request.endpoint_id == L"block" ||
      (request.operation == operation_e::read &&
       request.role == render_role_e::communications)) {
    Sleep(INFINITE);
  }

  if (request.operation == operation_e::set_device_format) {
    response_message_t response {
      helper_status_e::success,
      S_OK,
      E_UNEXPECTED,
      E_UNEXPECTED,
      {},
    };
    response.format_hresult = S_OK;
    response.operation = request.operation;
    if (request.endpoint_id == L"format-error") {
      response.status = helper_status_e::format_failure;
      response.format_hresult = E_FAIL;
    }
    return emit_response(response_handle, response, false, false);
  }

  if (request.operation == operation_e::set_and_readback && request.endpoint_id == L"com-error") {
    response_message_t response {
      helper_status_e::com_failure,
      E_FAIL,
      E_FAIL,
      E_UNEXPECTED,
      {},
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }

  if (
    request.operation == operation_e::set_and_readback &&
    request.endpoint_id == L"pre-read-failure") {
    response_message_t response {
      helper_status_e::pre_read_failure,
      E_FAIL,
      S_OK,
      E_FAIL,
      {},
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }

  if (
    request.operation == operation_e::set_and_readback &&
    request.endpoint_id == L"post-set-read-failure") {
    response_message_t response {
      helper_status_e::post_set_readback_failure,
      S_OK,
      S_OK,
      E_FAIL,
      {},
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }

  if (
    request.operation == operation_e::read &&
    request.role == render_role_e::multimedia) {
    response_message_t response {
      helper_status_e::com_failure,
      E_FAIL,
      E_UNEXPECTED,
      E_FAIL,
      {},
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }

  if (
    request.operation == operation_e::set_and_readback &&
    request.endpoint_id == L"post-read-com-error") {
    response_message_t response {
      helper_status_e::post_set_readback_failure,
      S_OK,
      S_OK,
      E_FAIL,
      {},
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }

  if (request.operation == operation_e::set_and_readback && request.endpoint_id == L"short") {
    response_message_t response {
      helper_status_e::success,
      S_OK,
      S_OK,
      S_OK,
      L"helper-readback",
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, true, false);
  }

  if (request.operation == operation_e::set_and_readback && request.endpoint_id == L"extra") {
    response_message_t response {
      helper_status_e::success,
      S_OK,
      S_OK,
      S_OK,
      L"helper-readback",
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, true);
  }

  if (request.operation == operation_e::set_and_readback && request.endpoint_id == L"stderr") {
    constexpr char diagnostic[] = "diagnostic\n";
    DWORD written = 0u;
    WriteFile(
      GetStdHandle(STD_ERROR_HANDLE),
      diagnostic,
      static_cast<DWORD>(sizeof(diagnostic) - 1u),
      &written,
      nullptr);
  }

  const auto readback = request.operation == operation_e::read
    ? std::wstring {L"helper-readback"}
    : request.endpoint_id;
  if (request.operation == operation_e::set_and_readback &&
      request.endpoint_id == L"precondition-mismatch") {
    response_message_t response {
      helper_status_e::precondition_mismatch,
      S_OK,
      kPreconditionMismatchHresult,
      S_OK,
      L"{new-current-after-precondition-change}",
    };
    response.format_hresult = E_UNEXPECTED;
    response.operation = request.operation;
    return emit_response(response_handle, response, false, false);
  }
  const auto result = emit_response(
    response_handle,
    [&request, &readback]() {
      response_message_t response {};
      response.status = helper_status_e::success;
      response.com_hresult = S_OK;
      response.set_hresult = E_UNEXPECTED;
      response.format_hresult = E_UNEXPECTED;
      response.read_hresult = S_OK;
      response.readback_id = readback;
      response.operation = request.operation;
      if (request.operation == operation_e::set_device_format) {
        response.set_hresult = E_UNEXPECTED;
        response.read_hresult = E_UNEXPECTED;
        response.readback_id.clear();
        response.format_hresult = S_OK;
      } else if (request.operation == operation_e::set_and_readback) {
        response.set_hresult = S_OK;
      } else if (request.operation == operation_e::read) {
        response.readback_id = L"helper-readback";
      }
      return response;
    }(),
    false,
    false);
  if (request.operation == operation_e::set_and_readback && request.endpoint_id == L"nonzero") {
    return result == 0 ? 9 : result;
  }
  return result;
}

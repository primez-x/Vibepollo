/**
 * @file src/platform/windows/audio_policy_helper_protocol.cpp
 * @brief Encoding and validation for the one-shot audio policy protocol.
 */

#include "audio_policy_helper_protocol.h"

#include <ksmedia.h>
#include <mmreg.h>

#include <cstring>

namespace platf::audio_policy::protocol {
namespace {

  constexpr GUID kPcmSubtype {
    0x00000001u,
    0x0000u,
    0x0010u,
    {0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u},
  };
  constexpr GUID kIeeeFloatSubtype {
    0x00000003u,
    0x0000u,
    0x0010u,
    {0x80u, 0x00u, 0x00u, 0xAAu, 0x00u, 0x38u, 0x9Bu, 0x71u},
  };

  bool valid_operation(const std::uint16_t value) noexcept {
    return value == static_cast<std::uint16_t>(operation_e::read) ||
           value == static_cast<std::uint16_t>(operation_e::set_and_readback) ||
           value == static_cast<std::uint16_t>(operation_e::set_device_format);
  }

  bool valid_role(const std::uint32_t value) noexcept {
    return value <= static_cast<std::uint32_t>(render_role_e::communications);
  }

  bool valid_status(const std::uint16_t value) noexcept {
    return value <= static_cast<std::uint16_t>(
      helper_status_e::post_set_readback_failure);
  }

  bool valid_execution(const std::uint16_t value) noexcept {
    return value <= static_cast<std::uint16_t>(execution_disposition_e::completed);
  }

  bool valid_operation_format_pair(
    const operation_e operation,
    const bool has_format) noexcept {
    return has_format == (operation == operation_e::set_device_format);
  }

  bool valid_operation_expected_pair(
    const operation_e operation,
    const bool has_expected_current_id) noexcept {
    return has_expected_current_id == (operation == operation_e::set_and_readback);
  }

  bool valid_endpoint_length(
    const std::uint32_t code_units,
    const std::uint32_t bytes) noexcept {
    return code_units <= kMaxEndpointIdCodeUnits &&
           bytes <= kMaxEndpointIdBytes &&
           bytes == code_units * sizeof(wchar_t) &&
           (bytes % sizeof(wchar_t)) == 0u;
  }

  bool is_guid(
    const std::array<std::uint8_t, 16u> &actual,
    const GUID &expected) noexcept {
    return std::memcmp(actual.data(), &expected, sizeof(expected)) == 0;
  }

  bool valid_channel_mask(
    const std::uint16_t channels,
    const std::uint32_t channel_mask) noexcept {
    constexpr auto stereo =
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    constexpr auto surround51_back =
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
      SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
      SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    constexpr auto surround51_side =
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
      SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
      SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    constexpr auto surround71 =
      SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
      SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
      SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
      SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;

    switch (channels) {
      case 2u:
        return channel_mask == stereo;
      case 6u:
        return channel_mask == surround51_back || channel_mask == surround51_side;
      case 8u:
        return channel_mask == surround71;
      default:
        return false;
    }
  }

  template<typename Header>
  bool valid_common_header(const Header &header) noexcept {
    return header.magic == kMagic &&
           header.version == kVersion &&
           header.reserved0 == 0u &&
           header.reserved1 == 0u;
  }

  void append_wstring_bytes(
    const std::wstring &value,
    std::vector<std::uint8_t> &encoded) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(value.data());
    encoded.insert(encoded.end(), bytes, bytes + value.size() * sizeof(wchar_t));
  }

  void append_format_bytes(
    const wave_format_extensible_t &format,
    std::vector<std::uint8_t> &encoded) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&format);
    encoded.insert(encoded.end(), bytes, bytes + sizeof(format));
  }

  std::wstring copy_wstring_bytes(
    const std::uint8_t *bytes,
    const std::size_t code_units) {
    std::wstring value(code_units, L'\0');
    if (!value.empty()) {
      std::memcpy(value.data(), bytes, code_units * sizeof(wchar_t));
    }
    return value;
  }

  wave_format_extensible_t copy_format_bytes(
    const std::uint8_t *bytes) {
    wave_format_extensible_t format {};
    std::memcpy(&format, bytes, sizeof(format));
    return format;
  }

  bool valid_readback_result(
    const HRESULT read_hresult,
    const std::wstring &readback_id) noexcept {
    return (SUCCEEDED(read_hresult) && !readback_id.empty()) ||
           (FAILED(read_hresult) && readback_id.empty());
  }

  bool valid_response_semantics(const response_message_t &response) noexcept {
    if (response.status == helper_status_e::invalid_request) {
      return response.execution == execution_disposition_e::not_started &&
             response.readback_id.empty() &&
             response.com_hresult == E_INVALIDARG &&
             response.set_hresult == E_INVALIDARG &&
             response.format_hresult == E_INVALIDARG &&
             response.read_hresult == E_INVALIDARG;
    }
    if (response.execution != execution_disposition_e::completed) {
      return false;
    }

    switch (response.operation) {
      case operation_e::read:
        if (response.set_hresult != E_UNEXPECTED ||
            response.format_hresult != E_UNEXPECTED) {
          return false;
        }
        switch (response.status) {
          case helper_status_e::success:
            return response.com_hresult == S_OK &&
                   response.read_hresult == S_OK &&
                   !response.readback_id.empty();
          case helper_status_e::com_failure:
            return FAILED(response.com_hresult) &&
                   response.read_hresult == response.com_hresult &&
                   response.readback_id.empty();
          case helper_status_e::read_failure:
            return response.com_hresult == S_OK &&
                   FAILED(response.read_hresult) &&
                   response.readback_id.empty();
          default:
            return false;
        }
      case operation_e::set_and_readback:
        if (response.format_hresult != E_UNEXPECTED) {
          return false;
        }
        switch (response.status) {
          case helper_status_e::success:
            return response.com_hresult == S_OK &&
                   response.set_hresult == S_OK &&
                   response.read_hresult == S_OK &&
                   !response.readback_id.empty();
          case helper_status_e::com_failure:
            return FAILED(response.com_hresult) &&
                   response.set_hresult == response.com_hresult &&
                   response.read_hresult == E_UNEXPECTED &&
                   response.readback_id.empty();
          case helper_status_e::pre_read_failure:
            return (response.com_hresult == S_OK ||
                    response.com_hresult == response.read_hresult) &&
                   response.set_hresult == S_OK &&
                   FAILED(response.read_hresult) &&
                   response.readback_id.empty();
          case helper_status_e::post_set_readback_failure:
            return response.com_hresult == S_OK &&
                   response.set_hresult == S_OK &&
                   FAILED(response.read_hresult) &&
                   response.readback_id.empty();
          case helper_status_e::set_failure:
            return response.com_hresult == S_OK &&
                   FAILED(response.set_hresult) &&
                   valid_readback_result(response.read_hresult, response.readback_id);
          case helper_status_e::precondition_mismatch:
            return response.com_hresult == S_OK &&
                   response.set_hresult == kPreconditionMismatchHresult &&
                   response.read_hresult == S_OK &&
                   !response.readback_id.empty();
          default:
            return false;
        }
      case operation_e::set_device_format:
        if (!response.readback_id.empty() ||
            response.set_hresult != E_UNEXPECTED ||
            response.read_hresult != E_UNEXPECTED) {
          return false;
        }
        switch (response.status) {
          case helper_status_e::success:
            return response.com_hresult == S_OK &&
                   response.format_hresult == S_OK;
          case helper_status_e::com_failure:
            return FAILED(response.com_hresult) &&
                   response.format_hresult == response.com_hresult;
          case helper_status_e::format_failure:
            return response.com_hresult == S_OK &&
                   FAILED(response.format_hresult);
          default:
            return false;
        }
      default:
        return false;
    }
  }

}  // namespace

bool is_valid_utf16(const std::wstring_view value) noexcept {
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto code_unit = static_cast<std::uint16_t>(value[index]);
    if (code_unit == 0u) {
      return false;
    }
    if (code_unit >= 0xD800u && code_unit <= 0xDBFFu) {
      if (index + 1u >= value.size()) {
        return false;
      }
      const auto low = static_cast<std::uint16_t>(value[++index]);
      if (low < 0xDC00u || low > 0xDFFFu) {
        return false;
      }
    } else if (code_unit >= 0xDC00u && code_unit <= 0xDFFFu) {
      return false;
    }
  }
  return true;
}

bool is_valid_wave_format(const wave_format_extensible_t &format) noexcept {
  const auto is_pcm = is_guid(format.sub_format, kPcmSubtype);
  const auto is_float = is_guid(format.sub_format, kIeeeFloatSubtype);
  const auto valid_container =
    format.bits_per_sample == 16u ||
    format.bits_per_sample == 24u ||
    format.bits_per_sample == 32u;
  const auto valid_bits =
    format.valid_bits_per_sample == 16u ||
    format.valid_bits_per_sample == 24u ||
    format.valid_bits_per_sample == 32u;
  const auto expected_block_align =
    static_cast<std::uint64_t>(format.channels) * format.bits_per_sample / 8u;
  const auto expected_average_bytes =
    static_cast<std::uint64_t>(format.samples_per_sec) * expected_block_align;

  if (format.format_tag != WAVE_FORMAT_EXTENSIBLE ||
      format.cb_size != 22u ||
      format.samples_per_sec != 48'000u ||
      (format.channels != 2u && format.channels != 6u && format.channels != 8u) ||
      !valid_container ||
      !valid_bits ||
      format.valid_bits_per_sample > format.bits_per_sample ||
      expected_block_align != format.block_align ||
      expected_average_bytes != format.avg_bytes_per_sec ||
      !valid_channel_mask(format.channels, format.channel_mask) ||
      (!is_pcm && !is_float) ||
      (is_float &&
       (format.bits_per_sample != 32u || format.valid_bits_per_sample != 32u))) {
    return false;
  }
  return true;
}

bool encode_request(
  const request_message_t &request,
  std::vector<std::uint8_t> &encoded) noexcept {
  encoded.clear();
  const auto operation = static_cast<std::uint16_t>(request.operation);
  const auto role = static_cast<std::uint32_t>(request.role);
  const auto code_units = request.endpoint_id.size();
  const auto bytes = code_units * sizeof(wchar_t);
  const auto expected_code_units = request.expected_current_id.size();
  const auto expected_bytes = expected_code_units * sizeof(wchar_t);
  if (code_units > kMaxEndpointIdCodeUnits) {
    return false;
  }
  if (expected_code_units > kMaxEndpointIdCodeUnits) {
    return false;
  }
  const auto has_format = request.device_format.has_value();
  if (!valid_operation(operation) ||
      !valid_role(role) ||
      !valid_endpoint_length(
        static_cast<std::uint32_t>(code_units),
        static_cast<std::uint32_t>(bytes)) ||
      !valid_endpoint_length(
        static_cast<std::uint32_t>(expected_code_units),
        static_cast<std::uint32_t>(expected_bytes)) ||
      !is_valid_utf16(request.endpoint_id) ||
      !is_valid_utf16(request.expected_current_id) ||
      (request.operation == operation_e::read && !request.endpoint_id.empty()) ||
      (request.operation != operation_e::read && request.endpoint_id.empty()) ||
      !valid_operation_format_pair(request.operation, has_format) ||
      !valid_operation_expected_pair(
        request.operation,
        !request.expected_current_id.empty()) ||
      (has_format && !is_valid_wave_format(*request.device_format))) {
    return false;
  }

  const request_header_t header {
    kMagic,
    kVersion,
    operation,
    role,
    static_cast<std::uint32_t>(code_units),
    static_cast<std::uint32_t>(bytes),
    static_cast<std::uint32_t>(expected_code_units),
    static_cast<std::uint32_t>(expected_bytes),
    has_format ? static_cast<std::uint32_t>(sizeof(wave_format_extensible_t)) : 0u,
    0u,
    0u,
  };
  encoded.resize(sizeof(header));
  std::memcpy(encoded.data(), &header, sizeof(header));
  append_wstring_bytes(request.endpoint_id, encoded);
  append_wstring_bytes(request.expected_current_id, encoded);
  if (has_format) {
    append_format_bytes(*request.device_format, encoded);
  }
  return true;
}

bool decode_request(
  const std::span<const std::uint8_t> encoded,
  request_message_t &request) noexcept {
  request = {};
  if (encoded.size() < sizeof(request_header_t)) {
    return false;
  }

  request_header_t header {};
  std::memcpy(&header, encoded.data(), sizeof(header));
  const auto operation = static_cast<operation_e>(header.operation);
  if (!valid_common_header(header) ||
      !valid_operation(header.operation) ||
      !valid_role(header.role) ||
      !valid_endpoint_length(header.endpoint_code_units, header.endpoint_bytes) ||
      !valid_endpoint_length(header.expected_code_units, header.expected_bytes) ||
      (header.format_bytes != 0u &&
       header.format_bytes != sizeof(wave_format_extensible_t)) ||
      !valid_operation_format_pair(operation, header.format_bytes != 0u) ||
      !valid_operation_expected_pair(operation, header.expected_bytes != 0u) ||
      encoded.size() != sizeof(header) + header.endpoint_bytes +
        header.expected_bytes + header.format_bytes) {
    return false;
  }

  const auto endpoint = copy_wstring_bytes(
    encoded.data() + sizeof(header),
    header.endpoint_code_units);
  const auto expected_current_id = copy_wstring_bytes(
    encoded.data() + sizeof(header) + header.endpoint_bytes,
    header.expected_code_units);
  if (!is_valid_utf16(endpoint) ||
      !is_valid_utf16(expected_current_id) ||
      (operation == operation_e::read && !endpoint.empty()) ||
      (operation != operation_e::read && endpoint.empty()) ||
      !valid_operation_expected_pair(operation, !expected_current_id.empty())) {
    return false;
  }

  request.operation = operation;
  request.role = static_cast<render_role_e>(header.role);
  request.endpoint_id = endpoint;
  request.expected_current_id = expected_current_id;
  if (header.format_bytes != 0u) {
    const auto format = copy_format_bytes(
      encoded.data() + sizeof(header) + header.endpoint_bytes + header.expected_bytes);
    if (!is_valid_wave_format(format)) {
      return false;
    }
    request.device_format = format;
  }
  return true;
}

bool encode_response(
  const response_message_t &response,
  std::vector<std::uint8_t> &encoded) noexcept {
  encoded.clear();
  const auto operation = static_cast<std::uint16_t>(response.operation);
  const auto status = static_cast<std::uint16_t>(response.status);
  const auto code_units = response.readback_id.size();
  const auto bytes = code_units * sizeof(wchar_t);
  if (code_units > kMaxEndpointIdCodeUnits) {
    return false;
  }
  const auto is_format_operation = response.operation == operation_e::set_device_format;
  if (!valid_status(status) ||
      !valid_operation(operation) ||
      !valid_endpoint_length(
        static_cast<std::uint32_t>(code_units),
        static_cast<std::uint32_t>(bytes)) ||
      !is_valid_utf16(response.readback_id) ||
      (is_format_operation && !response.readback_id.empty()) ||
      (!is_format_operation &&
       response.status == helper_status_e::success && response.readback_id.empty())) {
    return false;
  }
  if (!valid_response_semantics(response)) {
    return false;
  }

  const response_header_t header {
    kMagic,
    kVersion,
    operation,
    status,
    static_cast<std::uint16_t>(response.execution),
    0u,
    static_cast<std::int32_t>(response.com_hresult),
    static_cast<std::int32_t>(response.set_hresult),
    static_cast<std::int32_t>(response.format_hresult),
    static_cast<std::int32_t>(response.read_hresult),
    static_cast<std::uint32_t>(code_units),
    static_cast<std::uint32_t>(bytes),
    0u,
    0u,
  };
  encoded.resize(sizeof(header));
  std::memcpy(encoded.data(), &header, sizeof(header));
  append_wstring_bytes(response.readback_id, encoded);
  return true;
}

bool decode_response(
  const std::span<const std::uint8_t> encoded,
  response_message_t &response) noexcept {
  response = {};
  if (encoded.size() < sizeof(response_header_t)) {
    return false;
  }

  response_header_t header {};
  std::memcpy(&header, encoded.data(), sizeof(header));
  const auto operation = static_cast<operation_e>(header.operation);
  const auto status = static_cast<helper_status_e>(header.status);
  if (header.magic != kMagic ||
      header.version != kVersion ||
      header.reserved_header != 0u ||
      header.reserved0 != 0u ||
      header.reserved1 != 0u ||
      !valid_operation(header.operation) ||
      !valid_status(header.status) ||
      !valid_execution(header.execution) ||
      !valid_endpoint_length(header.readback_code_units, header.readback_bytes) ||
      encoded.size() != sizeof(header) + header.readback_bytes) {
    return false;
  }

  const auto readback_id = copy_wstring_bytes(
    encoded.data() + sizeof(header),
    header.readback_code_units);
  if (!is_valid_utf16(readback_id) ||
      (operation == operation_e::set_device_format && !readback_id.empty()) ||
      (operation != operation_e::set_device_format &&
       status == helper_status_e::success && readback_id.empty())) {
    return false;
  }

  response.operation = operation;
  response.status = status;
  response.execution = static_cast<execution_disposition_e>(header.execution);
  response.com_hresult = static_cast<HRESULT>(header.com_hresult);
  response.set_hresult = static_cast<HRESULT>(header.set_hresult);
  response.format_hresult = static_cast<HRESULT>(header.format_hresult);
  response.read_hresult = static_cast<HRESULT>(header.read_hresult);
  response.readback_id = readback_id;
  return valid_response_semantics(response);
}

}  // namespace platf::audio_policy::protocol

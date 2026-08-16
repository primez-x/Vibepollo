/**
 * @file src/platform/windows/audio_policy_helper_protocol.h
 * @brief Versioned bounded wire protocol for the one-shot audio policy helper.
 */
#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace platf::audio_policy::protocol {

  inline constexpr std::uint32_t kMagic = 0x41555031u;  // "AUP1"
  inline constexpr std::uint16_t kVersion = 2u;
  inline constexpr std::size_t kMaxEndpointIdCodeUnits = 2048u;
  inline constexpr std::size_t kMaxEndpointIdBytes = kMaxEndpointIdCodeUnits * sizeof(char16_t);
  inline constexpr HRESULT kPreconditionMismatchHresult =
    HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);

  enum class operation_e : std::uint16_t {
    read = 1,
    set_and_readback = 2,
    set_device_format = 3,
  };

  enum class render_role_e : std::uint32_t {
    console = 0,
    multimedia = 1,
    communications = 2,
  };

  enum class execution_disposition_e : std::uint16_t {
    not_started = 0,
    child_resumed_may_have_executed = 1,
    completed = 2,
  };

  enum class helper_status_e : std::uint16_t {
    success = 0,
    com_failure = 1,
    set_failure = 2,
    read_failure = 3,
    format_failure = 4,
    precondition_mismatch = 5,
    invalid_request = 6,
    // A failed fresh read/policy-client setup before SetDefaultEndpoint.
    // The authenticated HRESULT matrix requires set_hresult == S_OK.
    pre_read_failure = 7,
    // The write call completed, but the fresh readback failed afterwards.
    post_set_readback_failure = 8,
  };

#pragma pack(push, 1)
  struct wave_format_extensible_t {
    std::uint16_t format_tag;
    std::uint16_t channels;
    std::uint32_t samples_per_sec;
    std::uint32_t avg_bytes_per_sec;
    std::uint16_t block_align;
    std::uint16_t bits_per_sample;
    std::uint16_t cb_size;
    std::uint16_t valid_bits_per_sample;
    std::uint32_t channel_mask;
    std::array<std::uint8_t, 16u> sub_format;
  };

  struct request_header_t {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t operation;
    std::uint32_t role;
    std::uint32_t endpoint_code_units;
    std::uint32_t endpoint_bytes;
    std::uint32_t expected_code_units;
    std::uint32_t expected_bytes;
    std::uint32_t format_bytes;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
  };

  struct response_header_t {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t operation;
    std::uint16_t status;
    std::uint16_t execution;
    std::uint16_t reserved_header;
    std::int32_t com_hresult;
    std::int32_t set_hresult;
    std::int32_t format_hresult;
    std::int32_t read_hresult;
    std::uint32_t readback_code_units;
    std::uint32_t readback_bytes;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
  };
#pragma pack(pop)

  static_assert(sizeof(wave_format_extensible_t) == 40u);
  static_assert(sizeof(request_header_t) == 40u);
  static_assert(sizeof(response_header_t) == 46u);
  static_assert(sizeof(wchar_t) == sizeof(char16_t));

  struct request_message_t {
    operation_e operation;
    render_role_e role;
    std::wstring endpoint_id;
    std::optional<wave_format_extensible_t> device_format;
    std::wstring expected_current_id;
  };

  struct response_message_t {
    helper_status_e status;
    HRESULT com_hresult;
    HRESULT set_hresult;
    HRESULT read_hresult;
    std::wstring readback_id;
    HRESULT format_hresult {E_UNEXPECTED};
    operation_e operation {operation_e::read};
    execution_disposition_e execution {execution_disposition_e::completed};
  };

  bool is_valid_utf16(std::wstring_view value) noexcept;

  bool is_valid_wave_format(const wave_format_extensible_t &format) noexcept;

  bool encode_request(
    const request_message_t &request,
    std::vector<std::uint8_t> &encoded) noexcept;

  bool decode_request(
    std::span<const std::uint8_t> encoded,
    request_message_t &request) noexcept;

  bool encode_response(
    const response_message_t &response,
    std::vector<std::uint8_t> &encoded) noexcept;

  bool decode_response(
    std::span<const std::uint8_t> encoded,
    response_message_t &response) noexcept;

}  // namespace platf::audio_policy::protocol

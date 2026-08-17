/**
 * @file src/client_hdr_capabilities.h
 * @brief Bounded host parsing for v1 client HDR display capabilities.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace client_hdr {
  inline constexpr std::size_t max_encoded_value_bytes = 4096;
  inline constexpr std::size_t max_decoded_json_bytes = 3072;
  inline constexpr std::uint32_t minimum_peak_luminance_nits = 1;
  inline constexpr std::uint32_t maximum_peak_luminance_nits = 100000;

  inline constexpr std::string_view calibrated_source = "windows-icc-mhc2";
  inline constexpr std::string_view display_reported_source = "dxgi-output";

  enum class parse_status_e {
    absent,
    invalid,
    unsupported,
    valid,
  };

  struct peak_t {
    std::uint32_t peak_luminance_nits {};
    std::string source;

    auto operator==(const peak_t &) const -> bool = default;
  };

  struct capabilities_t {
    std::uint32_t version {1};
    std::string platform {"windows-desktop"};
    std::optional<peak_t> calibrated;
    std::optional<peak_t> display_reported;

    auto operator==(const capabilities_t &) const -> bool = default;
  };

  struct parse_result_t {
    parse_status_e status {parse_status_e::absent};
    std::optional<capabilities_t> capabilities;
  };

  parse_result_t parse(std::string_view encoded_value);
}  // namespace client_hdr

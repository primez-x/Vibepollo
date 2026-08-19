/**
 * @file src/client_hdr_peak.h
 * @brief Bounded parsing and precedence for client-reported HDR peak luminance.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace client_hdr_peak {
  inline constexpr std::uint32_t minimum_reported_nits = 1;
  inline constexpr std::uint32_t maximum_reported_nits = 100000;
  inline constexpr std::uint32_t minimum_host_nits = 400;
  inline constexpr std::uint32_t maximum_host_nits = 2000;

  enum class source_e {
    calibrated,
    display_reported,
  };

  struct result_t {
    std::uint32_t peak_nits {};
    source_e source {source_e::display_reported};
  };

  std::optional<std::uint32_t> parse_nits(std::string_view value);
  std::optional<result_t> resolve(
    std::string_view calibrated_value,
    std::string_view display_reported_value
  );
}  // namespace client_hdr_peak

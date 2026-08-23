/**
 * @file src/client_hdr_peak.cpp
 * @brief Bounded parsing and precedence for client-reported HDR peak luminance.
 */

#include "client_hdr_peak.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace client_hdr_peak {
  namespace {
    std::uint32_t clamp_for_host(const std::uint32_t value) {
      return std::clamp(value, minimum_host_nits, maximum_host_nits);
    }
  }  // namespace

  std::optional<std::uint32_t> parse_nits(const std::string_view value) {
    if (value.empty()) {
      return std::nullopt;
    }

    std::uint32_t parsed {};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc {} || end != value.data() + value.size() ||
        parsed < minimum_reported_nits || parsed > maximum_reported_nits) {
      return std::nullopt;
    }
    return parsed;
  }

  std::optional<result_t> resolve(
    const std::string_view calibrated_value,
    const std::string_view display_reported_value
  ) {
    if (const auto calibrated = parse_nits(calibrated_value)) {
      return result_t {clamp_for_host(*calibrated), source_e::calibrated};
    }
    if (const auto display_reported = parse_nits(display_reported_value)) {
      return result_t {clamp_for_host(*display_reported), source_e::display_reported};
    }
    return std::nullopt;
  }

  effective_request_t resolve_effective_request(
    const bool client_hdr_requested,
    const bool client_prefer_sdr_10bit,
    const request_override_e request_override
  ) {
    switch (request_override) {
      case request_override_e::force_on:
        return {true, false, false};
      case request_override_e::force_off:
        return {false, client_prefer_sdr_10bit, true};
      case request_override_e::automatic:
        return {client_hdr_requested, client_prefer_sdr_10bit, false};
    }
    return {client_hdr_requested, client_prefer_sdr_10bit, false};
  }
}  // namespace client_hdr_peak

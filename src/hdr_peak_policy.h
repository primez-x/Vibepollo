/**
 * @file src/hdr_peak_policy.h
 * @brief Deterministic host HDR peak precedence policy.
 */
#pragma once

#include "client_hdr_capabilities.h"

#include <cstdint>
#include <optional>

namespace hdr::peak_policy {
  enum class source_e {
    none,
    explicit_numeric_override,
    manual_profile,
    client_calibrated,
    client_display_reported,
    global_default,
  };

  enum class explanation_e {
    no_override,
    explicit_numeric_override,
    manual_profile,
    invalid_manual_profile,
    client_calibrated,
    client_display_reported,
    global_default,
    hdr_disabled,
  };

  struct inputs_t {
    bool final_effective_hdr_requested = false;
    std::optional<std::uint32_t> explicit_override_peak_nits;
    bool manual_profile_selected = false;
    std::optional<std::uint32_t> manual_profile_peak_nits;
    client_hdr::capabilities_t client_capabilities;
    std::optional<std::uint32_t> global_default_peak_nits;
  };

  struct result_t {
    std::optional<std::uint32_t> reported_peak_nits;
    source_e source {source_e::none};
    explanation_e explanation {explanation_e::no_override};
  };

  result_t resolve(const inputs_t &inputs);
}  // namespace hdr::peak_policy

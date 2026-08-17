/**
 * @file src/hdr_peak_policy.cpp
 * @brief Deterministic host HDR peak precedence policy.
 */

#include "hdr_peak_policy.h"

namespace hdr::peak_policy {
  namespace {
    result_t global_fallback(const inputs_t &inputs, const explanation_e explanation) {
      if (inputs.global_default_peak_nits) {
        return {
          inputs.global_default_peak_nits,
          source_e::global_default,
          explanation,
        };
      }
      return {
        std::nullopt,
        source_e::none,
        explanation == explanation_e::global_default ? explanation_e::no_override : explanation,
      };
    }
  }  // namespace

  result_t resolve(const inputs_t &inputs) {
    if (inputs.explicit_override_peak_nits) {
      return {
        inputs.explicit_override_peak_nits,
        source_e::explicit_numeric_override,
        explanation_e::explicit_numeric_override,
      };
    }

    if (inputs.manual_profile_selected) {
      if (inputs.manual_profile_peak_nits) {
        return {
          inputs.manual_profile_peak_nits,
          source_e::manual_profile,
          explanation_e::manual_profile,
        };
      }
      return global_fallback(inputs, explanation_e::invalid_manual_profile);
    }

    if (inputs.final_effective_hdr_requested) {
      if (inputs.client_capabilities.calibrated) {
        return {
          inputs.client_capabilities.calibrated->peak_luminance_nits,
          source_e::client_calibrated,
          explanation_e::client_calibrated,
        };
      }
      if (inputs.client_capabilities.display_reported) {
        return {
          inputs.client_capabilities.display_reported->peak_luminance_nits,
          source_e::client_display_reported,
          explanation_e::client_display_reported,
        };
      }
      return global_fallback(inputs, explanation_e::global_default);
    }

    return global_fallback(inputs, explanation_e::hdr_disabled);
  }
}  // namespace hdr::peak_policy

#include "hdr_request_policy.h"

#include <string_view>

namespace hdr::request_policy {

std::optional<override_e> parse_override(const std::string_view value) {
  if (value == "automatic" || value == "auto") {
    return override_e::automatic;
  }
  if (value == "force_on") {
    return override_e::force_on;
  }
  if (value == "force_off") {
    return override_e::force_off;
  }
  return std::nullopt;
}

result_t resolve(const inputs_t &inputs) {
  result_t result {
    .enable_hdr = inputs.client_hdr_requested,
    .prefer_sdr_10bit = inputs.prefer_sdr_10bit,
    .force_sdr = inputs.force_sdr,
  };

  switch (inputs.request_override) {
    case override_e::force_on:
      result.enable_hdr = true;
      result.prefer_sdr_10bit = false;
      result.force_sdr = false;
      break;
    case override_e::force_off:
      result.enable_hdr = false;
      result.force_sdr = true;
      break;
    case override_e::automatic:
      break;
  }

  return result;
}

}  // namespace hdr::request_policy

#pragma once

#include "tools/atmos_capability_probe_policy.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace atmos_probe {
  using observation_provider =
    std::function<probe_observation(const probe_options &)>;

  struct app_result {
    int exit_code {};
    std::string standard_output;
    std::string standard_error;
  };

  struct report_validation_result {
    bool valid {};
    std::string error;
  };

  app_result run_probe(
    std::span<const std::string_view> arguments,
    const observation_provider &provider);
  report_validation_result validate_serialized_report(std::string_view json);
}  // namespace atmos_probe

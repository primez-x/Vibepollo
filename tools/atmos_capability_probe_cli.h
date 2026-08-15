#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace atmos_probe {
  struct cli_options {
    bool json {};
    bool help {};
    std::optional<std::string> endpoint_id;
  };

  struct cli_parse_result {
    std::optional<cli_options> options;
    std::string error;
  };

  cli_parse_result parse_cli(std::span<const std::string_view> arguments);
}  // namespace atmos_probe

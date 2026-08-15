#include "tools/atmos_capability_probe_cli.h"

#include <utility>

namespace atmos_probe {
  cli_parse_result parse_cli(const std::span<const std::string_view> arguments) {
    cli_options options {};

    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto argument = arguments[index];
      if (argument == "--json") {
        options.json = true;
        continue;
      }
      if (argument == "--help") {
        options.help = true;
        continue;
      }
      if (argument == "--endpoint-id") {
        if (options.endpoint_id) {
          return {.error = "--endpoint-id may be specified once"};
        }
        if (index + 1 == arguments.size()) {
          return {.error = "--endpoint-id requires a value"};
        }
        options.endpoint_id = std::string {arguments[++index]};
        continue;
      }
      return {.error = "unknown option: " + std::string {argument}};
    }

    return {.options = std::move(options)};
  }
}  // namespace atmos_probe

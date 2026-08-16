#include "tools/nvidia_nvaudcap_probe.h"

#include <cstddef>
#include <iostream>
#include <string_view>
#include <vector>

int main(const int argc, char *argv[]) {
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto result = nvidia_nvaudcap_probe::run_probe(
    arguments,
    nvidia_nvaudcap_probe::collect_windows_observation);
  std::cout << result.standard_output;
  std::cerr << result.standard_error;
  return result.exit_code;
}

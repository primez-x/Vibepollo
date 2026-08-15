#include "tools/atmos_capability_probe_app.h"
#include "tools/atmos_capability_probe_windows.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include <winrt/base.h>

int main(const int argc, char *argv[]) {
  try {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
  } catch (const std::exception &error) {
    std::cerr << "failed to initialize STA: " << error.what() << '\n';
    return 3;
  } catch (...) {
    std::cerr << "failed to initialize STA\n";
    return 3;
  }

  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 0 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  const auto result = atmos_probe::run_probe(
    arguments,
    atmos_probe::collect_windows_observation);
  std::cout << result.standard_output;
  std::cerr << result.standard_error;
  return result.exit_code;
}

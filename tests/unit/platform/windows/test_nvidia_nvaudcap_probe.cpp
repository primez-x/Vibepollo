#include "../../../tests_common.h"
#include "tools/nvidia_nvaudcap_probe.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

TEST(NvidiaNvAudCapProbe, BuildsTheControllerCompatibleZeroedInterfaceTable) {
  const auto table = nvidia_nvaudcap_probe::make_controller_interface_table();

  EXPECT_EQ(sizeof(table), 0x210U);
  EXPECT_EQ(table.version, 0x01090210U);
  EXPECT_EQ(table.reserved_alignment, 0U);
  for (const auto address : table.methods) {
    EXPECT_EQ(address, 0U);
  }
  for (const auto value : table.reserved_tail) {
    EXPECT_EQ(value, std::byte {});
  }

  static_assert(std::is_same_v<
    decltype(table.methods)::value_type,
    std::uintptr_t>);
}

TEST(NvidiaNvAudCapProbe, DescribesEveryFactoryMethodSlotWithoutGaps) {
  const auto layout = nvidia_nvaudcap_probe::controller_method_layout();

  ASSERT_EQ(layout.size(), 44U);
  for (std::size_t index = 0; index < layout.size(); ++index) {
    EXPECT_EQ(layout[index].offset, 0x08U + index * sizeof(void *));
    EXPECT_FALSE(layout[index].name.empty());
  }
  EXPECT_EQ(layout.front().name, "NvAudCapOpenSession");
  EXPECT_EQ(layout.back().offset, 0x160U);
}

TEST(NvidiaNvAudCapProbe, RejectsNullOutsideAndNonExecutableFactoryPointers) {
  auto table = nvidia_nvaudcap_probe::make_controller_interface_table();
  for (std::size_t index = 0; index < table.methods.size(); ++index) {
    table.methods[index] = 0x1100U + index * 8U;
  }
  const nvidia_nvaudcap_probe::module_image image {
    .base = 0x1000U,
    .size = 0x1000U,
    .executable_ranges = {{.begin = 0x1100U, .end = 0x1800U}},
  };

  const auto valid = nvidia_nvaudcap_probe::validate_interface_table(table, image);
  EXPECT_TRUE(valid.valid);
  EXPECT_EQ(valid.methods.size(), table.methods.size());

  table.methods[3] = 0;
  table.methods[7] = 0x2500U;
  table.methods[11] = 0x1900U;
  const auto invalid = nvidia_nvaudcap_probe::validate_interface_table(table, image);
  EXPECT_FALSE(invalid.valid);
  EXPECT_EQ(invalid.methods[3].state, nvidia_nvaudcap_probe::pointer_state::null_pointer);
  EXPECT_EQ(invalid.methods[7].state, nvidia_nvaudcap_probe::pointer_state::outside_module);
  EXPECT_EQ(invalid.methods[11].state, nvidia_nvaudcap_probe::pointer_state::non_executable);
}

TEST(NvidiaNvAudCapProbe, ReportsTheFactoryOnlyNoInvocationContract) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = nvidia_nvaudcap_probe::run_probe(
    arguments,
    [] {
      nvidia_nvaudcap_probe::probe_observation observation {};
      observation.complete = true;
      observation.dll_path = R"(C:\Windows\System32\nvaudcap64v.dll)";
      observation.signature_valid = true;
      observation.factory_result = 0;
      observation.table = nvidia_nvaudcap_probe::make_controller_interface_table();
      return observation;
    });

  ASSERT_EQ(result.exit_code, 0);
  const auto report = nlohmann::json::parse(result.standard_output);
  EXPECT_EQ(report.at("schema_version"), 1);
  EXPECT_EQ(report.at("probe_kind"), "factory_only");
  EXPECT_FALSE(report.at("endpoint_mutation_attempted").get<bool>());
  EXPECT_FALSE(report.at("audio_capture_started").get<bool>());
  EXPECT_FALSE(report.at("interface_methods_invoked").get<bool>());
  EXPECT_EQ(report.at("interface").at("requested_version"), "0x01090210");
  EXPECT_EQ(report.at("interface").at("table_size"), 0x210U);
}

TEST(NvidiaNvAudCapProbe, RejectsUnknownCliOptionsBeforeLoadingNvidiaCode) {
  const std::array arguments {std::string_view {"--activate-endpoint"}};
  bool provider_called = false;
  const auto result = nvidia_nvaudcap_probe::run_probe(
    arguments,
    [&] {
      provider_called = true;
      return nvidia_nvaudcap_probe::probe_observation {};
    });

  EXPECT_EQ(result.exit_code, 2);
  EXPECT_FALSE(provider_called);
  EXPECT_TRUE(result.standard_error.contains("unknown option"));
}

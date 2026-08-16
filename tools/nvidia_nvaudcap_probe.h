#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nvidia_nvaudcap_probe {
  inline constexpr std::uint32_t controller_interface_version = 0x01090210U;
  inline constexpr std::size_t controller_interface_size = 0x210U;
  inline constexpr std::size_t controller_method_count = 44U;

  // The low 16 bits of NVIDIA's version value are the ABI structure size.
  // NVIDIA's current _nvspcaps64.dll requests this exact 1.9/0x210 table.
  struct alignas(void *) interface_table {
    std::uint32_t version {};
    std::uint32_t reserved_alignment {};
    std::array<std::uintptr_t, controller_method_count> methods {};
    std::array<std::byte, 0xA8U> reserved_tail {};
  };

  static_assert(sizeof(interface_table) == controller_interface_size);
  static_assert(offsetof(interface_table, methods) == 0x08U);
  static_assert(offsetof(interface_table, reserved_tail) == 0x168U);

  enum class method_evidence {
    embedded_name,
    direct_thunk,
    behavior_inferred,
    unresolved,
  };

  struct method_descriptor {
    std::size_t offset {};
    std::string_view name;
    method_evidence evidence {method_evidence::unresolved};
  };

  struct address_range {
    std::uintptr_t begin {};
    std::uintptr_t end {};
  };

  struct module_image {
    std::uintptr_t base {};
    std::size_t size {};
    std::vector<address_range> executable_ranges;
  };

  enum class pointer_state {
    executable,
    null_pointer,
    outside_module,
    non_executable,
  };

  struct method_observation {
    method_descriptor descriptor;
    std::uintptr_t address {};
    pointer_state state {pointer_state::null_pointer};
  };

  struct interface_validation {
    bool valid {};
    std::vector<method_observation> methods;
  };

  struct probe_observation {
    bool complete {};
    std::string dll_path;
    std::string file_version;
    bool signature_valid {};
    std::uint32_t signature_status {};
    std::uintptr_t module_base {};
    std::uintptr_t factory_address {};
    std::int32_t factory_result {};
    interface_table table {};
    interface_validation validation;
    std::vector<std::string> errors;
  };

  struct cli_options {
    bool json {};
    bool help {};
  };

  struct cli_parse_result {
    bool valid {};
    cli_options options;
    std::string error;
  };

  struct app_result {
    int exit_code {};
    std::string standard_output;
    std::string standard_error;
  };

  using observation_provider = std::function<probe_observation()>;

  interface_table make_controller_interface_table();
  std::span<const method_descriptor> controller_method_layout();
  interface_validation validate_interface_table(
    const interface_table &table,
    const module_image &image);
  cli_parse_result parse_cli(std::span<const std::string_view> arguments);
  std::string help_text();
  std::string serialize_report(const probe_observation &observation);
  app_result run_probe(
    std::span<const std::string_view> arguments,
    const observation_provider &provider);
  probe_observation collect_windows_observation();
}  // namespace nvidia_nvaudcap_probe

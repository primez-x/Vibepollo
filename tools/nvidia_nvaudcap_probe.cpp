#include "tools/nvidia_nvaudcap_probe.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {
  using nvidia_nvaudcap_probe::method_descriptor;
  using nvidia_nvaudcap_probe::method_evidence;
  using ordered_json = nlohmann::ordered_json;

  // Method names through 0x138 are anchored either by the wrapper's embedded
  // diagnostics or by a direct leaf thunk into the named implementation.
  // Three context-era slots have no embedded name in this build, so their
  // semantic confidence is stated instead of promoted to a false ABI fact.
  constexpr std::array k_controller_method_layout {
    method_descriptor {0x008U, "NvAudCapOpenSession", method_evidence::embedded_name},
    method_descriptor {0x010U, "NvAudCapRegisterEndpoint", method_evidence::embedded_name},
    method_descriptor {0x018U, "NvAudCapCheckDefaultEndpoint", method_evidence::embedded_name},
    method_descriptor {0x020U, "NvAudCapSetDefaultEndPoint", method_evidence::embedded_name},
    method_descriptor {0x028U, "NvAudCapStartAudioRedirection_v4", method_evidence::embedded_name},
    method_descriptor {0x030U, "NvAudCapGetFormat", method_evidence::embedded_name},
    method_descriptor {0x038U, "NvAudCapGetAudioFrame_v2", method_evidence::direct_thunk},
    method_descriptor {0x040U, "NvAudCapStopAudioRedirection", method_evidence::direct_thunk},
    method_descriptor {0x048U, "NvAudCapUnregisterEndpoint", method_evidence::direct_thunk},
    method_descriptor {0x050U, "NvAudCapCloseSession", method_evidence::embedded_name},
    method_descriptor {0x058U, "NvAudCapGetMicVolume", method_evidence::embedded_name},
    method_descriptor {0x060U, "NvAudCapGetMicMute", method_evidence::embedded_name},
    method_descriptor {0x068U, "NvAudCapGetMicAmplification", method_evidence::embedded_name},
    method_descriptor {0x070U, "NvAudCapGetMicCount", method_evidence::embedded_name},
    method_descriptor {0x078U, "NvAudCapGetMicList", method_evidence::embedded_name},
    method_descriptor {0x080U, "NvAudCapGetDefaultMicInfo", method_evidence::embedded_name},
    method_descriptor {0x088U, "NvAudCapSetMicVolume", method_evidence::direct_thunk},
    method_descriptor {0x090U, "NvAudCapSetMicMute", method_evidence::direct_thunk},
    method_descriptor {0x098U, "NvAudCapSetMicAmplification", method_evidence::direct_thunk},
    method_descriptor {0x0A0U, "NvAudCapSelectCapMic", method_evidence::embedded_name},
    method_descriptor {0x0A8U, "NvAudCapGetLoopbackAndMicAudioFrames", method_evidence::direct_thunk},
    method_descriptor {0x0B0U, "NvAudCapRegisterEndpointChangeCallback", method_evidence::embedded_name},
    method_descriptor {0x0B8U, "NvAudCapUnRegisterEndpointChangeCallback", method_evidence::embedded_name},
    method_descriptor {0x0C0U, "NvAudCapCheckDefaultPhysicalID", method_evidence::embedded_name},
    method_descriptor {0x0C8U, "NvAudCapMuteRenderEndpoint", method_evidence::embedded_name},
    method_descriptor {0x0D0U, "NvAudCapGetRenderAudioVolume", method_evidence::embedded_name},
    method_descriptor {0x0D8U, "NvAudCapSetRenderAudioVolume", method_evidence::direct_thunk},
    method_descriptor {0x0E0U, "NvAudCapSetEndpointChannelConfig", method_evidence::embedded_name},
    method_descriptor {0x0E8U, "NvAudCapStartMicAudioPush", method_evidence::embedded_name},
    method_descriptor {0x0F0U, "NvAudCapStopMicAudioPush", method_evidence::embedded_name},
    method_descriptor {0x0F8U, "NvAudCapPushMicFrame", method_evidence::embedded_name},
    method_descriptor {0x100U, "NvAudCapCreateContext", method_evidence::embedded_name},
    method_descriptor {0x108U, "NvAudCapRegisterNvVADEndpoint", method_evidence::embedded_name},
    method_descriptor {0x110U, "NvAudCapUnRegisterNvVADEndpoint", method_evidence::embedded_name},
    method_descriptor {0x118U, "NvAudCapConfigureSessions", method_evidence::embedded_name},
    method_descriptor {0x120U, "NvAudCapRegisterSpecificEndpointChangeCallback", method_evidence::embedded_name},
    method_descriptor {0x128U, "NvAudCapGetMixedAudioFrame", method_evidence::embedded_name},
    method_descriptor {0x130U, "NvAudCapStartAudioCapture", method_evidence::embedded_name},
    method_descriptor {0x138U, "NvAudCapStopAudioCapture", method_evidence::behavior_inferred},
    method_descriptor {0x140U, "opaque_context_cleanup", method_evidence::unresolved},
    method_descriptor {0x148U, "opaque_context_destroy", method_evidence::behavior_inferred},
    method_descriptor {0x150U, "NvAudCapGetParam", method_evidence::embedded_name},
    method_descriptor {0x158U, "NvAudCapSetParam", method_evidence::embedded_name},
    method_descriptor {0x160U, "opaque_track_release", method_evidence::behavior_inferred},
  };

  static_assert(k_controller_method_layout.size() ==
                nvidia_nvaudcap_probe::controller_method_count);

  std::string hex_value(const std::uint64_t value, const int width) {
    std::ostringstream output;
    output << "0x" << std::uppercase << std::hex << std::setfill('0')
           << std::setw(width) << value;
    return output.str();
  }

  const char *evidence_name(const method_evidence value) {
    switch (value) {
      case method_evidence::embedded_name: return "embedded_name";
      case method_evidence::direct_thunk: return "direct_thunk";
      case method_evidence::behavior_inferred: return "behavior_inferred";
      case method_evidence::unresolved: return "unresolved";
    }
    return "unresolved";
  }

  const char *pointer_state_name(
    const nvidia_nvaudcap_probe::pointer_state value) {
    using nvidia_nvaudcap_probe::pointer_state;
    switch (value) {
      case pointer_state::executable: return "executable";
      case pointer_state::null_pointer: return "null";
      case pointer_state::outside_module: return "outside_module";
      case pointer_state::non_executable: return "non_executable";
    }
    return "unknown";
  }
}  // namespace

namespace nvidia_nvaudcap_probe {
  interface_table make_controller_interface_table() {
    interface_table table {};
    table.version = controller_interface_version;
    return table;
  }

  std::span<const method_descriptor> controller_method_layout() {
    return k_controller_method_layout;
  }

  interface_validation validate_interface_table(
    const interface_table &table,
    const module_image &image) {
    interface_validation result {
      .valid = true,
      .methods = {},
    };
    result.methods.reserve(table.methods.size());

    const auto module_end = image.base <= UINTPTR_MAX - image.size
      ? image.base + image.size
      : UINTPTR_MAX;
    for (std::size_t index = 0; index < table.methods.size(); ++index) {
      const auto address = table.methods[index];
      pointer_state state = pointer_state::executable;
      if (address == 0) {
        state = pointer_state::null_pointer;
      } else if (address < image.base || address >= module_end) {
        state = pointer_state::outside_module;
      } else {
        const bool executable = std::ranges::any_of(
          image.executable_ranges,
          [address](const address_range &range) {
            return range.begin <= address && address < range.end;
          });
        if (!executable) {
          state = pointer_state::non_executable;
        }
      }
      if (state != pointer_state::executable) {
        result.valid = false;
      }
      result.methods.push_back(method_observation {
        .descriptor = k_controller_method_layout[index],
        .address = address,
        .state = state,
      });
    }
    if (table.version != controller_interface_version || image.base == 0 ||
        image.size == 0 || image.executable_ranges.empty()) {
      result.valid = false;
    }
    return result;
  }

  cli_parse_result parse_cli(
    const std::span<const std::string_view> arguments) {
    cli_options options {};
    for (const auto argument : arguments) {
      if (argument == "--json") {
        options.json = true;
      } else if (argument == "--help") {
        options.help = true;
      } else {
        return {
          .valid = false,
          .options = {},
          .error = "unknown option: " + std::string {argument},
        };
      }
    }
    return {
      .valid = true,
      .options = options,
      .error = {},
    };
  }

  std::string help_text() {
    return
      "Usage: nvidia-nvaudcap-probe [--json]\n"
      "Loads System32\\nvaudcap64v.dll, verifies its Authenticode signature, "
      "and calls only NvAudCapAPICreateInstance for NVIDIA interface 1.9.\n"
      "It never invokes a returned method, registers an endpoint, starts "
      "capture, or changes the Windows audio configuration.\n";
  }

  std::string serialize_report(const probe_observation &observation) {
    ordered_json methods = ordered_json::array();
    if (!observation.validation.methods.empty()) {
      for (const auto &method : observation.validation.methods) {
        methods.push_back({
          {"offset", hex_value(method.descriptor.offset, 3)},
          {"name", method.descriptor.name},
          {"name_evidence", evidence_name(method.descriptor.evidence)},
          {"address", hex_value(method.address, sizeof(void *) * 2)},
          {"pointer_state", pointer_state_name(method.state)},
        });
      }
    } else {
      const auto layout = controller_method_layout();
      for (std::size_t index = 0; index < layout.size(); ++index) {
        methods.push_back({
          {"offset", hex_value(layout[index].offset, 3)},
          {"name", layout[index].name},
          {"name_evidence", evidence_name(layout[index].evidence)},
          {"address", hex_value(observation.table.methods[index], sizeof(void *) * 2)},
          {"pointer_state", observation.table.methods[index] == 0 ? "null" : "not_validated"},
        });
      }
    }

    ordered_json report {
      {"schema_version", 1},
      {"probe_kind", "factory_only"},
      {"endpoint_mutation_attempted", false},
      {"audio_capture_started", false},
      {"interface_methods_invoked", false},
      {"complete", observation.complete},
      {"dll", {
        {"path", observation.dll_path},
        {"file_version", observation.file_version},
        {"signature_valid", observation.signature_valid},
        {"signature_status", hex_value(observation.signature_status, 8)},
        {"module_base", hex_value(observation.module_base, sizeof(void *) * 2)},
      }},
      {"factory", {
        {"export", "NvAudCapAPICreateInstance"},
        {"address", hex_value(observation.factory_address, sizeof(void *) * 2)},
        {"result", observation.factory_result},
      }},
      {"interface", {
        {"requested_version", hex_value(controller_interface_version, 8)},
        {"returned_version", hex_value(observation.table.version, 8)},
        {"table_size", controller_interface_size},
        {"method_pointer_count", controller_method_count},
        {"all_method_pointers_executable", observation.validation.valid},
        {"methods", std::move(methods)},
      }},
      {"errors", observation.errors},
    };
    return report.dump();
  }

  app_result run_probe(
    const std::span<const std::string_view> arguments,
    const observation_provider &provider) {
    const auto parsed = parse_cli(arguments);
    if (!parsed.valid) {
      return {
        .exit_code = 2,
        .standard_output = {},
        .standard_error = parsed.error + "\n",
      };
    }
    if (parsed.options.help) {
      return {
        .exit_code = 0,
        .standard_output = help_text(),
        .standard_error = {},
      };
    }

    probe_observation observation {};
    try {
      observation = provider();
    } catch (...) {
      observation.errors.emplace_back("observation provider threw an exception");
    }
    const auto output = parsed.options.json
      ? serialize_report(observation) + "\n"
      : (observation.complete ? "SUPPORTED\n" : "UNAVAILABLE\n") +
          serialize_report(observation) + "\n";
    return {
      .exit_code = observation.complete ? 0 : 3,
      .standard_output = output,
      .standard_error = {},
    };
  }
}  // namespace nvidia_nvaudcap_probe

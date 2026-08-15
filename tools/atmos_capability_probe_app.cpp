#include "tools/atmos_capability_probe_app.h"

#include "tools/atmos_capability_probe_cli.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
  using atmos_probe::api_error;
  using atmos_probe::diagnostic;
  using atmos_probe::endpoint_observation;
  using atmos_probe::form_factor_observation;
  using atmos_probe::gate_result;
  using atmos_probe::hresult_code;
  using atmos_probe::jack_subtype_observation;
  using atmos_probe::mat_observation;
  using atmos_probe::mat_profile;
  using atmos_probe::probe_observation;
  using atmos_probe::probe_options;
  using ordered_json = nlohmann::ordered_json;

  constexpr std::string_view k_atmos_home_theater_guid =
    "{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
  constexpr std::string_view k_preflight_caveat =
    "Endpoint preflight does not prove downstream receiver Atmos lock.";
  constexpr std::uint32_t k_s_ok = 0x00000000U;
  constexpr std::uint32_t k_audclnt_e_device_invalidated = 0x88890004U;
  constexpr std::uint32_t k_audclnt_e_unsupported_format = 0x88890008U;
  constexpr std::uint32_t k_audclnt_e_device_in_use = 0x8889000AU;
  constexpr std::uint32_t k_audclnt_e_exclusive_mode_not_allowed = 0x8889000EU;

  std::uint32_t hresult_bits(const hresult_code value) {
    return static_cast<std::uint32_t>(value);
  }

  std::string hresult_text(const std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << value;
    return stream.str();
  }

  std::string_view hresult_label(const std::uint32_t value) {
    switch (value) {
      case k_s_ok:
        return "S_OK";
      case k_audclnt_e_device_in_use:
        return "AUDCLNT_E_DEVICE_IN_USE";
      case k_audclnt_e_exclusive_mode_not_allowed:
        return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
      case k_audclnt_e_device_invalidated:
        return "AUDCLNT_E_DEVICE_INVALIDATED";
      case k_audclnt_e_unsupported_format:
        return "AUDCLNT_E_UNSUPPORTED_FORMAT";
      default:
        return "UNKNOWN_HRESULT";
    }
  }

  ordered_json serialize_optional_hresult(const std::optional<hresult_code> &value) {
    if (!value) {
      return nullptr;
    }
    return hresult_text(hresult_bits(*value));
  }

  std::string_view optional_hresult_label(const std::optional<hresult_code> &value) {
    return value ? hresult_label(hresult_bits(*value)) : "NOT_PROBED";
  }

  bool is_ready(const mat_observation &observation) {
    return observation.format_support_hresult && observation.initialize_hresult &&
           hresult_bits(*observation.format_support_hresult) == k_s_ok &&
           hresult_bits(*observation.initialize_hresult) == k_s_ok;
  }

  std::string_view state_name(const std::uint32_t state) {
    switch (state) {
      case 1:
        return "ACTIVE";
      case 2:
        return "DISABLED";
      case 4:
        return "NOT_PRESENT";
      case 8:
        return "UNPLUGGED";
      default:
        return "UNKNOWN";
    }
  }

  ordered_json serialize_form_factor(const form_factor_observation &value) {
    return std::visit(
      [](const auto &observation) -> ordered_json {
        using observation_type = std::decay_t<decltype(observation)>;
        if constexpr (std::is_same_v<observation_type, atmos_probe::property_missing>) {
          return {{"status", "MISSING"}};
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::property_wrong_type>) {
          return {
            {"status", "WRONG_TYPE"},
            {"variant_type", observation.variant_type},
          };
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::display_audio_form_factor>) {
          return {{"status", "DISPLAY_AUDIO"}};
        } else {
          return {
            {"status", "OTHER"},
            {"value", observation.value},
          };
        }
      },
      value);
  }

  ordered_json serialize_jack_subtype(const jack_subtype_observation &value) {
    return std::visit(
      [](const auto &observation) -> ordered_json {
        using observation_type = std::decay_t<decltype(observation)>;
        if constexpr (std::is_same_v<observation_type, atmos_probe::property_missing>) {
          return {{"status", "MISSING"}};
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::property_wrong_type>) {
          return {
            {"status", "WRONG_TYPE"},
            {"variant_type", observation.variant_type},
          };
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::malformed_connector_guid>) {
          return {
            {"status", "MALFORMED"},
            {"raw", observation.raw},
          };
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::hdmi_connector>) {
          return {{"status", "HDMI"}};
        } else if constexpr (std::is_same_v<observation_type, atmos_probe::displayport_connector>) {
          return {{"status", "DISPLAYPORT"}};
        } else {
          return {
            {"status", "OTHER"},
            {"canonical_guid", observation.canonical_guid},
          };
        }
      },
      value);
  }

  ordered_json serialize_endpoint(const endpoint_observation &endpoint) {
    return {
      {"id", endpoint.id},
      {"friendly_name", endpoint.friendly_name},
      {"state", endpoint.state},
      {"state_name", state_name(endpoint.state)},
      {"form_factor", serialize_form_factor(endpoint.form_factor)},
      {"jack_subtype", serialize_jack_subtype(endpoint.jack_subtype)},
      {"is_display_audio", atmos_probe::is_display_audio(endpoint)},
      {"is_hdmi", atmos_probe::is_hdmi(endpoint)},
    };
  }

  ordered_json serialize_endpoint_or_null(const std::optional<endpoint_observation> &endpoint) {
    return endpoint ? serialize_endpoint(*endpoint) : ordered_json(nullptr);
  }

  const std::optional<endpoint_observation> *find_role_endpoint(
    const std::array<atmos_probe::role_endpoint_observation, 3> &endpoints,
    const std::string_view role) {
    const auto match = std::find_if(
      endpoints.begin(),
      endpoints.end(),
      [role](const auto &endpoint) {
        return endpoint.role == role;
      });
    return match == endpoints.end() ? nullptr : &match->endpoint;
  }

  ordered_json serialize_default_endpoints(const probe_observation &observation) {
    const auto value_for = [&observation](const std::string_view role) -> ordered_json {
      const auto *endpoint = find_role_endpoint(observation.default_endpoints, role);
      return endpoint ? serialize_endpoint_or_null(*endpoint) : ordered_json(nullptr);
    };
    return {
      {"console", value_for("console")},
      {"multimedia", value_for("multimedia")},
      {"communications", value_for("communications")},
    };
  }

  ordered_json serialize_active_endpoints(const probe_observation &observation) {
    auto endpoints = ordered_json::array();
    for (const auto &endpoint : observation.active_endpoints) {
      endpoints.push_back(serialize_endpoint(endpoint));
    }
    return endpoints;
  }

  ordered_json serialize_spatial_audio(const probe_observation &observation) {
    const auto &spatial = observation.spatial;
    return {
      {"configuration_available", spatial.configuration_available},
      {"is_spatial_audio_supported", spatial.spatial_audio_supported},
      {"atmos_home_theater_supported", spatial.atmos_home_theater_supported},
      {"active_format_raw", spatial.active_format_raw},
      {"active_format_guid", spatial.active_format_guid},
      {"default_format_raw", spatial.default_format_raw},
      {"default_format_guid", spatial.default_format_guid},
    };
  }

  ordered_json serialize_mat_profile(
    const mat_profile profile,
    const mat_observation &observation) {
    return {
      {"profile", atmos_probe::to_string(profile)},
      {"format_support_hresult", serialize_optional_hresult(observation.format_support_hresult)},
      {"format_support_hresult_label", optional_hresult_label(observation.format_support_hresult)},
      {"initialize_hresult", serialize_optional_hresult(observation.initialize_hresult)},
      {"initialize_hresult_label", optional_hresult_label(observation.initialize_hresult)},
      {"ready", is_ready(observation)},
    };
  }

  ordered_json serialize_mat_profiles(const probe_observation &observation) {
    return ordered_json::array({
      serialize_mat_profile(mat_profile::mat21, observation.mat21),
      serialize_mat_profile(mat_profile::mat20, observation.mat20),
    });
  }

  ordered_json serialize_errors(const std::vector<api_error> &errors) {
    auto result = ordered_json::array();
    for (const auto &error : errors) {
      const auto bits = hresult_bits(error.hresult);
      result.push_back({
        {"operation", error.operation},
        {"hresult", hresult_text(bits)},
        {"hresult_label", hresult_label(bits)},
      });
    }
    return result;
  }

  ordered_json serialize_gate(const gate_result &gate) {
    auto ready_profiles = ordered_json::array();
    for (const auto profile : gate.ready_profiles) {
      ready_profiles.push_back(atmos_probe::to_string(profile));
    }
    auto diagnostics = ordered_json::array();
    for (const auto diagnostic_value : gate.diagnostics) {
      diagnostics.push_back(atmos_probe::to_string(diagnostic_value));
    }
    return {
      {"verdict", gate.ready ? "ENDPOINT_PREFLIGHT_READY" : "BLOCKED"},
      {"selected_profile", gate.selected_profile ?
        ordered_json(std::string {atmos_probe::to_string(*gate.selected_profile)}) : ordered_json(nullptr)},
      {"ready_profiles", std::move(ready_profiles)},
      {"diagnostics", std::move(diagnostics)},
    };
  }

  ordered_json serialize_selection(const probe_options &options) {
    if (options.endpoint_id) {
      return {
        {"kind", "explicit"},
        {"requested_endpoint_id", *options.endpoint_id},
      };
    }
    return {
      {"kind", "default:eConsole"},
      {"requested_endpoint_id", nullptr},
    };
  }

  ordered_json serialize_report(
    const probe_options &options,
    const probe_observation &observation,
    const gate_result &gate) {
    return {
      {"schema_version", 1},
      {"selection", serialize_selection(options)},
      {"selected_endpoint", serialize_endpoint_or_null(observation.selected_endpoint)},
      {"default_render_endpoints", serialize_default_endpoints(observation)},
      {"active_render_endpoints", serialize_active_endpoints(observation)},
      {"spatial_audio", serialize_spatial_audio(observation)},
      {"mat_profiles", serialize_mat_profiles(observation)},
      {"probe_errors", serialize_errors(observation.errors)},
      {"gate", serialize_gate(gate)},
      {"audio_bytes_written", false},
    };
  }

  std::string render_human_output(
    const probe_observation &observation,
    const gate_result &gate) {
    std::ostringstream stream;
    if (gate.ready && gate.selected_profile) {
      stream << "ENDPOINT_PREFLIGHT_READY: " << atmos_probe::to_string(*gate.selected_profile);
    } else {
      stream << "BLOCKED";
    }
    stream << "\nSelected endpoint: ";
    if (observation.selected_endpoint) {
      stream << observation.selected_endpoint->id;
      if (!observation.selected_endpoint->friendly_name.empty()) {
        stream << " (" << observation.selected_endpoint->friendly_name << ')';
      }
    } else {
      stream << "<none>";
    }
    stream << "\nDiagnostics:";
    if (gate.diagnostics.empty()) {
      stream << " none";
    } else {
      for (const auto diagnostic_value : gate.diagnostics) {
        stream << "\n- " << atmos_probe::to_string(diagnostic_value);
      }
    }
    stream << "\n" << k_preflight_caveat << "\n";
    return stream.str();
  }

  std::string help_text() {
    return "Usage: atmos-capability-probe [--json] [--endpoint-id <id>] [--help]\n"
           "--endpoint-id selects one exact render endpoint ID; without it the probe uses eConsole.\n" +
           std::string {k_preflight_caveat} + "\n";
  }

  std::optional<std::string> expect_object_keys(
    const ordered_json &value,
    const std::initializer_list<std::string_view> expected_keys,
    const std::string_view path) {
    if (!value.is_object()) {
      return std::string {path} + " must be an object";
    }
    if (value.size() != expected_keys.size()) {
      return std::string {path} + " has an unexpected key count";
    }
    auto iterator = value.begin();
    for (const auto expected_key : expected_keys) {
      if (iterator == value.end() || iterator.key() != expected_key) {
        return std::string {path} + " has an unexpected key order";
      }
      ++iterator;
    }
    return std::nullopt;
  }

  std::optional<std::string> expect_string(
    const ordered_json &value,
    const std::string_view path) {
    if (!value.is_string()) {
      return std::string {path} + " must be a string";
    }
    return std::nullopt;
  }

  std::optional<std::string> expect_boolean(
    const ordered_json &value,
    const std::string_view path) {
    if (!value.is_boolean()) {
      return std::string {path} + " must be a boolean";
    }
    return std::nullopt;
  }

  std::optional<std::string> expect_unsigned(
    const ordered_json &value,
    const std::uint64_t maximum,
    const std::string_view path) {
    if (!value.is_number_unsigned() && !value.is_number_integer()) {
      return std::string {path} + " must be an integer";
    }
    if (value.is_number_integer() && value.get<std::int64_t>() < 0) {
      return std::string {path} + " must be non-negative";
    }
    if (value.get<std::uint64_t>() > maximum) {
      return std::string {path} + " is out of range";
    }
    return std::nullopt;
  }

  bool is_upper_hex_digit(const char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
  }

  std::optional<std::uint32_t> parse_hresult_text(const std::string_view value) {
    if (value.size() != 10 || value.substr(0, 2) != "0x") {
      return std::nullopt;
    }
    std::uint32_t result {};
    for (const char digit : value.substr(2)) {
      if (!is_upper_hex_digit(digit)) {
        return std::nullopt;
      }
      const auto nibble = static_cast<std::uint32_t>(
        digit <= '9' ? digit - '0' : digit - 'A' + 10);
      result = (result << 4U) | nibble;
    }
    return result;
  }

  std::optional<std::string> validate_optional_hresult(
    const ordered_json &value,
    const ordered_json &label,
    std::optional<std::uint32_t> &bits,
    const std::string_view path) {
    if (value.is_null()) {
      if (!label.is_string() || label.get<std::string>() != "NOT_PROBED") {
        return std::string {path} + " has an inconsistent not-probed label";
      }
      bits.reset();
      return std::nullopt;
    }
    if (const auto error = expect_string(value, path)) {
      return error;
    }
    if (const auto error = expect_string(label, std::string {path} + "_label")) {
      return error;
    }
    const auto parsed = parse_hresult_text(value.get<std::string>());
    if (!parsed) {
      return std::string {path} + " must be a fixed-width uppercase HRESULT";
    }
    if (label.get<std::string>() != hresult_label(*parsed)) {
      return std::string {path} + " has an inconsistent HRESULT label";
    }
    bits = *parsed;
    return std::nullopt;
  }

  std::optional<std::string> validate_form_factor(
    const ordered_json &value,
    const std::string_view path) {
    if (const auto error = expect_object_keys(value, {"status"}, path)) {
      if (!value.is_object() || !value.contains("status") || !value.at("status").is_string()) {
        return error;
      }
    }
    if (const auto error = expect_string(value.at("status"), std::string {path} + ".status")) {
      return error;
    }
    const auto status = value.at("status").get<std::string>();
    if (status == "MISSING" || status == "DISPLAY_AUDIO") {
      return expect_object_keys(value, {"status"}, path);
    }
    if (status == "WRONG_TYPE") {
      if (const auto error = expect_object_keys(value, {"status", "variant_type"}, path)) {
        return error;
      }
      return expect_unsigned(value.at("variant_type"), 0xFFFFU, std::string {path} + ".variant_type");
    }
    if (status == "OTHER") {
      if (const auto error = expect_object_keys(value, {"status", "value"}, path)) {
        return error;
      }
      return expect_unsigned(value.at("value"), 0xFFFFFFFFU, std::string {path} + ".value");
    }
    return std::string {path} + " has an unknown status";
  }

  std::optional<std::string> validate_jack_subtype(
    const ordered_json &value,
    const std::string_view path) {
    if (const auto error = expect_object_keys(value, {"status"}, path)) {
      if (!value.is_object() || !value.contains("status") || !value.at("status").is_string()) {
        return error;
      }
    }
    if (const auto error = expect_string(value.at("status"), std::string {path} + ".status")) {
      return error;
    }
    const auto status = value.at("status").get<std::string>();
    if (status == "MISSING" || status == "HDMI" || status == "DISPLAYPORT") {
      return expect_object_keys(value, {"status"}, path);
    }
    if (status == "WRONG_TYPE") {
      if (const auto error = expect_object_keys(value, {"status", "variant_type"}, path)) {
        return error;
      }
      return expect_unsigned(value.at("variant_type"), 0xFFFFU, std::string {path} + ".variant_type");
    }
    if (status == "MALFORMED") {
      if (const auto error = expect_object_keys(value, {"status", "raw"}, path)) {
        return error;
      }
      return expect_string(value.at("raw"), std::string {path} + ".raw");
    }
    if (status == "OTHER") {
      if (const auto error = expect_object_keys(value, {"status", "canonical_guid"}, path)) {
        return error;
      }
      return expect_string(value.at("canonical_guid"), std::string {path} + ".canonical_guid");
    }
    return std::string {path} + " has an unknown status";
  }

  std::optional<std::string> validate_endpoint(
    const ordered_json &endpoint,
    const std::string_view path) {
    if (const auto error = expect_object_keys(
          endpoint,
          {
            "id",
            "friendly_name",
            "state",
            "state_name",
            "form_factor",
            "jack_subtype",
            "is_display_audio",
            "is_hdmi",
          },
          path)) {
      return error;
    }
    if (const auto error = expect_string(endpoint.at("id"), std::string {path} + ".id")) {
      return error;
    }
    if (const auto error = expect_string(endpoint.at("friendly_name"), std::string {path} + ".friendly_name")) {
      return error;
    }
    if (const auto error = expect_unsigned(endpoint.at("state"), 0xFFFFFFFFU, std::string {path} + ".state")) {
      return error;
    }
    if (const auto error = expect_string(endpoint.at("state_name"), std::string {path} + ".state_name")) {
      return error;
    }
    const auto state = endpoint.at("state").get<std::uint32_t>();
    if (endpoint.at("state_name").get<std::string>() != state_name(state)) {
      return std::string {path} + " has an inconsistent state_name";
    }
    if (const auto error = validate_form_factor(endpoint.at("form_factor"), std::string {path} + ".form_factor")) {
      return error;
    }
    if (const auto error = validate_jack_subtype(endpoint.at("jack_subtype"), std::string {path} + ".jack_subtype")) {
      return error;
    }
    if (const auto error = expect_boolean(endpoint.at("is_display_audio"), std::string {path} + ".is_display_audio")) {
      return error;
    }
    if (const auto error = expect_boolean(endpoint.at("is_hdmi"), std::string {path} + ".is_hdmi")) {
      return error;
    }
    const auto display_audio =
      endpoint.at("form_factor").at("status").get<std::string>() == "DISPLAY_AUDIO";
    const auto hdmi = endpoint.at("jack_subtype").at("status").get<std::string>() == "HDMI";
    if (endpoint.at("is_display_audio").get<bool>() != display_audio) {
      return std::string {path} + " has an inconsistent is_display_audio value";
    }
    if (endpoint.at("is_hdmi").get<bool>() != hdmi) {
      return std::string {path} + " has an inconsistent is_hdmi value";
    }
    return std::nullopt;
  }

  struct validated_mat_profile {
    std::string profile;
    std::optional<std::uint32_t> format_support_hresult;
    std::optional<std::uint32_t> initialize_hresult;
    bool ready {};
  };

  std::optional<std::string> validate_mat_profile(
    const ordered_json &value,
    const std::string_view expected_profile,
    validated_mat_profile &result,
    const std::string_view path) {
    if (const auto error = expect_object_keys(
          value,
          {
            "profile",
            "format_support_hresult",
            "format_support_hresult_label",
            "initialize_hresult",
            "initialize_hresult_label",
            "ready",
          },
          path)) {
      return error;
    }
    if (const auto error = expect_string(value.at("profile"), std::string {path} + ".profile")) {
      return error;
    }
    result.profile = value.at("profile").get<std::string>();
    if (result.profile != expected_profile) {
      return std::string {path} + " has an unexpected profile";
    }
    if (const auto error = validate_optional_hresult(
          value.at("format_support_hresult"),
          value.at("format_support_hresult_label"),
          result.format_support_hresult,
          std::string {path} + ".format_support_hresult")) {
      return error;
    }
    if (const auto error = validate_optional_hresult(
          value.at("initialize_hresult"),
          value.at("initialize_hresult_label"),
          result.initialize_hresult,
          std::string {path} + ".initialize_hresult")) {
      return error;
    }
    if (const auto error = expect_boolean(value.at("ready"), std::string {path} + ".ready")) {
      return error;
    }
    result.ready = value.at("ready").get<bool>();
    const auto expected_ready = result.format_support_hresult && result.initialize_hresult &&
                                *result.format_support_hresult == k_s_ok &&
                                *result.initialize_hresult == k_s_ok;
    if (result.ready != expected_ready) {
      return std::string {path} + " has an inconsistent ready value";
    }
    return std::nullopt;
  }

  std::optional<std::string> validate_report(const ordered_json &report) {
    if (const auto error = expect_object_keys(
          report,
          {
            "schema_version",
            "selection",
            "selected_endpoint",
            "default_render_endpoints",
            "active_render_endpoints",
            "spatial_audio",
            "mat_profiles",
            "probe_errors",
            "gate",
            "audio_bytes_written",
          },
          "report")) {
      return error;
    }
    if (!report.at("schema_version").is_number_integer() ||
        report.at("schema_version").get<int>() != 1) {
      return "schema_version must be 1";
    }

    const auto &selection = report.at("selection");
    if (const auto error = expect_object_keys(
          selection,
          {"kind", "requested_endpoint_id"},
          "selection")) {
      return error;
    }
    if (const auto error = expect_string(selection.at("kind"), "selection.kind")) {
      return error;
    }
    const auto selection_kind = selection.at("kind").get<std::string>();
    if (selection_kind == "explicit") {
      if (const auto error = expect_string(
            selection.at("requested_endpoint_id"),
            "selection.requested_endpoint_id")) {
        return error;
      }
    } else if (selection_kind == "default:eConsole") {
      if (!selection.at("requested_endpoint_id").is_null()) {
        return "default selection must not include a requested endpoint ID";
      }
    } else {
      return "selection.kind is invalid";
    }

    const auto &selected_endpoint = report.at("selected_endpoint");
    if (!selected_endpoint.is_null()) {
      if (const auto error = validate_endpoint(selected_endpoint, "selected_endpoint")) {
        return error;
      }
    }

    const auto &defaults = report.at("default_render_endpoints");
    if (const auto error = expect_object_keys(
          defaults,
          {"console", "multimedia", "communications"},
          "default_render_endpoints")) {
      return error;
    }
    for (const auto role : {"console", "multimedia", "communications"}) {
      const auto &endpoint = defaults.at(role);
      if (!endpoint.is_null()) {
        if (const auto error = validate_endpoint(endpoint, std::string {"default_render_endpoints."} + role)) {
          return error;
        }
      }
    }

    const auto &active_endpoints = report.at("active_render_endpoints");
    if (!active_endpoints.is_array()) {
      return "active_render_endpoints must be an array";
    }
    for (std::size_t index = 0; index < active_endpoints.size(); ++index) {
      if (const auto error = validate_endpoint(
            active_endpoints.at(index),
            "active_render_endpoints[" + std::to_string(index) + "]")) {
        return error;
      }
    }

    const auto &spatial = report.at("spatial_audio");
    if (const auto error = expect_object_keys(
          spatial,
          {
            "configuration_available",
            "is_spatial_audio_supported",
            "atmos_home_theater_supported",
            "active_format_raw",
            "active_format_guid",
            "default_format_raw",
            "default_format_guid",
          },
          "spatial_audio")) {
      return error;
    }
    for (const auto key : {
           "configuration_available",
           "is_spatial_audio_supported",
           "atmos_home_theater_supported",
         }) {
      if (const auto error = expect_boolean(spatial.at(key), std::string {"spatial_audio."} + key)) {
        return error;
      }
    }
    for (const auto key : {
           "active_format_raw",
           "active_format_guid",
           "default_format_raw",
           "default_format_guid",
         }) {
      if (const auto error = expect_string(spatial.at(key), std::string {"spatial_audio."} + key)) {
        return error;
      }
    }

    const auto &mat_profiles = report.at("mat_profiles");
    if (!mat_profiles.is_array() || mat_profiles.size() != 2) {
      return "mat_profiles must contain MAT21 followed by MAT20";
    }
    validated_mat_profile mat21 {};
    validated_mat_profile mat20 {};
    if (const auto error = validate_mat_profile(mat_profiles.at(0), "MAT21", mat21, "mat_profiles[0]")) {
      return error;
    }
    if (const auto error = validate_mat_profile(mat_profiles.at(1), "MAT20", mat20, "mat_profiles[1]")) {
      return error;
    }

    const auto &probe_errors = report.at("probe_errors");
    if (!probe_errors.is_array()) {
      return "probe_errors must be an array";
    }
    for (std::size_t index = 0; index < probe_errors.size(); ++index) {
      const auto &error = probe_errors.at(index);
      const auto path = "probe_errors[" + std::to_string(index) + "]";
      if (const auto schema_error = expect_object_keys(
            error,
            {"operation", "hresult", "hresult_label"},
            path)) {
        return schema_error;
      }
      if (const auto schema_error = expect_string(error.at("operation"), path + ".operation")) {
        return schema_error;
      }
      std::optional<std::uint32_t> error_bits;
      if (const auto schema_error = validate_optional_hresult(
            error.at("hresult"),
            error.at("hresult_label"),
            error_bits,
            path + ".hresult")) {
        return schema_error;
      }
      if (!error_bits) {
        return path + ".hresult must not be null";
      }
    }

    const auto &gate = report.at("gate");
    if (const auto error = expect_object_keys(
          gate,
          {"verdict", "selected_profile", "ready_profiles", "diagnostics"},
          "gate")) {
      return error;
    }
    if (const auto error = expect_string(gate.at("verdict"), "gate.verdict")) {
      return error;
    }
    const auto verdict = gate.at("verdict").get<std::string>();
    if (verdict != "BLOCKED" && verdict != "ENDPOINT_PREFLIGHT_READY") {
      return "gate.verdict is invalid";
    }
    if (!gate.at("selected_profile").is_null()) {
      if (const auto error = expect_string(gate.at("selected_profile"), "gate.selected_profile")) {
        return error;
      }
      const auto selected_profile = gate.at("selected_profile").get<std::string>();
      if (selected_profile != "MAT21" && selected_profile != "MAT20") {
        return "gate.selected_profile is invalid";
      }
    }
    if (!gate.at("ready_profiles").is_array()) {
      return "gate.ready_profiles must be an array";
    }
    std::vector<std::string> ready_profiles;
    for (const auto &profile : gate.at("ready_profiles")) {
      if (const auto error = expect_string(profile, "gate.ready_profiles[]")) {
        return error;
      }
      const auto name = profile.get<std::string>();
      if (name != "MAT21" && name != "MAT20") {
        return "gate.ready_profiles contains an invalid profile";
      }
      ready_profiles.push_back(name);
    }
    const std::vector<std::string> expected_ready_profiles = [&mat21, &mat20]() {
      std::vector<std::string> result;
      if (mat21.ready) {
        result.emplace_back("MAT21");
      }
      if (mat20.ready) {
        result.emplace_back("MAT20");
      }
      return result;
    }();
    if (ready_profiles != expected_ready_profiles) {
      return "gate.ready_profiles is inconsistent with MAT observations";
    }
    const auto expected_selected_profile = expected_ready_profiles.empty() ?
      std::optional<std::string> {} : std::optional<std::string> {expected_ready_profiles.front()};
    if (expected_selected_profile) {
      if (gate.at("selected_profile").is_null() ||
          gate.at("selected_profile").get<std::string>() != *expected_selected_profile) {
        return "gate.selected_profile is inconsistent with MAT observations";
      }
    } else if (!gate.at("selected_profile").is_null()) {
      return "gate.selected_profile must be null without ready profiles";
    }
    if (!gate.at("diagnostics").is_array()) {
      return "gate.diagnostics must be an array";
    }
    for (const auto &diagnostic_value : gate.at("diagnostics")) {
      if (const auto error = expect_string(diagnostic_value, "gate.diagnostics[]")) {
        return error;
      }
    }

    if (const auto error = expect_boolean(report.at("audio_bytes_written"), "audio_bytes_written")) {
      return error;
    }
    if (report.at("audio_bytes_written").get<bool>()) {
      return "audio_bytes_written must be false";
    }

    if (verdict != "ENDPOINT_PREFLIGHT_READY") {
      return std::nullopt;
    }
    if (selected_endpoint.is_null()) {
      return "a green report requires a selected endpoint";
    }
    if (selection_kind == "explicit" &&
        selected_endpoint.at("id").get<std::string>() !=
          selection.at("requested_endpoint_id").get<std::string>()) {
      return "an explicit request must match selected_endpoint.id";
    }
    if (selected_endpoint.at("state").get<std::uint32_t>() != 1U) {
      return "a green report requires an active endpoint";
    }
    if (selected_endpoint.at("form_factor").at("status").get<std::string>() != "DISPLAY_AUDIO") {
      return "a green report requires DISPLAY_AUDIO";
    }
    if (selected_endpoint.at("jack_subtype").at("status").get<std::string>() != "HDMI") {
      return "a green report requires HDMI";
    }
    if (!spatial.at("configuration_available").get<bool>() ||
        !spatial.at("is_spatial_audio_supported").get<bool>() ||
        !spatial.at("atmos_home_theater_supported").get<bool>() ||
        spatial.at("active_format_guid").get<std::string>() != k_atmos_home_theater_guid) {
      return "a green report requires the exact active Atmos Home Theater state";
    }
    if (!probe_errors.empty()) {
      return "a green report cannot contain probe errors";
    }
    if (!gate.at("diagnostics").empty()) {
      return "a green report cannot contain diagnostics";
    }
    if (!expected_selected_profile ||
        std::find(ready_profiles.begin(), ready_profiles.end(), *expected_selected_profile) == ready_profiles.end()) {
      return "a green report requires selected profile ready membership";
    }
    const auto &selected_mat = *expected_selected_profile == "MAT21" ? mat21 : mat20;
    if (!selected_mat.ready || selected_mat.format_support_hresult != k_s_ok ||
        selected_mat.initialize_hresult != k_s_ok) {
      return "a green report requires S_OK support and initialization for its selected MAT profile";
    }
    return std::nullopt;
  }
}  // namespace

namespace atmos_probe {
  report_validation_result validate_serialized_report(const std::string_view json) {
    try {
      const auto report = ordered_json::parse(json);
      if (const auto error = validate_report(report)) {
        return {.valid = false, .error = *error};
      }
      return {.valid = true};
    } catch (const std::exception &error) {
      return {.valid = false, .error = "invalid JSON report: " + std::string {error.what()}};
    }
  }

  app_result run_probe(
    const std::span<const std::string_view> arguments,
    const observation_provider &provider) {
    const auto parsed = parse_cli(arguments);
    if (!parsed.options) {
      return {
        .exit_code = 2,
        .standard_error = parsed.error + "\n",
      };
    }
    if (parsed.options->help) {
      return {
        .exit_code = 0,
        .standard_output = help_text(),
      };
    }

    probe_options options {};
    probe_observation observation {};
    try {
      options.endpoint_id = parsed.options->endpoint_id;
      observation = provider(options);
    } catch (const std::exception &error) {
      return {
        .exit_code = 3,
        .standard_error = "provider/runtime failure: " + std::string {error.what()} + "\n",
      };
    } catch (...) {
      return {
        .exit_code = 3,
        .standard_error = "provider/runtime failure: unknown exception\n",
      };
    }

    gate_result gate {};
    std::string serialized_report;
    try {
      gate = evaluate(observation);
      serialized_report = serialize_report(options, observation, gate).dump();
      const auto validation = validate_serialized_report(serialized_report);
      if (!validation.valid) {
        return {
          .exit_code = 3,
          .standard_error = "report validation failure: " + validation.error + "\n",
        };
      }
    } catch (const std::exception &error) {
      return {
        .exit_code = 3,
        .standard_error = "report/runtime failure: " + std::string {error.what()} + "\n",
      };
    } catch (...) {
      return {
        .exit_code = 3,
        .standard_error = "report/runtime failure: unknown exception\n",
      };
    }

    const auto exit_code = gate.ready ? 0 : 1;
    if (parsed.options->json) {
      return {
        .exit_code = exit_code,
        .standard_output = serialized_report + "\n",
        .standard_error = std::string {k_preflight_caveat} + "\n",
      };
    }
    return {
      .exit_code = exit_code,
      .standard_output = render_human_output(observation, gate),
    };
  }
}  // namespace atmos_probe

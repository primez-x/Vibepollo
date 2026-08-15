#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace atmos_probe {
  using hresult_code = std::int32_t;

  enum class mat_profile { mat20, mat21 };

  struct probe_options {
    std::optional<std::string> endpoint_id;
  };

  enum class diagnostic {
    probe_runtime_error,
    selected_endpoint_not_found,
    selected_endpoint_not_active,
    selected_endpoint_not_display_audio,
    selected_endpoint_not_hdmi,
    spatial_configuration_unavailable,
    spatial_audio_unsupported,
    atmos_home_theater_unsupported,
    active_spatial_format_not_atmos_home_theater,
    mat20_exclusive_probe_failed,
    mat20_exclusive_format_unsupported,
    mat20_exclusive_initialize_failed,
    mat21_exclusive_probe_failed,
    mat21_exclusive_format_unsupported,
    mat21_exclusive_initialize_failed,
    no_exclusive_mat_profile_ready,
  };

  struct property_missing {};
  struct property_wrong_type {
    std::uint16_t variant_type;
  };
  struct display_audio_form_factor {};
  struct other_form_factor {
    std::uint32_t value;
  };
  using form_factor_observation = std::variant<
    property_missing,
    property_wrong_type,
    display_audio_form_factor,
    other_form_factor>;

  struct malformed_connector_guid {
    std::string raw;
  };
  struct hdmi_connector {};
  struct displayport_connector {};
  struct other_connector_guid {
    std::string canonical_guid;
  };
  using jack_subtype_observation = std::variant<
    property_missing,
    property_wrong_type,
    malformed_connector_guid,
    hdmi_connector,
    displayport_connector,
    other_connector_guid>;

  struct endpoint_observation {
    std::string id;
    std::string friendly_name;
    std::uint32_t state {};
    form_factor_observation form_factor {property_missing {}};
    jack_subtype_observation jack_subtype {property_missing {}};
  };

  struct role_endpoint_observation {
    std::string role;
    std::optional<endpoint_observation> endpoint;
  };

  struct spatial_observation {
    bool configuration_available {};
    bool spatial_audio_supported {};
    bool atmos_home_theater_supported {};
    std::string active_format_raw;
    std::string active_format_guid;
    std::string default_format_raw;
    std::string default_format_guid;
  };

  struct mat_observation {
    std::optional<hresult_code> format_support_hresult;
    std::optional<hresult_code> initialize_hresult;
  };

  struct api_error {
    std::string operation;
    hresult_code hresult;
  };

  struct probe_observation {
    bool probe_complete {true};
    std::optional<endpoint_observation> selected_endpoint;
    std::array<role_endpoint_observation, 3> default_endpoints;
    std::vector<endpoint_observation> active_endpoints;
    spatial_observation spatial;
    mat_observation mat21;
    mat_observation mat20;
    std::vector<api_error> errors;
  };

  struct gate_result {
    bool ready {};
    std::optional<mat_profile> selected_profile;
    std::vector<mat_profile> ready_profiles;
    std::vector<diagnostic> diagnostics;
  };

  gate_result evaluate(const probe_observation &observation);
  bool is_active(const endpoint_observation &endpoint);
  bool is_display_audio(const endpoint_observation &endpoint);
  bool is_hdmi(const endpoint_observation &endpoint);
  std::string_view to_string(mat_profile profile);
  std::string_view to_string(diagnostic value);
}  // namespace atmos_probe

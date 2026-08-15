#include "tools/atmos_capability_probe_policy.h"

#include <algorithm>

namespace {
  constexpr std::uint32_t device_state_active = 1;
  constexpr atmos_probe::hresult_code s_ok = 0;
  constexpr std::string_view atmos_home_theater_guid =
    "{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
  constexpr std::string_view winrt_default_link_source =
    "winrt_default_and_communications";

  enum class mat_readiness {
    ready,
    probe_failed,
    format_unsupported,
    initialize_failed,
  };

  mat_readiness evaluate_mat(const atmos_probe::mat_observation &observation) {
    if (!observation.format_support_hresult) {
      return mat_readiness::probe_failed;
    }
    if (*observation.format_support_hresult != s_ok) {
      return mat_readiness::format_unsupported;
    }
    if (!observation.initialize_hresult || *observation.initialize_hresult != s_ok) {
      return mat_readiness::initialize_failed;
    }
    return mat_readiness::ready;
  }

  void append_mat_failure(
    std::vector<atmos_probe::diagnostic> &diagnostics,
    const atmos_probe::mat_profile profile,
    const mat_readiness readiness) {
    using atmos_probe::diagnostic;
    using atmos_probe::mat_profile;

    switch (profile) {
      case mat_profile::mat10:
        switch (readiness) {
          case mat_readiness::ready:
            return;
          case mat_readiness::probe_failed:
            diagnostics.push_back(diagnostic::mat10_exclusive_probe_failed);
            return;
          case mat_readiness::format_unsupported:
            diagnostics.push_back(diagnostic::mat10_exclusive_format_unsupported);
            return;
          case mat_readiness::initialize_failed:
            diagnostics.push_back(diagnostic::mat10_exclusive_initialize_failed);
            return;
        }
        return;
      case mat_profile::mat20:
        switch (readiness) {
          case mat_readiness::ready:
            return;
          case mat_readiness::probe_failed:
            diagnostics.push_back(diagnostic::mat20_exclusive_probe_failed);
            return;
          case mat_readiness::format_unsupported:
            diagnostics.push_back(diagnostic::mat20_exclusive_format_unsupported);
            return;
          case mat_readiness::initialize_failed:
            diagnostics.push_back(diagnostic::mat20_exclusive_initialize_failed);
            return;
        }
        return;
      case mat_profile::mat21:
        switch (readiness) {
          case mat_readiness::ready:
            return;
          case mat_readiness::probe_failed:
            diagnostics.push_back(diagnostic::mat21_exclusive_probe_failed);
            return;
          case mat_readiness::format_unsupported:
            diagnostics.push_back(diagnostic::mat21_exclusive_format_unsupported);
            return;
          case mat_readiness::initialize_failed:
            diagnostics.push_back(diagnostic::mat21_exclusive_initialize_failed);
            return;
        }
        return;
    }
  }
}  // namespace

namespace atmos_probe {
  bool is_active(const endpoint_observation &endpoint) {
    return endpoint.state == device_state_active;
  }

  bool is_display_audio(const endpoint_observation &endpoint) {
    return std::holds_alternative<display_audio_form_factor>(endpoint.form_factor);
  }

  bool is_hdmi(const endpoint_observation &endpoint) {
    return std::holds_alternative<hdmi_connector>(endpoint.jack_subtype);
  }

  bool all_default_roles_match_selected_endpoint(
    const std::optional<endpoint_observation> &selected_endpoint,
    const std::array<role_endpoint_observation, 3> &default_endpoints) {
    if (!selected_endpoint || selected_endpoint->id.empty()) {
      return false;
    }

    for (const std::string_view required_role : {"console", "multimedia", "communications"}) {
      const auto role = std::find_if(
        default_endpoints.begin(),
        default_endpoints.end(),
        [required_role](const role_endpoint_observation &candidate) {
          return candidate.role == required_role;
        });
      if (role == default_endpoints.end() || !role->endpoint ||
          role->endpoint->id != selected_endpoint->id) {
        return false;
      }
    }
    return true;
  }

  bool default_route_link_is_stable(
    const std::optional<endpoint_observation> &selected_endpoint,
    const std::array<role_endpoint_observation, 3> &initial_default_endpoints,
    const std::array<role_endpoint_observation, 3> &final_default_endpoints,
    const std::string_view initial_default_render_id,
    const std::string_view initial_communications_render_id,
    const std::string_view final_default_render_id,
    const std::string_view final_communications_render_id) {
    return all_default_roles_match_selected_endpoint(
             selected_endpoint,
             initial_default_endpoints) &&
           all_default_roles_match_selected_endpoint(
             selected_endpoint,
             final_default_endpoints) &&
           !initial_default_render_id.empty() &&
           initial_default_render_id == initial_communications_render_id &&
           initial_default_render_id == final_default_render_id &&
           initial_default_render_id == final_communications_render_id;
  }

  gate_result evaluate(const probe_observation &observation) {
    gate_result result {};

    if (!observation.selected_endpoint) {
      result.diagnostics.push_back(diagnostic::selected_endpoint_not_found);
      return result;
    }

    if (!observation.probe_complete || !observation.errors.empty()) {
      result.diagnostics.push_back(diagnostic::probe_runtime_error);
    }

    const auto &endpoint = *observation.selected_endpoint;
    if (!is_active(endpoint)) {
      result.diagnostics.push_back(diagnostic::selected_endpoint_not_active);
    }
    if (!is_display_audio(endpoint)) {
      result.diagnostics.push_back(diagnostic::selected_endpoint_not_display_audio);
    }
    if (!is_hdmi(endpoint)) {
      result.diagnostics.push_back(diagnostic::selected_endpoint_not_hdmi);
    }

    const auto spatial_endpoint_linked =
      all_default_roles_match_selected_endpoint(
        observation.selected_endpoint,
        observation.default_endpoints) &&
      observation.spatial.selected_endpoint_linked &&
      observation.spatial.link_source == winrt_default_link_source &&
      !observation.spatial.input_render_device_id.empty();
    if (!spatial_endpoint_linked) {
      result.diagnostics.push_back(diagnostic::spatial_device_id_unlinked);
    } else if (!observation.spatial.configuration_available) {
      result.diagnostics.push_back(diagnostic::spatial_configuration_unavailable);
    } else if (observation.spatial.returned_render_device_id !=
               observation.spatial.input_render_device_id) {
      result.diagnostics.push_back(diagnostic::spatial_device_id_unlinked);
    } else {
      if (!observation.spatial.spatial_audio_supported) {
        result.diagnostics.push_back(diagnostic::spatial_audio_unsupported);
      }
      if (!observation.spatial.atmos_home_theater_supported) {
        result.diagnostics.push_back(diagnostic::atmos_home_theater_unsupported);
      }
      if (observation.spatial.active_format_guid != atmos_home_theater_guid) {
        result.diagnostics.push_back(diagnostic::active_spatial_format_not_atmos_home_theater);
      }
    }

    const auto mat20_readiness = evaluate_mat(observation.mat20);
    const auto mat21_readiness = evaluate_mat(observation.mat21);
    const auto mat10_readiness = evaluate_mat(observation.mat10);
    if (mat21_readiness == mat_readiness::ready) {
      result.ready_profiles.push_back(mat_profile::mat21);
    }
    if (mat20_readiness == mat_readiness::ready) {
      result.ready_profiles.push_back(mat_profile::mat20);
    }
    if (mat10_readiness == mat_readiness::ready) {
      result.ready_profiles.push_back(mat_profile::mat10);
    }

    if (result.ready_profiles.empty()) {
      append_mat_failure(result.diagnostics, mat_profile::mat10, mat10_readiness);
      append_mat_failure(result.diagnostics, mat_profile::mat20, mat20_readiness);
      append_mat_failure(result.diagnostics, mat_profile::mat21, mat21_readiness);
      result.diagnostics.push_back(diagnostic::no_exclusive_mat_profile_ready);
    } else {
      result.selected_profile = result.ready_profiles.front();
    }

    result.ready = result.diagnostics.empty();
    return result;
  }

  std::string_view to_string(const mat_profile profile) {
    switch (profile) {
      case mat_profile::mat10:
        return "MAT10";
      case mat_profile::mat20:
        return "MAT20";
      case mat_profile::mat21:
        return "MAT21";
    }
    return {};
  }

  std::string_view to_string(const diagnostic value) {
    switch (value) {
      case diagnostic::probe_runtime_error:
        return "PROBE_RUNTIME_ERROR";
      case diagnostic::selected_endpoint_not_found:
        return "SELECTED_ENDPOINT_NOT_FOUND";
      case diagnostic::selected_endpoint_not_active:
        return "SELECTED_ENDPOINT_NOT_ACTIVE";
      case diagnostic::selected_endpoint_not_display_audio:
        return "SELECTED_ENDPOINT_NOT_DISPLAY_AUDIO";
      case diagnostic::selected_endpoint_not_hdmi:
        return "SELECTED_ENDPOINT_NOT_HDMI";
      case diagnostic::spatial_device_id_unlinked:
        return "SPATIAL_DEVICE_ID_UNLINKED";
      case diagnostic::spatial_configuration_unavailable:
        return "SPATIAL_CONFIGURATION_UNAVAILABLE";
      case diagnostic::spatial_audio_unsupported:
        return "SPATIAL_AUDIO_UNSUPPORTED";
      case diagnostic::atmos_home_theater_unsupported:
        return "ATMOS_HOME_THEATER_UNSUPPORTED";
      case diagnostic::active_spatial_format_not_atmos_home_theater:
        return "ACTIVE_SPATIAL_FORMAT_NOT_ATMOS_HOME_THEATER";
      case diagnostic::mat10_exclusive_probe_failed:
        return "MAT10_EXCLUSIVE_PROBE_FAILED";
      case diagnostic::mat10_exclusive_format_unsupported:
        return "MAT10_EXCLUSIVE_FORMAT_UNSUPPORTED";
      case diagnostic::mat10_exclusive_initialize_failed:
        return "MAT10_EXCLUSIVE_INITIALIZE_FAILED";
      case diagnostic::mat20_exclusive_probe_failed:
        return "MAT20_EXCLUSIVE_PROBE_FAILED";
      case diagnostic::mat20_exclusive_format_unsupported:
        return "MAT20_EXCLUSIVE_FORMAT_UNSUPPORTED";
      case diagnostic::mat20_exclusive_initialize_failed:
        return "MAT20_EXCLUSIVE_INITIALIZE_FAILED";
      case diagnostic::mat21_exclusive_probe_failed:
        return "MAT21_EXCLUSIVE_PROBE_FAILED";
      case diagnostic::mat21_exclusive_format_unsupported:
        return "MAT21_EXCLUSIVE_FORMAT_UNSUPPORTED";
      case diagnostic::mat21_exclusive_initialize_failed:
        return "MAT21_EXCLUSIVE_INITIALIZE_FAILED";
      case diagnostic::no_exclusive_mat_profile_ready:
        return "NO_EXCLUSIVE_MAT_PROFILE_READY";
    }
    return {};
  }
}  // namespace atmos_probe

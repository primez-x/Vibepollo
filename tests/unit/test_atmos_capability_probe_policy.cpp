#include "../tests_common.h"
#include "tools/atmos_capability_probe_policy.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using atmos_probe::api_error;
  using atmos_probe::diagnostic;
  using atmos_probe::display_audio_form_factor;
  using atmos_probe::displayport_connector;
  using atmos_probe::endpoint_observation;
  using atmos_probe::form_factor_observation;
  using atmos_probe::gate_result;
  using atmos_probe::hdmi_connector;
  using atmos_probe::hresult_code;
  using atmos_probe::jack_subtype_observation;
  using atmos_probe::malformed_connector_guid;
  using atmos_probe::mat_observation;
  using atmos_probe::mat_profile;
  using atmos_probe::other_connector_guid;
  using atmos_probe::other_form_factor;
  using atmos_probe::probe_observation;
  using atmos_probe::property_missing;
  using atmos_probe::property_wrong_type;

  constexpr std::string_view atmos_home_theater_guid =
    "{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
  constexpr hresult_code device_in_use_hresult = -2004287478;

  probe_observation ready_observation() {
    probe_observation observation {};
    const endpoint_observation endpoint {
      .id = "endpoint-id",
      .friendly_name = "HDMI display audio",
      .state = 1,
      .form_factor = display_audio_form_factor {},
      .jack_subtype = hdmi_connector {},
    };
    observation.selected_endpoint = endpoint;
    observation.default_endpoints = {
      atmos_probe::role_endpoint_observation {.role = "console", .endpoint = endpoint},
      atmos_probe::role_endpoint_observation {.role = "multimedia", .endpoint = endpoint},
      atmos_probe::role_endpoint_observation {.role = "communications", .endpoint = endpoint},
    };
    observation.spatial = {
      .selected_endpoint_linked = true,
      .link_source = "winrt_default_and_communications",
      .input_render_device_id = "opaque-winrt-render-id",
      .returned_render_device_id = "opaque-winrt-render-id",
      .configuration_available = true,
      .spatial_audio_supported = true,
      .atmos_home_theater_supported = true,
      .active_format_raw = "Dolby Atmos for Home Theater",
      .active_format_guid = std::string {atmos_home_theater_guid},
      .default_format_raw = "Dolby Atmos for Home Theater",
      .default_format_guid = std::string {atmos_home_theater_guid},
    };
    observation.mat21 = {
      .format_support_hresult = 0,
      .initialize_hresult = 0,
    };
    observation.mat20 = {
      .format_support_hresult = 0,
      .initialize_hresult = 0,
    };
    return observation;
  }

  void expect_gate_result(
    const gate_result &result,
    bool ready,
    const std::optional<mat_profile> &selected_profile,
    const std::vector<mat_profile> &ready_profiles,
    const std::vector<diagnostic> &diagnostics) {
    EXPECT_EQ(result.ready, ready);
    EXPECT_EQ(result.selected_profile, selected_profile);
    EXPECT_EQ(result.ready_profiles, ready_profiles);
    EXPECT_EQ(result.diagnostics, diagnostics);
  }
}  // namespace

// Catches profile inference that lets the MAT20 probe authorize MAT21, or vice versa.
TEST(AtmosCapabilityProbePolicy, SelectsOnlyEachIndependentlyObservedReadyProfile) {
  struct readiness_case {
    std::string_view name;
    probe_observation observation;
    std::optional<mat_profile> selected_profile;
    std::vector<mat_profile> ready_profiles;
  };

  auto mat21_only = ready_observation();
  mat21_only.mat20 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };

  auto mat20_only = ready_observation();
  mat20_only.mat21 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };

  auto mat10_only = ready_observation();
  mat10_only.mat21 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };
  mat10_only.mat20 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };
  mat10_only.mat10 = {
    .format_support_hresult = 0,
    .initialize_hresult = 0,
  };

  const std::array<readiness_case, 3> cases {
    readiness_case {
      .name = "MAT21 only",
      .observation = std::move(mat21_only),
      .selected_profile = mat_profile::mat21,
      .ready_profiles = {mat_profile::mat21},
    },
    readiness_case {
      .name = "MAT20 only",
      .observation = std::move(mat20_only),
      .selected_profile = mat_profile::mat20,
      .ready_profiles = {mat_profile::mat20},
    },
    readiness_case {
      .name = "MAT10 only",
      .observation = std::move(mat10_only),
      .selected_profile = mat_profile::mat10,
      .ready_profiles = {mat_profile::mat10},
    },
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.name});
    expect_gate_result(
      atmos_probe::evaluate(test_case.observation),
      true,
      test_case.selected_profile,
      test_case.ready_profiles,
      std::vector<diagnostic> {}
    );
  }
}

// Catches preference drift that promotes an older profile ahead of an independently ready newer
// profile, or omits MAT10 from the canonical ready list.
TEST(AtmosCapabilityProbePolicy, PrefersMat21ThenMat20ThenMat10) {
  auto all_ready = ready_observation();
  all_ready.mat10 = {
    .format_support_hresult = 0,
    .initialize_hresult = 0,
  };

  auto mat21_and_mat10 = all_ready;
  mat21_and_mat10.mat20 = {};

  auto mat20_and_mat10 = all_ready;
  mat20_and_mat10.mat21 = {};

  struct preference_case {
    std::string_view name;
    probe_observation observation;
    mat_profile selected_profile;
    std::vector<mat_profile> ready_profiles;
  };
  const std::array<preference_case, 3> cases {
    preference_case {
      .name = "all ready",
      .observation = std::move(all_ready),
      .selected_profile = mat_profile::mat21,
      .ready_profiles = {mat_profile::mat21, mat_profile::mat20, mat_profile::mat10},
    },
    preference_case {
      .name = "MAT21 and MAT10",
      .observation = std::move(mat21_and_mat10),
      .selected_profile = mat_profile::mat21,
      .ready_profiles = {mat_profile::mat21, mat_profile::mat10},
    },
    preference_case {
      .name = "MAT20 and MAT10",
      .observation = std::move(mat20_and_mat10),
      .selected_profile = mat_profile::mat20,
      .ready_profiles = {mat_profile::mat20, mat_profile::mat10},
    },
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.name});
    expect_gate_result(
      atmos_probe::evaluate(test_case.observation),
      true,
      test_case.selected_profile,
      test_case.ready_profiles,
      std::vector<diagnostic> {}
    );
  }
}

// Catches default-role inference from a subset, a missing role, or endpoint-name similarity.
TEST(AtmosCapabilityProbePolicy, AuthorizesDefaultRoleLinkOnlyForThreeExactEndpointIds) {
  auto observation = ready_observation();
  EXPECT_TRUE(atmos_probe::all_default_roles_match_selected_endpoint(
    observation.selected_endpoint,
    observation.default_endpoints));

  observation.default_endpoints[1].endpoint.reset();
  EXPECT_FALSE(atmos_probe::all_default_roles_match_selected_endpoint(
    observation.selected_endpoint,
    observation.default_endpoints));

  observation = ready_observation();
  observation.default_endpoints[2].endpoint->id = "different-id";
  observation.default_endpoints[2].endpoint->friendly_name =
    observation.selected_endpoint->friendly_name;
  EXPECT_FALSE(atmos_probe::all_default_roles_match_selected_endpoint(
    observation.selected_endpoint,
    observation.default_endpoints));

  observation = ready_observation();
  observation.selected_endpoint.reset();
  EXPECT_FALSE(atmos_probe::all_default_roles_match_selected_endpoint(
    observation.selected_endpoint,
    observation.default_endpoints));
}

// Catches a cross-endpoint false green when the Windows default route changes between the initial
// Core Audio/WinRT samples and the final bookend samples.
TEST(AtmosCapabilityProbePolicy, RequiresStableBookendedDefaultRouteLink) {
  const auto stable = ready_observation();
  const auto initial_defaults = stable.default_endpoints;
  const std::string_view render_id_a = "opaque-winrt-render-a";

  EXPECT_TRUE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    initial_defaults,
    render_id_a,
    render_id_a,
    render_id_a,
    render_id_a));

  const auto selected_endpoint_id = stable.selected_endpoint->id;
  EXPECT_FALSE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    initial_defaults,
    selected_endpoint_id,
    selected_endpoint_id,
    selected_endpoint_id,
    selected_endpoint_id));

  auto switched_core_defaults = initial_defaults;
  switched_core_defaults[2].endpoint->id = "endpoint-b";
  EXPECT_FALSE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    switched_core_defaults,
    render_id_a,
    render_id_a,
    render_id_a,
    render_id_a));

  EXPECT_FALSE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    initial_defaults,
    render_id_a,
    render_id_a,
    "opaque-winrt-render-b",
    "opaque-winrt-render-b"));

  EXPECT_FALSE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    initial_defaults,
    render_id_a,
    "opaque-winrt-render-b",
    render_id_a,
    render_id_a));

  EXPECT_FALSE(atmos_probe::default_route_link_is_stable(
    stable.selected_endpoint,
    initial_defaults,
    initial_defaults,
    "",
    "",
    "",
    ""));
}

// Catches a false-green spatial lookup whose returned DeviceId does not exactly match its opaque
// input, or whose selected endpoint was never authorized through all default roles.
TEST(AtmosCapabilityProbePolicy, BlocksUnlinkedOrMismatchedSpatialDeviceIds) {
  auto unlinked = ready_observation();
  unlinked.spatial.selected_endpoint_linked = false;
  unlinked.spatial.link_source.clear();
  unlinked.spatial.input_render_device_id.clear();
  unlinked.spatial.returned_render_device_id.clear();
  expect_gate_result(
    atmos_probe::evaluate(unlinked),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_device_id_unlinked}
  );

  auto mismatched = ready_observation();
  mismatched.spatial.returned_render_device_id = "different-winrt-id";
  expect_gate_result(
    atmos_probe::evaluate(mismatched),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_device_id_unlinked}
  );

  auto collapsed_namespace = ready_observation();
  collapsed_namespace.spatial.input_render_device_id =
    collapsed_namespace.selected_endpoint->id;
  collapsed_namespace.spatial.returned_render_device_id =
    collapsed_namespace.selected_endpoint->id;
  expect_gate_result(
    atmos_probe::evaluate(collapsed_namespace),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_device_id_unlinked}
  );

  auto input_namespace_collision = ready_observation();
  input_namespace_collision.spatial.configuration_available = false;
  input_namespace_collision.spatial.input_render_device_id =
    input_namespace_collision.selected_endpoint->id;
  input_namespace_collision.spatial.returned_render_device_id.clear();
  expect_gate_result(
    atmos_probe::evaluate(input_namespace_collision),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_device_id_unlinked}
  );

  auto returned_namespace_collision = ready_observation();
  returned_namespace_collision.spatial.configuration_available = false;
  returned_namespace_collision.spatial.returned_render_device_id =
    returned_namespace_collision.selected_endpoint->id;
  expect_gate_result(
    atmos_probe::evaluate(returned_namespace_collision),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_device_id_unlinked}
  );
}

// Catches a preference regression that selects MAT20 before the independently ready MAT21.
TEST(AtmosCapabilityProbePolicy, PrefersMat21WhenBothProfilesAreReady) {
  expect_gate_result(
    atmos_probe::evaluate(ready_observation()),
    true,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {}
  );
}

// Catches a missing-endpoint branch that leaks unrelated diagnostics or a profile result.
TEST(AtmosCapabilityProbePolicy, ReportsOnlyNotFoundForMissingSelectedEndpoint) {
  auto observation = ready_observation();
  observation.selected_endpoint.reset();
  observation.probe_complete = false;
  observation.errors = {api_error {.operation = "Activate", .hresult = -1}};

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {diagnostic::selected_endpoint_not_found}
  );
}

// Catches a bitwise DEVICE_STATE_ACTIVE check that treats any nonzero raw state as active.
TEST(AtmosCapabilityProbePolicy, RequiresRawStateToBeExactlyActive) {
  const std::array<std::uint32_t, 2> inactive_states {0U, 3U};

  for (const auto state : inactive_states) {
    auto observation = ready_observation();
    observation.selected_endpoint->state = state;

    expect_gate_result(
      atmos_probe::evaluate(observation),
      false,
      mat_profile::mat21,
      std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
      std::vector<diagnostic> {diagnostic::selected_endpoint_not_active}
    );
  }
}

// Catches a form-factor branch that accepts missing, wrong-type, or non-display observations.
TEST(AtmosCapabilityProbePolicy, RequiresDisplayAudioFormFactorAlternative) {
  const std::array<form_factor_observation, 3> non_display_observations {
    property_missing {},
    property_wrong_type {.variant_type = 31},
    other_form_factor {.value = 0},
  };

  for (const auto &form_factor : non_display_observations) {
    auto observation = ready_observation();
    observation.selected_endpoint->form_factor = form_factor;

    expect_gate_result(
      atmos_probe::evaluate(observation),
      false,
      mat_profile::mat21,
      std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
      std::vector<diagnostic> {diagnostic::selected_endpoint_not_display_audio}
    );
  }
}

// Catches a connector branch that accepts a malformed, DisplayPort, missing, wrong-type, or other GUID.
TEST(AtmosCapabilityProbePolicy, RequiresHdmiConnectorAlternative) {
  const std::array<jack_subtype_observation, 5> non_hdmi_observations {
    displayport_connector {},
    other_connector_guid {.canonical_guid = "{00000000-0000-0000-0000-000000000001}"},
    malformed_connector_guid {.raw = "not-a-guid"},
    property_missing {},
    property_wrong_type {.variant_type = 8},
  };

  for (const auto &jack_subtype : non_hdmi_observations) {
    auto observation = ready_observation();
    observation.selected_endpoint->jack_subtype = jack_subtype;

    expect_gate_result(
      atmos_probe::evaluate(observation),
      false,
      mat_profile::mat21,
      std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
      std::vector<diagnostic> {diagnostic::selected_endpoint_not_hdmi}
    );
  }
}

// Catches use of unavailable subordinate spatial values after configuration lookup failed.
TEST(AtmosCapabilityProbePolicy, SuppressesSubordinateSpatialDiagnosticsWhenConfigurationIsUnavailable) {
  auto observation = ready_observation();
  observation.spatial = {
    .selected_endpoint_linked = true,
    .link_source = "winrt_default_and_communications",
    .input_render_device_id = "opaque-winrt-render-id",
    .returned_render_device_id = "",
    .configuration_available = false,
    .spatial_audio_supported = false,
    .atmos_home_theater_supported = false,
    .active_format_raw = "other",
    .active_format_guid = "{00000000-0000-0000-0000-000000000001}",
    .default_format_raw = "other",
    .default_format_guid = "{00000000-0000-0000-0000-000000000001}",
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::spatial_configuration_unavailable}
  );
}

// Catches spatial prerequisite branches that incorrectly ignore an available capability failure.
TEST(AtmosCapabilityProbePolicy, RequiresEachAvailableSpatialCapability) {
  struct spatial_case {
    bool spatial_audio_supported;
    bool atmos_home_theater_supported;
    diagnostic expected_diagnostic;
  };

  const std::array<spatial_case, 2> cases {
    spatial_case {
      .spatial_audio_supported = false,
      .atmos_home_theater_supported = true,
      .expected_diagnostic = diagnostic::spatial_audio_unsupported,
    },
    spatial_case {
      .spatial_audio_supported = true,
      .atmos_home_theater_supported = false,
      .expected_diagnostic = diagnostic::atmos_home_theater_unsupported,
    },
  };

  for (const auto &test_case : cases) {
    auto observation = ready_observation();
    observation.spatial.spatial_audio_supported = test_case.spatial_audio_supported;
    observation.spatial.atmos_home_theater_supported = test_case.atmos_home_theater_supported;

    expect_gate_result(
      atmos_probe::evaluate(observation),
      false,
      mat_profile::mat21,
      std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
      std::vector<diagnostic> {test_case.expected_diagnostic}
    );
  }
}

// Catches validation of DefaultSpatialAudioFormat or the wrong active spatial GUID.
TEST(AtmosCapabilityProbePolicy, RequiresExactActiveAtmosHomeTheaterGuid) {
  auto observation = ready_observation();
  observation.spatial.active_format_raw = "Windows Sonic for Headphones";
  observation.spatial.active_format_guid = "{00000000-0000-0000-0000-000000000001}";
  observation.spatial.default_format_raw = "Dolby Atmos for Home Theater";
  observation.spatial.default_format_guid = std::string {atmos_home_theater_guid};

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::active_spatial_format_not_atmos_home_theater}
  );
}

// Catches a branch that lets DefaultSpatialAudioFormat affect an otherwise valid active format.
TEST(AtmosCapabilityProbePolicy, TreatsDefaultSpatialFormatAsDiagnosticOnly) {
  auto observation = ready_observation();
  observation.spatial.default_format_raw = "Windows Sonic for Headphones";
  observation.spatial.default_format_guid = "{00000000-0000-0000-0000-000000000001}";

  expect_gate_result(
    atmos_probe::evaluate(observation),
    true,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {}
  );
}

// Catches a SUCCEEDED check that accepts a nonzero HRESULT as an exclusive-format success.
TEST(AtmosCapabilityProbePolicy, RequiresExactSOkForMatFormatSupport) {
  auto observation = ready_observation();
  observation.mat20 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };
  observation.mat21 = {
    .format_support_hresult = 1,
    .initialize_hresult = 0,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_probe_failed,
      diagnostic::mat21_exclusive_format_unsupported,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches a SUCCEEDED check that accepts S_FALSE as an exclusive-initialize success.
TEST(AtmosCapabilityProbePolicy, RequiresExactSOkForMatInitialize) {
  auto observation = ready_observation();
  observation.mat20 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };
  observation.mat21 = {
    .format_support_hresult = 0,
    .initialize_hresult = 1,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_probe_failed,
      diagnostic::mat21_exclusive_initialize_failed,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches an initialize-result branch that ignores AUDCLNT_E_DEVICE_IN_USE after S_OK support.
TEST(AtmosCapabilityProbePolicy, ReportsMat21ExclusiveInitializeFailure) {
  auto observation = ready_observation();
  observation.mat20 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };
  observation.mat21 = {
    .format_support_hresult = 0,
    .initialize_hresult = device_in_use_hresult,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_probe_failed,
      diagnostic::mat21_exclusive_initialize_failed,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches a MAT20 diagnostic mapping or ordering regression after an S_OK support probe.
TEST(AtmosCapabilityProbePolicy, ReportsMat20ExclusiveInitializeFailure) {
  auto observation = ready_observation();
  observation.mat20 = {
    .format_support_hresult = 0,
    .initialize_hresult = device_in_use_hresult,
  };
  observation.mat21 = {
    .format_support_hresult = std::nullopt,
    .initialize_hresult = std::nullopt,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_initialize_failed,
      diagnostic::mat21_exclusive_probe_failed,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches each MAT10 failure state being inferred from another profile or mapped to a MAT20/21
// diagnostic.
TEST(AtmosCapabilityProbePolicy, ReportsEachMat10FailureExactly) {
  struct failure_case {
    std::string_view name;
    mat_observation mat10;
    diagnostic expected;
  };
  const std::array<failure_case, 3> cases {
    failure_case {
      .name = "probe missing",
      .mat10 = {.format_support_hresult = std::nullopt, .initialize_hresult = std::nullopt},
      .expected = diagnostic::mat10_exclusive_probe_failed,
    },
    failure_case {
      .name = "format unsupported",
      .mat10 = {.format_support_hresult = -1, .initialize_hresult = std::nullopt},
      .expected = diagnostic::mat10_exclusive_format_unsupported,
    },
    failure_case {
      .name = "initialize failed",
      .mat10 = {.format_support_hresult = 0, .initialize_hresult = device_in_use_hresult},
      .expected = diagnostic::mat10_exclusive_initialize_failed,
    },
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.name});
    auto observation = ready_observation();
    observation.mat21 = {};
    observation.mat20 = {};
    observation.mat10 = test_case.mat10;
    expect_gate_result(
      atmos_probe::evaluate(observation),
      false,
      std::nullopt,
      std::vector<mat_profile> {},
      std::vector<diagnostic> {
        test_case.expected,
        diagnostic::mat20_exclusive_probe_failed,
        diagnostic::mat21_exclusive_probe_failed,
        diagnostic::no_exclusive_mat_profile_ready,
      }
    );
  }
}

// Catches profile diagnostic reordering or a branch that reports support failures as probe failures.
TEST(AtmosCapabilityProbePolicy, ReportsUnsupportedFormatsAfterMat10InCanonicalOrder) {
  auto observation = ready_observation();
  observation.mat20 = {
    .format_support_hresult = -1,
    .initialize_hresult = 0,
  };
  observation.mat21 = {
    .format_support_hresult = -1,
    .initialize_hresult = 0,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_format_unsupported,
      diagnostic::mat21_exclusive_format_unsupported,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches diagnostic reordering across endpoint, spatial, MAT20, MAT21, and summary groups.
TEST(AtmosCapabilityProbePolicy, ProducesDiagnosticsInStableGroupOrder) {
  auto observation = ready_observation();
  observation.selected_endpoint->state = 0;
  observation.selected_endpoint->form_factor = property_missing {};
  observation.selected_endpoint->jack_subtype = displayport_connector {};
  observation.spatial.configuration_available = false;
  observation.mat20 = {
    .format_support_hresult = -1,
    .initialize_hresult = 0,
  };
  observation.mat21 = {
    .format_support_hresult = -1,
    .initialize_hresult = 0,
  };

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    std::nullopt,
    std::vector<mat_profile> {},
    std::vector<diagnostic> {
      diagnostic::selected_endpoint_not_active,
      diagnostic::selected_endpoint_not_display_audio,
      diagnostic::selected_endpoint_not_hdmi,
      diagnostic::spatial_configuration_unavailable,
      diagnostic::mat10_exclusive_probe_failed,
      diagnostic::mat20_exclusive_format_unsupported,
      diagnostic::mat21_exclusive_format_unsupported,
      diagnostic::no_exclusive_mat_profile_ready,
    }
  );
}

// Catches a probe-completeness branch that reports green from otherwise valid capability fields.
TEST(AtmosCapabilityProbePolicy, ProbeRuntimeErrorPreventsReadiness) {
  auto observation = ready_observation();
  observation.probe_complete = false;

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::probe_runtime_error}
  );
}

// Catches ignored collected API failures that would allow a partially failed probe to report green.
TEST(AtmosCapabilityProbePolicy, CollectedApiErrorPreventsReadiness) {
  auto observation = ready_observation();
  observation.errors = {api_error {.operation = "GetProperty", .hresult = -1}};

  expect_gate_result(
    atmos_probe::evaluate(observation),
    false,
    mat_profile::mat21,
    std::vector<mat_profile> {mat_profile::mat21, mat_profile::mat20},
    std::vector<diagnostic> {diagnostic::probe_runtime_error}
  );
}

// Catches a helper that derives endpoint truth from anything other than canonical alternatives.
TEST(AtmosCapabilityProbePolicy, DerivesEndpointFactsOnlyFromCanonicalObservations) {
  auto observation = ready_observation();
  auto endpoint = *observation.selected_endpoint;

  EXPECT_TRUE(atmos_probe::is_active(endpoint));
  EXPECT_TRUE(atmos_probe::is_display_audio(endpoint));
  EXPECT_TRUE(atmos_probe::is_hdmi(endpoint));

  endpoint.state = 3;
  endpoint.form_factor = property_wrong_type {.variant_type = 31};
  endpoint.jack_subtype = malformed_connector_guid {.raw = "not-a-guid"};

  EXPECT_FALSE(atmos_probe::is_active(endpoint));
  EXPECT_FALSE(atmos_probe::is_display_audio(endpoint));
  EXPECT_FALSE(atmos_probe::is_hdmi(endpoint));
}

// Catches an incomplete or lower-case enum-to-string mapping in report serialization.
TEST(AtmosCapabilityProbePolicy, UsesStableUppercaseEnumNames) {
  EXPECT_EQ(atmos_probe::to_string(mat_profile::mat10), "MAT10");
  EXPECT_EQ(atmos_probe::to_string(mat_profile::mat20), "MAT20");
  EXPECT_EQ(atmos_probe::to_string(mat_profile::mat21), "MAT21");

  const std::array<std::pair<diagnostic, std::string_view>, 20> names {
    std::pair {diagnostic::probe_runtime_error, "PROBE_RUNTIME_ERROR"},
    std::pair {diagnostic::selected_endpoint_not_found, "SELECTED_ENDPOINT_NOT_FOUND"},
    std::pair {diagnostic::selected_endpoint_not_active, "SELECTED_ENDPOINT_NOT_ACTIVE"},
    std::pair {diagnostic::selected_endpoint_not_display_audio, "SELECTED_ENDPOINT_NOT_DISPLAY_AUDIO"},
    std::pair {diagnostic::selected_endpoint_not_hdmi, "SELECTED_ENDPOINT_NOT_HDMI"},
    std::pair {diagnostic::spatial_device_id_unlinked, "SPATIAL_DEVICE_ID_UNLINKED"},
    std::pair {diagnostic::spatial_configuration_unavailable, "SPATIAL_CONFIGURATION_UNAVAILABLE"},
    std::pair {diagnostic::spatial_audio_unsupported, "SPATIAL_AUDIO_UNSUPPORTED"},
    std::pair {diagnostic::atmos_home_theater_unsupported, "ATMOS_HOME_THEATER_UNSUPPORTED"},
    std::pair {diagnostic::active_spatial_format_not_atmos_home_theater, "ACTIVE_SPATIAL_FORMAT_NOT_ATMOS_HOME_THEATER"},
    std::pair {diagnostic::mat10_exclusive_probe_failed, "MAT10_EXCLUSIVE_PROBE_FAILED"},
    std::pair {diagnostic::mat10_exclusive_format_unsupported, "MAT10_EXCLUSIVE_FORMAT_UNSUPPORTED"},
    std::pair {diagnostic::mat10_exclusive_initialize_failed, "MAT10_EXCLUSIVE_INITIALIZE_FAILED"},
    std::pair {diagnostic::mat20_exclusive_probe_failed, "MAT20_EXCLUSIVE_PROBE_FAILED"},
    std::pair {diagnostic::mat20_exclusive_format_unsupported, "MAT20_EXCLUSIVE_FORMAT_UNSUPPORTED"},
    std::pair {diagnostic::mat20_exclusive_initialize_failed, "MAT20_EXCLUSIVE_INITIALIZE_FAILED"},
    std::pair {diagnostic::mat21_exclusive_probe_failed, "MAT21_EXCLUSIVE_PROBE_FAILED"},
    std::pair {diagnostic::mat21_exclusive_format_unsupported, "MAT21_EXCLUSIVE_FORMAT_UNSUPPORTED"},
    std::pair {diagnostic::mat21_exclusive_initialize_failed, "MAT21_EXCLUSIVE_INITIALIZE_FAILED"},
    std::pair {diagnostic::no_exclusive_mat_profile_ready, "NO_EXCLUSIVE_MAT_PROFILE_READY"},
  };

  for (const auto &[value, expected_name] : names) {
    EXPECT_EQ(atmos_probe::to_string(value), expected_name);
  }
}

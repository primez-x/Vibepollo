#include "../tests_common.h"
#include "tools/atmos_capability_probe_app.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {
  using atmos_probe::api_error;
  using atmos_probe::display_audio_form_factor;
  using atmos_probe::endpoint_observation;
  using atmos_probe::hdmi_connector;
  using atmos_probe::hresult_code;
  using atmos_probe::mat_profile;
  using atmos_probe::probe_observation;
  using atmos_probe::probe_options;
  using atmos_probe::role_endpoint_observation;
  using ordered_json = nlohmann::ordered_json;

  constexpr std::string_view k_atmos_home_theater_guid =
    "{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
  constexpr std::string_view k_exact_endpoint_id = "exact-id";
  constexpr std::string_view k_preflight_caveat =
    "Endpoint preflight does not prove source-profile emission or downstream receiver Atmos lock.";
  constexpr std::string_view k_ready_token = "ENDPOINT_PREFLIGHT_READY";

  probe_observation canonical_ready_observation() {
    probe_observation observation {};
    const endpoint_observation endpoint {
      .id = std::string {k_exact_endpoint_id},
      .friendly_name = "Exact HDMI display audio",
      .state = 1,
      .form_factor = display_audio_form_factor {},
      .jack_subtype = hdmi_connector {},
    };
    observation.selected_endpoint = endpoint;
    observation.default_endpoints = {
      role_endpoint_observation {.role = "console", .endpoint = endpoint},
      role_endpoint_observation {.role = "multimedia", .endpoint = endpoint},
      role_endpoint_observation {.role = "communications", .endpoint = endpoint},
    };
    observation.active_endpoints = {endpoint};
    observation.spatial = {
      .selected_endpoint_linked = true,
      .link_source = "winrt_default_and_communications",
      .input_render_device_id = "opaque-winrt-render-id",
      .returned_render_device_id = "opaque-winrt-render-id",
      .configuration_available = true,
      .spatial_audio_supported = true,
      .atmos_home_theater_supported = true,
      .active_format_raw = "Dolby Atmos for Home Theater",
      .active_format_guid = std::string {k_atmos_home_theater_guid},
      .default_format_raw = "Dolby Atmos for Home Theater",
      .default_format_guid = std::string {k_atmos_home_theater_guid},
    };
    observation.mat21 = {
      .format_support_hresult = 0,
      .initialize_hresult = 0,
    };
    observation.mat20 = {
      .format_support_hresult = 0,
      .initialize_hresult = 0,
    };
    observation.mat10 = {
      .format_support_hresult = 0,
      .initialize_hresult = 0,
    };
    return observation;
  }

  ordered_json parse_report(const std::string &serialized) {
    return ordered_json::parse(serialized);
  }

  void expect_object_keys(
    const ordered_json &object,
    const std::initializer_list<std::string_view> expected_keys) {
    ASSERT_TRUE(object.is_object());

    std::vector<std::string> actual_keys;
    actual_keys.reserve(object.size());
    for (const auto &item : object.items()) {
      actual_keys.push_back(item.key());
    }

    std::vector<std::string> expected;
    expected.reserve(expected_keys.size());
    for (const auto key : expected_keys) {
      expected.emplace_back(key);
    }
    EXPECT_EQ(actual_keys, expected);
  }

  const ordered_json *find_profile(
    const ordered_json &profiles,
    const std::string_view profile_name) {
    for (const auto &profile : profiles) {
      if (profile.is_object() && profile.contains("profile") &&
          profile.at("profile").get<std::string>() == profile_name) {
        return &profile;
      }
    }
    return nullptr;
  }
}  // namespace

// Catches an app coordinator that emits a non-canonical default green report or reorders the public
// JSON contract.
TEST(AtmosCapabilityProbeApp, SerializesCanonicalDefaultMat21ReadyReport) {
  std::optional<probe_options> captured_options;
  int provider_calls {};
  const std::array arguments {
    std::string_view {"--json"},
  };

  const auto result = atmos_probe::run_probe(
    arguments,
    [&captured_options, &provider_calls](const probe_options &options) {
      ++provider_calls;
      captured_options = options;
      return canonical_ready_observation();
    });

  ASSERT_EQ(provider_calls, 1);
  ASSERT_TRUE(captured_options.has_value());
  EXPECT_FALSE(captured_options->endpoint_id.has_value());
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  EXPECT_TRUE(result.standard_error.contains(k_preflight_caveat));

  const auto report = parse_report(result.standard_output);
  expect_object_keys(
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
    });
  expect_object_keys(report.at("selection"), {"kind", "requested_endpoint_id"});
  expect_object_keys(
    report.at("default_render_endpoints"),
    {"console", "multimedia", "communications"});
  expect_object_keys(
    report.at("spatial_audio"),
    {
      "configuration_available",
      "selected_endpoint_linked",
      "link_source",
      "input_render_device_id",
      "returned_render_device_id",
      "is_spatial_audio_supported",
      "atmos_home_theater_supported",
      "active_format_raw",
      "active_format_guid",
      "default_format_raw",
      "default_format_guid",
    });
  expect_object_keys(
    report.at("gate"),
    {"verdict", "selected_profile", "ready_profiles", "diagnostics"});

  EXPECT_EQ(report.at("schema_version").get<int>(), 2);
  EXPECT_EQ(report.at("selection").at("kind").get<std::string>(), "default:eConsole");
  EXPECT_TRUE(report.at("selection").at("requested_endpoint_id").is_null());
  ASSERT_TRUE(report.at("selected_endpoint").is_object());
  expect_object_keys(
    report.at("selected_endpoint"),
    {
      "id",
      "friendly_name",
      "state",
      "state_name",
      "form_factor",
      "jack_subtype",
      "is_display_audio",
      "is_hdmi",
    });
  EXPECT_EQ(
    report.at("selected_endpoint").at("id").get<std::string>(),
    k_exact_endpoint_id);
  EXPECT_EQ(report.at("selected_endpoint").at("state").get<std::uint32_t>(), 1U);
  EXPECT_EQ(report.at("selected_endpoint").at("state_name").get<std::string>(), "ACTIVE");
  EXPECT_EQ(
    report.at("selected_endpoint").at("form_factor").at("status").get<std::string>(),
    "DISPLAY_AUDIO");
  EXPECT_EQ(
    report.at("selected_endpoint").at("jack_subtype").at("status").get<std::string>(),
    "HDMI");
  EXPECT_TRUE(report.at("selected_endpoint").at("is_display_audio").get<bool>());
  EXPECT_TRUE(report.at("selected_endpoint").at("is_hdmi").get<bool>());
  EXPECT_EQ(report.at("gate").at("verdict").get<std::string>(), "ENDPOINT_PREFLIGHT_READY");
  EXPECT_EQ(report.at("gate").at("selected_profile").get<std::string>(), "MAT21");
  EXPECT_EQ(
    report.at("gate").at("ready_profiles").at(0).get<std::string>(),
    "MAT21");
  ASSERT_EQ(report.at("gate").at("ready_profiles").size(), 3U);
  EXPECT_EQ(report.at("gate").at("ready_profiles").at(1).get<std::string>(), "MAT20");
  EXPECT_EQ(report.at("gate").at("ready_profiles").at(2).get<std::string>(), "MAT10");
  EXPECT_TRUE(report.at("spatial_audio").at("selected_endpoint_linked").get<bool>());
  EXPECT_EQ(
    report.at("spatial_audio").at("link_source").get<std::string>(),
    "winrt_default_and_communications");
  EXPECT_EQ(
    report.at("spatial_audio").at("input_render_device_id").get<std::string>(),
    "opaque-winrt-render-id");
  EXPECT_EQ(
    report.at("spatial_audio").at("returned_render_device_id").get<std::string>(),
    "opaque-winrt-render-id");
  EXPECT_EQ(
    report.at("spatial_audio").at("active_format_guid").get<std::string>(),
    k_atmos_home_theater_guid);
  EXPECT_FALSE(report.at("audio_bytes_written").get<bool>());

  const auto *mat21 = find_profile(report.at("mat_profiles"), "MAT21");
  ASSERT_NE(mat21, nullptr);
  EXPECT_EQ(
    mat21->at("format_support_hresult").get<std::string>(),
    "0x00000000");
  EXPECT_EQ(mat21->at("format_support_hresult_label").get<std::string>(), "S_OK");
  EXPECT_EQ(mat21->at("initialize_hresult").get<std::string>(), "0x00000000");
  EXPECT_EQ(mat21->at("initialize_hresult_label").get<std::string>(), "S_OK");
  EXPECT_TRUE(mat21->at("ready").get<bool>());

  ASSERT_EQ(report.at("mat_profiles").size(), 3U);
  EXPECT_EQ(report.at("mat_profiles").at(0).at("profile").get<std::string>(), "MAT21");
  EXPECT_EQ(report.at("mat_profiles").at(1).at("profile").get<std::string>(), "MAT20");
  EXPECT_EQ(report.at("mat_profiles").at(2).at("profile").get<std::string>(), "MAT10");

  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches loss or alteration of an explicit opaque Core Audio endpoint ID while preserving the
// fail-closed spatial-link boundary for diagnostic endpoint inspection.
TEST(AtmosCapabilityProbeApp, PreservesExplicitIdForBlockedCoreAudioInspection) {
  std::optional<probe_options> captured_options;
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    k_exact_endpoint_id,
    std::string_view {"--json"},
  };
  const auto result = atmos_probe::run_probe(
    arguments,
    [&captured_options](const probe_options &options) {
      captured_options = options;
      auto observation = canonical_ready_observation();
      observation.spatial = {};
      return observation;
    });

  ASSERT_TRUE(captured_options.has_value());
  ASSERT_TRUE(captured_options->endpoint_id.has_value());
  EXPECT_EQ(*captured_options->endpoint_id, k_exact_endpoint_id);
  ASSERT_EQ(result.exit_code, 1) << result.standard_error;
  const auto report = parse_report(result.standard_output);
  EXPECT_EQ(report.at("selection").at("kind").get<std::string>(), "explicit");
  EXPECT_EQ(
    report.at("selection").at("requested_endpoint_id").get<std::string>(),
    k_exact_endpoint_id);
  EXPECT_EQ(
    report.at("selected_endpoint").at("id").get<std::string>(),
    k_exact_endpoint_id);
  EXPECT_EQ(report.at("gate").at("verdict").get<std::string>(), "BLOCKED");
  EXPECT_EQ(
    report.at("gate").at("diagnostics").at(0).get<std::string>(),
    "SPATIAL_DEVICE_ID_UNLINKED");
}

// Catches a hostile provider that synthesizes linked WinRT fields to turn an explicit endpoint
// selection into READY without a documented API linkage for that exact endpoint.
TEST(AtmosCapabilityProbeApp, RejectsSyntheticExplicitLinkedReadyReport) {
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    k_exact_endpoint_id,
    std::string_view {"--json"},
  };
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return canonical_ready_observation();
    });

  EXPECT_EQ(result.exit_code, 3);
  EXPECT_TRUE(result.standard_output.empty());
  EXPECT_EQ(result.standard_error, "report validation failure\n");
}

// Catches a coordinator or validator that omits an independently ready MAT10 profile or refuses
// to select it when MAT21 and MAT20 are unavailable.
TEST(AtmosCapabilityProbeApp, SerializesMat10OnlyReadyReport) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      auto observation = canonical_ready_observation();
      observation.mat21 = {};
      observation.mat20 = {};
      return observation;
    });

  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  const auto report = parse_report(result.standard_output);
  EXPECT_EQ(report.at("gate").at("selected_profile").get<std::string>(), "MAT10");
  ASSERT_EQ(report.at("gate").at("ready_profiles").size(), 1U);
  EXPECT_EQ(report.at("gate").at("ready_profiles").at(0).get<std::string>(), "MAT10");
  const auto *mat10 = find_profile(report.at("mat_profiles"), "MAT10");
  ASSERT_NE(mat10, nullptr);
  EXPECT_TRUE(mat10->at("ready").get<bool>());
  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches a validator that accepts a self-consistent hostile green report after a real endpoint
// or MAT prerequisite is removed.
TEST(AtmosCapabilityProbeApp, RejectsEverySelfConsistentMutatedGreenPrerequisite) {
  const std::array arguments {
    std::string_view {"--json"},
  };
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return canonical_ready_observation();
    });
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  const auto canonical_report = parse_report(result.standard_output);

  struct mutation_case {
    std::string_view name;
    void (*mutate)(ordered_json &);
    std::string_view expected_error;
  };
  const std::array<mutation_case, 17> mutations {
    mutation_case {
      .name = "requested endpoint ID",
      .mutate = [](ordered_json &report) {
        report["selection"]["requested_endpoint_id"] = "wrong-requested-id";
      },
      .expected_error = "default selection must not include a requested endpoint ID",
    },
    mutation_case {
      .name = "selected endpoint ID",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["id"] = "wrong-selected-id";
      },
      .expected_error = "a green report requires all default roles to match selected_endpoint.id",
    },
    mutation_case {
      .name = "self-consistent empty endpoint IDs",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["id"] = "";
        report["default_render_endpoints"]["console"]["id"] = "";
        report["default_render_endpoints"]["multimedia"]["id"] = "";
        report["default_render_endpoints"]["communications"]["id"] = "";
      },
      .expected_error = "a green report requires a nonempty selected endpoint ID",
    },
    mutation_case {
      .name = "state name mismatch",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["state_name"] = "UNKNOWN";
      },
      .expected_error = "selected_endpoint has an inconsistent state_name",
    },
    mutation_case {
      .name = "inactive endpoint state and matching state name",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["state"] = 0;
        report["selected_endpoint"]["state_name"] = "UNKNOWN";
      },
      .expected_error = "a green report requires an active endpoint",
    },
    mutation_case {
      .name = "full other form factor and derived presentation",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["form_factor"] = {
          {"status", "OTHER"},
          {"value", 7},
        };
        report["selected_endpoint"]["is_display_audio"] = false;
      },
      .expected_error = "a green report requires DISPLAY_AUDIO",
    },
    mutation_case {
      .name = "DisplayPort connector and derived presentation",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["jack_subtype"]["status"] = "DISPLAYPORT";
        report["selected_endpoint"]["is_hdmi"] = false;
      },
      .expected_error = "a green report requires HDMI",
    },
    mutation_case {
      .name = "active Atmos GUID",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["active_format_guid"] =
          "{00000000-0000-0000-0000-000000000001}";
      },
      .expected_error = "a green report requires the exact active Atmos Home Theater state",
    },
    mutation_case {
      .name = "self-consistent unlinked spatial endpoint",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["selected_endpoint_linked"] = false;
        report["spatial_audio"]["link_source"] = "";
        report["spatial_audio"]["input_render_device_id"] = "";
        report["spatial_audio"]["returned_render_device_id"] = "";
        report["spatial_audio"]["configuration_available"] = false;
        report["gate"]["verdict"] = "ENDPOINT_PREFLIGHT_READY";
      },
      .expected_error = "a green report requires an exact linked spatial DeviceId",
    },
    mutation_case {
      .name = "wrong spatial link source",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["link_source"] = "friendly_name_inference";
      },
      .expected_error = "spatial_audio.link_source is invalid",
    },
    mutation_case {
      .name = "empty spatial input DeviceId",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["input_render_device_id"] = "";
      },
      .expected_error = "a linked spatial endpoint requires a nonempty input_render_device_id",
    },
    mutation_case {
      .name = "returned spatial DeviceId mismatch",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["returned_render_device_id"] = "different-winrt-id";
      },
      .expected_error = "a green report requires an exact linked spatial DeviceId",
    },
    mutation_case {
      .name = "self-consistent spatial DeviceId namespace collapse",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["input_render_device_id"] = k_exact_endpoint_id;
        report["spatial_audio"]["returned_render_device_id"] = k_exact_endpoint_id;
      },
      .expected_error = "a green report requires WinRT DeviceIds distinct from selected_endpoint.id",
    },
    mutation_case {
      .name = "self-consistent missing ready-profile membership",
      .mutate = [](ordered_json &report) {
        for (auto &profile : report["mat_profiles"]) {
          profile["format_support_hresult"] = nullptr;
          profile["format_support_hresult_label"] = "NOT_PROBED";
          profile["ready"] = false;
        }
        report["gate"]["ready_profiles"] = ordered_json::array();
        report["gate"]["selected_profile"] = nullptr;
      },
      .expected_error = "a green report requires selected profile ready membership",
    },
    mutation_case {
      .name = "self-consistent MAT format support HRESULT",
      .mutate = [](ordered_json &report) {
        for (auto &profile : report["mat_profiles"]) {
          profile["format_support_hresult"] = "0x8889000A";
          profile["format_support_hresult_label"] = "AUDCLNT_E_DEVICE_IN_USE";
          profile["ready"] = false;
        }
        report["gate"]["ready_profiles"] = ordered_json::array();
        report["gate"]["selected_profile"] = nullptr;
      },
      .expected_error = "a green report requires selected profile ready membership",
    },
    mutation_case {
      .name = "self-consistent MAT initialize HRESULT",
      .mutate = [](ordered_json &report) {
        for (auto &profile : report["mat_profiles"]) {
          profile["initialize_hresult"] = "0x8889000A";
          profile["initialize_hresult_label"] = "AUDCLNT_E_DEVICE_IN_USE";
          profile["ready"] = false;
        }
        report["gate"]["ready_profiles"] = ordered_json::array();
        report["gate"]["selected_profile"] = nullptr;
      },
      .expected_error = "a green report requires selected profile ready membership",
    },
    mutation_case {
      .name = "audio bytes written",
      .mutate = [](ordered_json &report) {
        report["audio_bytes_written"] = true;
      },
      .expected_error = "audio_bytes_written must be false",
    },
  };

  for (const auto &mutation : mutations) {
    SCOPED_TRACE(std::string {mutation.name});
    auto mutated = canonical_report;
    mutation.mutate(mutated);

    const auto validation = atmos_probe::validate_serialized_report(mutated.dump());
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(validation.error, mutation.expected_error);
  }
}

// Catches malformed, non-object, null, array, and otherwise well-formed but wrong-typed JSON
// reports that would bypass report validation before invariant evaluation.
TEST(AtmosCapabilityProbeApp, RejectsMalformedNonObjectNullArrayAndWrongTypeReports) {
  const std::array<std::string_view, 4> non_reports {
    "{",
    "\"not-an-object\"",
    "null",
    "[]",
  };
  for (const auto json : non_reports) {
    SCOPED_TRACE(std::string {json});
    const auto validation = atmos_probe::validate_serialized_report(json);
    EXPECT_FALSE(validation.valid);
    EXPECT_FALSE(validation.error.empty());
  }

  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return canonical_ready_observation();
    });
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  auto wrong_type = parse_report(result.standard_output);
  wrong_type["schema_version"] = "2";

  const auto validation = atmos_probe::validate_serialized_report(wrong_type.dump());
  EXPECT_FALSE(validation.valid);
  EXPECT_FALSE(validation.error.empty());

  auto old_schema = parse_report(result.standard_output);
  old_schema["schema_version"] = 1;
  const auto old_schema_validation =
    atmos_probe::validate_serialized_report(old_schema.dump());
  EXPECT_FALSE(old_schema_validation.valid);
  EXPECT_FALSE(old_schema_validation.error.empty());
}

// Catches schema acceptance of an omitted or duplicated MAT profile in an otherwise canonical
// report.
TEST(AtmosCapabilityProbeApp, RejectsMissingAndDuplicateMatProfiles) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return canonical_ready_observation();
    });
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  const auto canonical_report = parse_report(result.standard_output);

  auto missing_profile = canonical_report;
  missing_profile["mat_profiles"] = ordered_json::array({canonical_report["mat_profiles"].at(0)});
  const auto missing_validation =
    atmos_probe::validate_serialized_report(missing_profile.dump());
  EXPECT_FALSE(missing_validation.valid);
  EXPECT_FALSE(missing_validation.error.empty());

  auto duplicate_profile = canonical_report;
  duplicate_profile["mat_profiles"].at(1) = duplicate_profile["mat_profiles"].at(0);
  const auto duplicate_validation =
    atmos_probe::validate_serialized_report(duplicate_profile.dump());
  EXPECT_FALSE(duplicate_validation.valid);
  EXPECT_FALSE(duplicate_validation.error.empty());

  auto reordered_profiles = canonical_report;
  std::swap(reordered_profiles["mat_profiles"].at(1), reordered_profiles["mat_profiles"].at(2));
  const auto reordered_validation =
    atmos_probe::validate_serialized_report(reordered_profiles.dump());
  EXPECT_FALSE(reordered_validation.valid);
  EXPECT_FALSE(reordered_validation.error.empty());

  auto extra_profile = canonical_report;
  extra_profile["mat_profiles"].push_back(canonical_report["mat_profiles"].at(2));
  const auto extra_validation = atmos_probe::validate_serialized_report(extra_profile.dump());
  EXPECT_FALSE(extra_validation.valid);
  EXPECT_FALSE(extra_validation.error.empty());
}

// Catches HResult labels that contradict either a S_OK value or a not-probed value.
TEST(AtmosCapabilityProbeApp, RejectsMisleadingHresultLabels) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return canonical_ready_observation();
    });
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  const auto canonical_report = parse_report(result.standard_output);

  struct label_case {
    std::string_view name;
    void (*mutate)(ordered_json &);
  };
  const std::array<label_case, 3> cases {
    label_case {
      .name = "S_OK format support with device-in-use label",
      .mutate = [](ordered_json &report) {
        report["mat_profiles"].at(0)["format_support_hresult_label"] =
          "AUDCLNT_E_DEVICE_IN_USE";
      },
    },
    label_case {
      .name = "S_OK initialization with unknown label",
      .mutate = [](ordered_json &report) {
        report["mat_profiles"].at(0)["initialize_hresult_label"] = "UNKNOWN_HRESULT";
      },
    },
    label_case {
      .name = "not-probed support with S_OK label",
      .mutate = [](ordered_json &report) {
        report["mat_profiles"].at(0)["format_support_hresult"] = nullptr;
        report["mat_profiles"].at(0)["format_support_hresult_label"] = "S_OK";
      },
    },
  };
  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.name});
    auto mutated = canonical_report;
    test_case.mutate(mutated);

    const auto validation = atmos_probe::validate_serialized_report(mutated.dump());
    EXPECT_FALSE(validation.valid);
    EXPECT_FALSE(validation.error.empty());
  }
}

// Catches a provider/result mismatch that would otherwise print a green verdict for a route other
// than the explicit endpoint that the coordinator requested.
TEST(AtmosCapabilityProbeApp, TurnsExplicitSelectionMismatchIntoRuntimeFailureWithoutGreenOutput) {
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    k_exact_endpoint_id,
    std::string_view {"--json"},
  };
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      auto observation = canonical_ready_observation();
      observation.selected_endpoint->id = "provider-selected-a-different-id";
      return observation;
    });

  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.standard_output.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error, "report validation failure\n");
}

// Catches an exit mapper that treats a policy-blocked endpoint as a successful process result or
// fails to provide the human preflight caveat.
TEST(AtmosCapabilityProbeApp, MapsCanonicalBlockedObservationToExitOneAndHumanBlockedOutput) {
  const std::array<std::string_view, 0> arguments {};
  int provider_calls {};
  const auto result = atmos_probe::run_probe(
    arguments,
    [&provider_calls](const probe_options &) {
      ++provider_calls;
      auto observation = canonical_ready_observation();
      observation.selected_endpoint->state = 0;
      return observation;
    });

  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_TRUE(result.standard_output.starts_with("BLOCKED\n"));
  EXPECT_TRUE(result.standard_output.contains(k_preflight_caveat));
  EXPECT_TRUE(result.standard_error.empty());
}

// Catches a returned incomplete observation being treated like an ordinary capability block even
// though it carries the policy's explicit runtime diagnostic.
TEST(AtmosCapabilityProbeApp, MapsIncompleteProviderObservationToExitThreeWithDiagnosticJson) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      auto observation = canonical_ready_observation();
      observation.probe_complete = false;
      return observation;
    });

  ASSERT_EQ(result.exit_code, 3) << result.standard_error;
  const auto report = parse_report(result.standard_output);
  EXPECT_EQ(report.at("gate").at("verdict").get<std::string>(), "BLOCKED");
  ASSERT_EQ(report.at("gate").at("diagnostics").size(), 1U);
  EXPECT_EQ(report.at("gate").at("diagnostics").at(0).get<std::string>(), "PROBE_RUNTIME_ERROR");
  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches a returned API-error observation being treated like an ordinary capability block instead
// of returning the report and the dedicated runtime exit code.
TEST(AtmosCapabilityProbeApp, MapsProviderErrorObservationToExitThreeWithDiagnosticJson) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      auto observation = canonical_ready_observation();
      observation.errors.push_back(api_error {
        .operation = "collector failure",
        .hresult = static_cast<hresult_code>(0x80004005U),
      });
      return observation;
    });

  ASSERT_EQ(result.exit_code, 3) << result.standard_error;
  const auto report = parse_report(result.standard_output);
  EXPECT_EQ(report.at("gate").at("verdict").get<std::string>(), "BLOCKED");
  ASSERT_EQ(report.at("gate").at("diagnostics").size(), 1U);
  EXPECT_EQ(report.at("gate").at("diagnostics").at(0).get<std::string>(), "PROBE_RUNTIME_ERROR");
  ASSERT_EQ(report.at("probe_errors").size(), 1U);
  EXPECT_EQ(report.at("probe_errors").at(0).at("operation").get<std::string>(), "collector failure");
  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches JSON null construction that would turn a normal missing-endpoint, unprobed observation
// into a report-validation failure instead of the documented blocked result.
TEST(AtmosCapabilityProbeApp, SerializesMissingEndpointAndUnprobedMatValuesAsBlockedJsonNulls) {
  const std::array arguments {std::string_view {"--json"}};
  const auto result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      return probe_observation {};
    });

  ASSERT_EQ(result.exit_code, 1) << result.standard_error;
  const auto report = parse_report(result.standard_output);
  EXPECT_TRUE(report.at("selected_endpoint").is_null());
  EXPECT_TRUE(report.at("default_render_endpoints").at("console").is_null());
  EXPECT_TRUE(report.at("default_render_endpoints").at("multimedia").is_null());
  EXPECT_TRUE(report.at("default_render_endpoints").at("communications").is_null());
  EXPECT_TRUE(report.at("mat_profiles").at(0).at("format_support_hresult").is_null());
  EXPECT_EQ(
    report.at("mat_profiles").at(0).at("format_support_hresult_label").get<std::string>(),
    "NOT_PROBED");
  EXPECT_TRUE(report.at("gate").at("selected_profile").is_null());
  EXPECT_EQ(report.at("gate").at("verdict").get<std::string>(), "BLOCKED");

  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches an invalid-argument path that contacts the Windows provider before rejecting the CLI.
TEST(AtmosCapabilityProbeApp, RejectsInvalidArgumentsWithoutProviderCall) {
  const std::array arguments {std::string_view {"--bogus"}};
  int provider_calls {};
  const auto result = atmos_probe::run_probe(
    arguments,
    [&provider_calls](const probe_options &) {
      ++provider_calls;
      return canonical_ready_observation();
    });

  EXPECT_EQ(provider_calls, 0);
  EXPECT_EQ(result.exit_code, 2);
  EXPECT_EQ(result.standard_error, "unknown option: --bogus\n");
}

// Catches an exception path that accidentally preserves a green output or a successful exit code.
TEST(AtmosCapabilityProbeApp, MapsThrowingProviderToExitThreeWithoutGreenOutput) {
  const std::array arguments {std::string_view {"--json"}};
  int provider_calls {};
  const auto result = atmos_probe::run_probe(
    arguments,
    [&provider_calls](const probe_options &) -> probe_observation {
      ++provider_calls;
      throw std::runtime_error {"provider ENDPOINT_PREFLIGHT_READY exploded"};
    });

  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.standard_output.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error, "provider/runtime failure\n");
}

// Catches a JSON serialization exception that would otherwise escape the coordinator instead of
// mapping to the documented runtime exit code without a false-green report.
TEST(AtmosCapabilityProbeApp, MapsReportSerializationFailureToExitThreeWithoutGreenOutput) {
  const std::array arguments {std::string_view {"--json"}};
  atmos_probe::app_result result {};

  EXPECT_NO_THROW(result = atmos_probe::run_probe(
    arguments,
    [](const probe_options &) {
      auto observation = canonical_ready_observation();
      observation.selected_endpoint->id =
        std::string {k_ready_token} + std::string(1, static_cast<char>(0xFF));
      return observation;
    }));

  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.standard_output.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error.find(k_ready_token), std::string::npos);
  EXPECT_EQ(result.standard_error, "report/runtime failure\n");
}

// Catches a help implementation that initializes Windows probing instead of returning immediately.
TEST(AtmosCapabilityProbeApp, MapsHelpToExitZeroWithoutProviderCall) {
  const std::array arguments {std::string_view {"--help"}};
  int provider_calls {};
  const auto result = atmos_probe::run_probe(
    arguments,
    [&provider_calls](const probe_options &) {
      ++provider_calls;
      return canonical_ready_observation();
    });

  EXPECT_EQ(provider_calls, 0);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_TRUE(result.standard_output.contains("Usage:"));
  EXPECT_TRUE(result.standard_output.contains(k_preflight_caveat));
  EXPECT_TRUE(result.standard_error.empty());
}

// Catches an HRESULT serializer that loses fixed-width hexadecimal spelling or a known audio
// client failure label in a blocked diagnostic report.
TEST(AtmosCapabilityProbeApp, UsesStableKnownAndUnknownHresultStringsAndLabels) {
  struct hresult_case {
    hresult_code value;
    std::string_view text;
    std::string_view label;
  };
  const std::array<hresult_case, 6> cases {
    hresult_case {.value = 0, .text = "0x00000000", .label = "S_OK"},
    hresult_case {
      .value = static_cast<hresult_code>(0x8889000AU),
      .text = "0x8889000A",
      .label = "AUDCLNT_E_DEVICE_IN_USE",
    },
    hresult_case {
      .value = static_cast<hresult_code>(0x8889000EU),
      .text = "0x8889000E",
      .label = "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED",
    },
    hresult_case {
      .value = static_cast<hresult_code>(0x88890004U),
      .text = "0x88890004",
      .label = "AUDCLNT_E_DEVICE_INVALIDATED",
    },
    hresult_case {
      .value = static_cast<hresult_code>(0x88890008U),
      .text = "0x88890008",
      .label = "AUDCLNT_E_UNSUPPORTED_FORMAT",
    },
    hresult_case {
      .value = static_cast<hresult_code>(0x80004005U),
      .text = "0x80004005",
      .label = "UNKNOWN_HRESULT",
    },
  };
  const std::array arguments {std::string_view {"--json"}};

  for (const auto &test_case : cases) {
    SCOPED_TRACE(std::string {test_case.text});
    const auto result = atmos_probe::run_probe(
      arguments,
      [test_case](const probe_options &) {
        auto observation = canonical_ready_observation();
        observation.mat21.format_support_hresult = test_case.value;
        return observation;
      });
    ASSERT_FALSE(result.standard_output.empty()) << result.standard_error;
    const auto report = parse_report(result.standard_output);
    const auto *mat21 = find_profile(report.at("mat_profiles"), "MAT21");

    ASSERT_NE(mat21, nullptr);
    EXPECT_EQ(mat21->at("format_support_hresult").get<std::string>(), test_case.text);
    EXPECT_EQ(mat21->at("format_support_hresult_label").get<std::string>(), test_case.label);
  }
}

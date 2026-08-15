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
    "Endpoint preflight does not prove downstream receiver Atmos lock.";

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

// Catches an app coordinator that alters an explicit endpoint ID, delegates selection text to the
// provider, emits a non-canonical green report, or reorders the public JSON contract.
TEST(AtmosCapabilityProbeApp, PassesExplicitIdToProviderAndSerializesCanonicalMat21ReadyReport) {
  std::optional<probe_options> captured_options;
  int provider_calls {};
  const std::array arguments {
    std::string_view {"--endpoint-id"},
    k_exact_endpoint_id,
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
  ASSERT_TRUE(captured_options->endpoint_id.has_value());
  EXPECT_EQ(*captured_options->endpoint_id, k_exact_endpoint_id);
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

  EXPECT_EQ(report.at("schema_version").get<int>(), 1);
  EXPECT_EQ(report.at("selection").at("kind").get<std::string>(), "explicit");
  EXPECT_EQ(
    report.at("selection").at("requested_endpoint_id").get<std::string>(),
    k_exact_endpoint_id);
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

  const auto validation = atmos_probe::validate_serialized_report(result.standard_output);
  EXPECT_TRUE(validation.valid) << validation.error;
}

// Catches a validator that accepts an internally inconsistent green report after a prerequisite
// was changed independently of the coordinator's canonical observations.
TEST(AtmosCapabilityProbeApp, RejectsEveryMutatedGreenPrerequisite) {
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
  ASSERT_EQ(result.exit_code, 0) << result.standard_error;
  const auto canonical_report = parse_report(result.standard_output);

  struct mutation_case {
    std::string_view name;
    void (*mutate)(ordered_json &);
  };
  const std::array<mutation_case, 10> mutations {
    mutation_case {
      .name = "requested endpoint ID",
      .mutate = [](ordered_json &report) {
        report["selection"]["requested_endpoint_id"] = "wrong-requested-id";
      },
    },
    mutation_case {
      .name = "selected endpoint ID",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["id"] = "wrong-selected-id";
      },
    },
    mutation_case {
      .name = "endpoint state",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["state"] = 0;
      },
    },
    mutation_case {
      .name = "display form factor",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["form_factor"]["status"] = "OTHER";
      },
    },
    mutation_case {
      .name = "HDMI connector",
      .mutate = [](ordered_json &report) {
        report["selected_endpoint"]["jack_subtype"]["status"] = "DISPLAYPORT";
      },
    },
    mutation_case {
      .name = "active Atmos GUID",
      .mutate = [](ordered_json &report) {
        report["spatial_audio"]["active_format_guid"] =
          "{00000000-0000-0000-0000-000000000001}";
      },
    },
    mutation_case {
      .name = "selected profile ready membership",
      .mutate = [](ordered_json &report) {
        report["gate"]["ready_profiles"] = ordered_json::array({"MAT20"});
      },
    },
    mutation_case {
      .name = "MAT format support HRESULT",
      .mutate = [](ordered_json &report) {
        report["mat_profiles"][0]["format_support_hresult"] = "0x8889000A";
      },
    },
    mutation_case {
      .name = "MAT initialize HRESULT",
      .mutate = [](ordered_json &report) {
        report["mat_profiles"][0]["initialize_hresult"] = "0x8889000A";
      },
    },
    mutation_case {
      .name = "audio bytes written",
      .mutate = [](ordered_json &report) {
        report["audio_bytes_written"] = true;
      },
    },
  };

  for (const auto &mutation : mutations) {
    SCOPED_TRACE(std::string {mutation.name});
    auto mutated = canonical_report;
    mutation.mutate(mutated);

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
  EXPECT_EQ(result.standard_output.find("ENDPOINT_PREFLIGHT_READY"), std::string::npos);
  EXPECT_FALSE(result.standard_error.empty());
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
      throw std::runtime_error {"provider exploded"};
    });

  EXPECT_EQ(provider_calls, 1);
  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.standard_output.find("ENDPOINT_PREFLIGHT_READY"), std::string::npos);
  EXPECT_TRUE(result.standard_error.contains("provider exploded"));
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
      observation.selected_endpoint->id = std::string {"\xFF", 1};
      return observation;
    }));

  EXPECT_EQ(result.exit_code, 3);
  EXPECT_EQ(result.standard_output.find("ENDPOINT_PREFLIGHT_READY"), std::string::npos);
  EXPECT_TRUE(result.standard_error.contains("report/runtime failure"));
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

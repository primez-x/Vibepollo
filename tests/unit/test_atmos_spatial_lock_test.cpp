#include "../tests_common.h"
#include "tools/atmos_spatial_lock_test.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef _WIN32
  #include <ksmedia.h>
#endif

namespace {
  using atmos_spatial_lock::diagnostic;

  constexpr std::string_view endpoint_id =
    "{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";
  constexpr std::string_view winrt_render_id =
    "SWD\\MMDEVAPI\\{0.0.0.00000000}.{11111111-2222-3333-4444-555555555555}";

  atmos_spatial_lock::route_observation ready_route() {
    return {
      .sample_complete = true,
      .endpoint = {
        .present = true,
        .id = std::string {endpoint_id},
        .friendly_name = "HDMI Receiver",
        .state = 1,
        .display_audio = true,
        .hdmi = true,
      },
      .core_default_ids = {
        std::string {endpoint_id},
        std::string {endpoint_id},
        std::string {endpoint_id},
      },
      .winrt_default_id = std::string {winrt_render_id},
      .winrt_communications_id = std::string {winrt_render_id},
      .spatial_configuration_available = true,
      .spatial_configuration_id = std::string {winrt_render_id},
      .spatial_audio_supported = true,
      .atmos_home_theater_supported = true,
      .active_spatial_format = std::string {atmos_spatial_lock::atmos_home_theater_format},
    };
  }

  atmos_spatial_lock::preflight_observation ready_preflight() {
    atmos_spatial_lock::preflight_observation observation {
      .initial_route = ready_route(),
      .pre_start_route = ready_route(),
      .mat10 = {
        .format_support_hresult = 0,
        .initialize_hresult = 0,
      },
      .objects = {
        .spatial_client_hresult = 0,
        .stream_available_hresult = 0,
        .native_static_object_mask = atmos_spatial_lock::required_static_object_mask,
        .max_dynamic_object_count = atmos_spatial_lock::required_dynamic_object_count,
        .supported_format_count = 1,
        .negotiated_format = atmos_spatial_lock::object_format {
          .format_tag = 3,
          .channels = 1,
          .samples_per_second = 48000,
          .average_bytes_per_second = 192000,
          .block_align = 4,
          .bits_per_sample = 32,
          .extra_size = 0,
          .float_pcm = true,
        },
        .format_support_hresult = 0,
        .max_frame_count = 1024,
      },
    };
    return observation;
  }

  bool contains(
    const std::vector<diagnostic> &diagnostics,
    const diagnostic expected) {
    return std::ranges::find(diagnostics, expected) != diagnostics.end();
  }

  void count_cleanup(void *context) noexcept {
    ++*static_cast<int *>(context);
  }
}  // namespace

// Catches defaults that drift from the documented bounded staging command.
TEST(AtmosSpatialLockTest, ParsesSafeDefaultsAndStableStagingCommand) {
  const auto defaults = atmos_spatial_lock::parse_options({});
  ASSERT_TRUE(defaults.value.has_value()) << defaults.error;
  EXPECT_TRUE(defaults.value->json);
  EXPECT_EQ(defaults.value->duration_ms, 8000u);
  EXPECT_DOUBLE_EQ(defaults.value->frequency_hz, 440.0);
  EXPECT_FLOAT_EQ(defaults.value->amplitude, 0.02f);

  constexpr std::array arguments {
    std::string_view {"--json"},
    std::string_view {"--duration-ms"},
    std::string_view {"8000"},
    std::string_view {"--frequency-hz"},
    std::string_view {"440"},
    std::string_view {"--amplitude"},
    std::string_view {"0.02"},
  };
  const auto parsed = atmos_spatial_lock::parse_options(arguments);
  ASSERT_TRUE(parsed.value.has_value()) << parsed.error;
  EXPECT_TRUE(parsed.value->json);
  EXPECT_EQ(parsed.value->duration_ms, 8000u);
  EXPECT_DOUBLE_EQ(parsed.value->frequency_hz, 440.0);
  EXPECT_FLOAT_EQ(parsed.value->amplitude, 0.02f);
}

// Catches unsafe numeric values reaching the renderer or ambiguous duplicate options.
TEST(AtmosSpatialLockTest, ClampsFiniteValuesAndRejectsInvalidOrDuplicateOptions) {
  constexpr std::array clamped_arguments {
    std::string_view {"--duration-ms=-5"},
    std::string_view {"--frequency-hz=99999"},
    std::string_view {"--amplitude=1"},
  };
  const auto clamped = atmos_spatial_lock::parse_options(clamped_arguments);
  ASSERT_TRUE(clamped.value.has_value()) << clamped.error;
  EXPECT_EQ(clamped.value->duration_ms, atmos_spatial_lock::minimum_duration_ms);
  EXPECT_DOUBLE_EQ(clamped.value->frequency_hz, atmos_spatial_lock::maximum_frequency_hz);
  EXPECT_FLOAT_EQ(clamped.value->amplitude, atmos_spatial_lock::maximum_amplitude);

  constexpr std::array duplicate {
    std::string_view {"--duration-ms"},
    std::string_view {"1000"},
    std::string_view {"--duration-ms"},
    std::string_view {"2000"},
  };
  EXPECT_FALSE(atmos_spatial_lock::parse_options(duplicate).value.has_value());

  constexpr std::array non_finite {
    std::string_view {"--amplitude"},
    std::string_view {"nan"},
  };
  EXPECT_FALSE(atmos_spatial_lock::parse_options(non_finite).value.has_value());

  constexpr std::array unknown {std::string_view {"--endpoint-id"}};
  EXPECT_FALSE(atmos_spatial_lock::parse_options(unknown).value.has_value());
}

// Catches a gate that starts despite any missing exact machine-side route or object prerequisite.
TEST(AtmosSpatialLockTest, PreflightRequiresEveryExactRouteAndObjectGate) {
  const auto ready = atmos_spatial_lock::evaluate_preflight(ready_preflight());
  EXPECT_TRUE(ready.ready);
  EXPECT_TRUE(ready.diagnostics.empty());

  struct failure_case {
    const char *name;
    std::function<void(atmos_spatial_lock::preflight_observation &)> mutate;
    diagnostic expected;
  };
  const std::vector<failure_case> cases {
    {"inactive endpoint", [](auto &value) { value.initial_route.endpoint.state = 2; }, diagnostic::endpoint_not_active},
    {"not display audio", [](auto &value) { value.initial_route.endpoint.display_audio = false; }, diagnostic::endpoint_not_display_audio},
    {"not hdmi", [](auto &value) { value.initial_route.endpoint.hdmi = false; }, diagnostic::endpoint_not_hdmi},
    {"core role mismatch", [](auto &value) { value.initial_route.core_default_ids[1] = "other"; }, diagnostic::core_defaults_not_exact},
    {"winrt role mismatch", [](auto &value) { value.initial_route.winrt_communications_id = "other"; }, diagnostic::winrt_defaults_not_exact},
    {"winrt ids empty", [](auto &value) {
       value.initial_route.winrt_default_id.clear();
       value.initial_route.winrt_communications_id.clear();
       value.initial_route.spatial_configuration_id.clear();
     }, diagnostic::winrt_defaults_not_exact},
    {"winrt namespace collapses to mmdevice", [](auto &value) {
       value.initial_route.winrt_default_id = std::string {endpoint_id};
       value.initial_route.winrt_communications_id = std::string {endpoint_id};
       value.initial_route.spatial_configuration_id = std::string {endpoint_id};
    }, diagnostic::winrt_defaults_not_exact},
    {"spatial id empty", [](auto &value) { value.initial_route.spatial_configuration_id.clear(); }, diagnostic::spatial_configuration_id_mismatch},
    {"spatial id mismatch", [](auto &value) { value.initial_route.spatial_configuration_id = "other"; }, diagnostic::spatial_configuration_id_mismatch},
    {"spatial id incorrectly matches mmdevice", [](auto &value) { value.initial_route.spatial_configuration_id = std::string {endpoint_id}; }, diagnostic::spatial_configuration_id_mismatch},
    {"spatial unsupported", [](auto &value) { value.initial_route.spatial_audio_supported = false; }, diagnostic::spatial_audio_unsupported},
    {"atmos unsupported", [](auto &value) { value.initial_route.atmos_home_theater_supported = false; }, diagnostic::atmos_home_theater_unsupported},
    {"wrong active format", [](auto &value) { value.initial_route.active_spatial_format = "other"; }, diagnostic::active_spatial_format_not_atmos},
    {"route changed", [](auto &value) { value.pre_start_route.winrt_default_id = "other"; }, diagnostic::route_not_stable},
    {"mat support not exact s ok", [](auto &value) { value.mat10.format_support_hresult = 1; }, diagnostic::mat10_format_unsupported},
    {"mat initialize not exact s ok", [](auto &value) { value.mat10.initialize_hresult = static_cast<std::int32_t>(0x88890008u); }, diagnostic::mat10_initialize_failed},
    {"spatial stream unavailable", [](auto &value) { value.objects.stream_available_hresult = static_cast<std::int32_t>(0x88890107u); }, diagnostic::spatial_stream_unavailable},
    {"dynamic capacity not exact", [](auto &value) { value.objects.max_dynamic_object_count = 19; }, diagnostic::dynamic_object_count_not_20},
    {"incomplete bed", [](auto &value) { *value.objects.native_static_object_mask &= ~0x1000u; }, diagnostic::static_bed_unsupported},
    {"not float pcm", [](auto &value) { value.objects.negotiated_format->float_pcm = false; }, diagnostic::float_object_format_unavailable},
    {"noncanonical extensible size", [](auto &value) {
       value.objects.negotiated_format->format_tag = 0xfffe;
       value.objects.negotiated_format->extra_size = 23;
     }, diagnostic::float_object_format_unavailable},
    {"object format unsupported", [](auto &value) { value.objects.format_support_hresult = 1; }, diagnostic::object_format_unsupported},
    {"missing max frames", [](auto &value) { value.objects.max_frame_count.reset(); }, diagnostic::max_frame_count_unavailable},
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.name);
    auto observation = ready_preflight();
    test_case.mutate(observation);
    const auto result = atmos_spatial_lock::evaluate_preflight(observation);
    EXPECT_FALSE(result.ready);
    EXPECT_TRUE(contains(result.diagnostics, test_case.expected));
  }
}

// Catches treating the native static-object capability as an exact equality
// instead of a bitwise capability mask. Extra advertised types are ignored;
// every required 7.1.4 bit must still be present.
TEST(AtmosSpatialLockTest, NativeStaticMaskContainsRequiredSevenPointOnePointFourBits) {
  auto observation = ready_preflight();
  *observation.objects.native_static_object_mask = 0xC1FFE;
  const auto actual_capabilities = atmos_spatial_lock::evaluate_preflight(observation);
  EXPECT_TRUE(actual_capabilities.ready);
  EXPECT_FALSE(contains(actual_capabilities.diagnostics, diagnostic::static_bed_unsupported));

  auto dynamic_bit = observation;
  *dynamic_bit.objects.native_static_object_mask =
    atmos_spatial_lock::required_static_object_mask | 0x1u;
  const auto dynamic_bit_result = atmos_spatial_lock::evaluate_preflight(dynamic_bit);
  EXPECT_FALSE(dynamic_bit_result.ready);
  EXPECT_TRUE(contains(dynamic_bit_result.diagnostics, diagnostic::static_bed_unsupported));

  auto unknown_future_bit = observation;
  *unknown_future_bit.objects.native_static_object_mask =
    atmos_spatial_lock::required_static_object_mask | 0x80000000u;
  const auto unknown_future_bit_result =
    atmos_spatial_lock::evaluate_preflight(unknown_future_bit);
  EXPECT_TRUE(unknown_future_bit_result.ready);
  EXPECT_FALSE(contains(
    unknown_future_bit_result.diagnostics,
    diagnostic::static_bed_unsupported));

  for (std::uint32_t bit = 1; bit <= atmos_spatial_lock::required_static_object_mask; bit <<= 1) {
    if ((atmos_spatial_lock::required_static_object_mask & bit) == 0) {
      continue;
    }
    auto missing_required_bit = observation;
    *missing_required_bit.objects.native_static_object_mask &= ~bit;
    const auto result = atmos_spatial_lock::evaluate_preflight(missing_required_bit);
    EXPECT_FALSE(result.ready) << "bit 0x" << std::hex << bit;
    EXPECT_TRUE(contains(result.diagnostics, diagnostic::static_bed_unsupported))
      << "bit 0x" << std::hex << bit;
  }
}

// Catches route comparisons that ignore a changed Core Audio or WinRT bookend.
TEST(AtmosSpatialLockTest, RouteStabilityRequiresExactBookends) {
  const auto before = ready_route();
  auto after = before;
  EXPECT_TRUE(atmos_spatial_lock::routes_stable(before, after));

  after.core_default_ids[2] = "other";
  EXPECT_FALSE(atmos_spatial_lock::routes_stable(before, after));
  after = before;
  after.spatial_configuration_id = "other";
  EXPECT_FALSE(atmos_spatial_lock::routes_stable(before, after));
  after = before;
  after.active_spatial_format = "other";
  EXPECT_FALSE(atmos_spatial_lock::routes_stable(before, after));

  auto collapsed_namespaces = before;
  collapsed_namespaces.winrt_default_id = std::string {endpoint_id};
  collapsed_namespaces.winrt_communications_id = std::string {endpoint_id};
  collapsed_namespaces.spatial_configuration_id = std::string {endpoint_id};
  EXPECT_FALSE(atmos_spatial_lock::routes_stable(
    collapsed_namespaces,
    collapsed_namespaces));
}

// Catches object properties being set before BeginUpdatingAudioObjects, after
// EndUpdatingAudioObjects, or after buffer submission has already started.
TEST(AtmosSpatialLockTest, ObjectStateChangesRequireTheOpenMetadataPhase) {
  using atmos_spatial_lock::detail::object_update_action;
  using atmos_spatial_lock::detail::object_update_lifecycle;

  object_update_lifecycle lifecycle;
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_position));
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_volume));
  EXPECT_FALSE(lifecycle.end_update());

  EXPECT_TRUE(lifecycle.begin_update());
  EXPECT_FALSE(lifecycle.begin_update());
  EXPECT_TRUE(lifecycle.accept(object_update_action::set_position));
  EXPECT_TRUE(lifecycle.accept(object_update_action::set_volume));
  EXPECT_TRUE(lifecycle.accept(object_update_action::get_buffer));
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_position));
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_volume));
  EXPECT_TRUE(lifecycle.accept(object_update_action::set_end_of_stream));
  EXPECT_TRUE(lifecycle.end_update());

  EXPECT_FALSE(lifecycle.accept(object_update_action::set_position));
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_volume));
  EXPECT_FALSE(lifecycle.accept(object_update_action::get_buffer));
}

// Catches the first successful update failing to put both initial dynamic
// properties before every object buffer in the 12-static-plus-1 scene.
TEST(AtmosSpatialLockTest, FirstUpdateAcceptsInitialPropertiesBeforeAllThirteenBuffers) {
  using atmos_spatial_lock::detail::object_update_action;
  using atmos_spatial_lock::detail::object_update_lifecycle;

  object_update_lifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin_update());
  ASSERT_TRUE(lifecycle.accept(object_update_action::set_position));
  ASSERT_TRUE(lifecycle.accept(object_update_action::set_volume));
  for (std::size_t object_index = 0; object_index < 13; ++object_index) {
    SCOPED_TRACE(object_index);
    EXPECT_TRUE(lifecycle.accept(object_update_action::get_buffer));
  }
  EXPECT_TRUE(lifecycle.end_update());
}

// Catches a failed property setter being followed by buffer access instead of
// failing the update closed while preserving the matching End call.
TEST(AtmosSpatialLockTest, SetterFailureRejectsBuffersButStillAllowsBalancedEnd) {
  using atmos_spatial_lock::detail::object_update_action;
  using atmos_spatial_lock::detail::object_update_lifecycle;

  object_update_lifecycle lifecycle;
  ASSERT_TRUE(lifecycle.begin_update());
  ASSERT_TRUE(lifecycle.accept(object_update_action::set_position));
  EXPECT_TRUE(lifecycle.fail_update());
  EXPECT_FALSE(lifecycle.accept(object_update_action::set_volume));
  EXPECT_FALSE(lifecycle.accept(object_update_action::get_buffer));
  EXPECT_TRUE(lifecycle.end_update());
}

// Catches exception paths that leave a successful Start or BeginUpdating call
// without its required no-throw cleanup.
TEST(AtmosSpatialLockTest, NoexceptCleanupGuardBalancesExactlyOnce) {
  using atmos_spatial_lock::detail::noexcept_cleanup_guard;
  static_assert(std::is_nothrow_destructible_v<noexcept_cleanup_guard>);

  int cleanup_calls {};
  try {
    noexcept_cleanup_guard guard {&cleanup_calls, count_cleanup};
    guard.arm();
    throw std::runtime_error {"injected allocation/report failure"};
  } catch (const std::runtime_error &) {
  }
  EXPECT_EQ(cleanup_calls, 1);

  cleanup_calls = 0;
  {
    noexcept_cleanup_guard guard {&cleanup_calls, count_cleanup};
    guard.arm();
    guard.invoke();
    guard.invoke();
  }
  EXPECT_EQ(cleanup_calls, 1);
}

// Catches pre-Start failures, including COM apartment initialization, being
// misreported as post-Start renderer failures.
TEST(AtmosSpatialLockTest, FailureExitCodeDistinguishesPreAndPostStart) {
  EXPECT_EQ(atmos_spatial_lock::failure_exit_code(false), 2);
  EXPECT_EQ(atmos_spatial_lock::failure_exit_code(true), 3);
}

// Catches sample generation that exceeds the scene amplitude or writes non-float payloads.
TEST(AtmosSpatialLockTest, GeneratesOnlyBoundedFiniteFloatPcm) {
  std::array<float, 480> samples {};
  const float peak = atmos_spatial_lock::fill_scene_object(
    samples,
    48000,
    440.0,
    0.02f,
    0,
    3,
    13);

  EXPECT_GT(peak, 0.0f);
  EXPECT_LE(peak, 0.02f / std::sqrt(13.0f) + 1.0e-6f);
  EXPECT_TRUE(std::ranges::any_of(samples, [](const float sample) { return sample != 0.0f; }));
  EXPECT_TRUE(std::ranges::all_of(samples, [](const float sample) { return std::isfinite(sample); }));
}

// Catches a generated-scene submission/proof-scope schema that omits its
// explicit statement that no receiver-lock measurement was performed.
TEST(AtmosSpatialLockTest, SerializesRequiredJsonEvidenceAndHresultLabels) {
  atmos_spatial_lock::run_report report {
    .ready = true,
    .endpoint = ready_route().endpoint,
    .negotiated_format = ready_preflight().objects.negotiated_format,
    .object_capabilities = ready_preflight().objects,
    .update_count = 375,
    .frame_count = 384000,
    .requested_duration_ms = 8000,
    .submitted_duration_ms = 8000.0,
    .wall_elapsed_ms = 8100.0,
    .peak_sample = 0.005f,
    .hresults = {{"ISpatialAudioObjectRenderStream::Start", 0}},
    .started = true,
    .stopped = true,
    .route_stable = true,
    .audio_generated = true,
    .dynamic_object_activated = true,
  };

  const auto json = nlohmann::json::parse(atmos_spatial_lock::serialize_report(report));
  EXPECT_EQ(json.at("tool"), "atmos-spatial-lock-test");
  EXPECT_EQ(
    json.at("proof_scope"),
    "machine-side Windows spatial generated-scene API submission evidence for the active HDMI/eARC route");
  EXPECT_EQ(
    json.at("ready_meaning"),
    "the exact machine-side API and route contract completed successfully");
  EXPECT_TRUE(json.at("physical_receiver_lock_requires_external_observation"));
  EXPECT_EQ(
    json.at("does_not_prove"),
    nlohmann::json::array({
      "physical receiver lock state without external observation",
      "native-content provenance",
      "upmixed versus non-spatial content discrimination",
      "bit-exact relay",
      "end-to-end host-to-client relay integrity",
      "render drain or downstream playback completion",
    }));
  EXPECT_EQ(json.at("endpoint").at("id"), endpoint_id);
  EXPECT_EQ(json.at("negotiated_object_format").at("samples_per_second"), 48000);
  EXPECT_EQ(json.at("object_capabilities").at("max_dynamic_object_count"), 20);
  EXPECT_EQ(json.at("update_count"), 375);
  EXPECT_EQ(json.at("frame_count"), 384000);
  EXPECT_EQ(json.at("requested_duration_ms"), 8000);
  EXPECT_FALSE(json.contains("actual_duration_ms"));
  EXPECT_DOUBLE_EQ(json.at("submitted_duration_ms").get<double>(), 8000.0);
  EXPECT_DOUBLE_EQ(json.at("wall_elapsed_ms").get<double>(), 8100.0);
  EXPECT_FALSE(json.at("drain_completion_observed"));
  EXPECT_FLOAT_EQ(json.at("peak_sample").get<float>(), 0.005f);
  EXPECT_EQ(json.at("hresults").at(0).at("label"), "S_OK");
  EXPECT_TRUE(json.at("started"));
  EXPECT_TRUE(json.at("stopped"));
  EXPECT_TRUE(json.at("route_stable"));
  EXPECT_TRUE(json.at("audio_generated"));

  const auto help = atmos_spatial_lock::help_text();
  EXPECT_NE(help.find("submitted frames"), std::string::npos);
  EXPECT_NE(help.find("does not observe render drain"), std::string::npos);
}

// Catches byte-count arithmetic that overflows before the render span is built.
TEST(AtmosSpatialLockTest, ChecksFloatBufferSizingBeforeNarrowing) {
  constexpr auto max_frame_count =
    std::numeric_limits<std::uint32_t>::max() / sizeof(float);

  const auto normal = atmos_spatial_lock::checked_float_buffer_bytes(1024, 1024);
  ASSERT_TRUE(normal.has_value());
  EXPECT_EQ(*normal, 1024u * sizeof(float));

  const auto exact_limit = atmos_spatial_lock::checked_float_buffer_bytes(
    max_frame_count,
    max_frame_count);
  ASSERT_TRUE(exact_limit.has_value());
  EXPECT_EQ(
    *exact_limit,
    std::numeric_limits<std::uint32_t>::max() -
      (std::numeric_limits<std::uint32_t>::max() % sizeof(float)));

  EXPECT_FALSE(atmos_spatial_lock::checked_float_buffer_bytes(
    max_frame_count + 1,
    max_frame_count + 1));
  EXPECT_FALSE(atmos_spatial_lock::checked_float_buffer_bytes(1024, 1025));
}

#ifdef _WIN32
// Catches an abbreviated subtype-only check or any drift in the complete 52-byte MAT10 descriptor.
TEST(AtmosSpatialLockTest, BuildsCompleteCanonicalMat10MlpDescriptor) {
  const auto format = atmos_spatial_lock::make_mat10_format();
  static_assert(sizeof(format) == 52);
  EXPECT_EQ(format.FormatExt.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
  EXPECT_EQ(format.FormatExt.Format.nChannels, 8);
  EXPECT_EQ(format.FormatExt.Format.nSamplesPerSec, 192000u);
  EXPECT_EQ(format.FormatExt.Format.nAvgBytesPerSec, 3072000u);
  EXPECT_EQ(format.FormatExt.Format.nBlockAlign, 16);
  EXPECT_EQ(format.FormatExt.Format.wBitsPerSample, 16);
  EXPECT_EQ(format.FormatExt.Format.cbSize, 34);
  EXPECT_EQ(format.FormatExt.Samples.wValidBitsPerSample, 16);
  EXPECT_EQ(format.FormatExt.dwChannelMask, KSAUDIO_SPEAKER_7POINT1);
  EXPECT_TRUE(IsEqualGUID(format.FormatExt.SubFormat, atmos_spatial_lock::mat10_mlp_subformat));
  EXPECT_EQ(format.dwEncodedSamplesPerSec, 96000u);
  EXPECT_EQ(format.dwEncodedChannelCount, 8u);
  EXPECT_EQ(format.dwAverageBytesPerSec, 0u);
}
#endif

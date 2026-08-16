#include "../../../tests_common.h"
#include "tools/atmos_pcm_spatial_probe.h"

#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
  constexpr std::uint32_t exact_channel_mask = 0x2D63FU;
}

TEST(AtmosPcmSpatialProbe, BuildsExactSevenPointOneFourFloatCandidate) {
  const auto candidate = atmos_pcm_probe::make_exact_candidate_format();

  EXPECT_EQ(candidate.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
  EXPECT_EQ(candidate.Format.nChannels, 12U);
  EXPECT_EQ(candidate.Format.nSamplesPerSec, 48000U);
  EXPECT_EQ(candidate.Format.nBlockAlign, 48U);
  EXPECT_EQ(candidate.Format.nAvgBytesPerSec, 2304000U);
  EXPECT_EQ(candidate.Format.wBitsPerSample, 32U);
  EXPECT_EQ(candidate.Format.cbSize, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
  EXPECT_EQ(candidate.Samples.wValidBitsPerSample, 32U);
  EXPECT_EQ(candidate.dwChannelMask, exact_channel_mask);
  EXPECT_TRUE(IsEqualGUID(candidate.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
}

TEST(AtmosPcmSpatialProbe, ExactCandidateValidatorRejectsAnyDescriptorDrift) {
  auto candidate = atmos_pcm_probe::make_exact_candidate_format();
  EXPECT_TRUE(atmos_pcm_probe::is_exact_candidate_format(
    reinterpret_cast<const WAVEFORMATEX *>(&candidate)));

  const std::array<void (*)(WAVEFORMATEXTENSIBLE &), 7> mutations {
    [](WAVEFORMATEXTENSIBLE &value) { value.Format.nChannels = 8; },
    [](WAVEFORMATEXTENSIBLE &value) { value.Format.nSamplesPerSec = 44100; },
    [](WAVEFORMATEXTENSIBLE &value) { value.Format.nBlockAlign = 24; },
    [](WAVEFORMATEXTENSIBLE &value) { value.Format.nAvgBytesPerSec = 1152000; },
    [](WAVEFORMATEXTENSIBLE &value) { value.Samples.wValidBitsPerSample = 24; },
    [](WAVEFORMATEXTENSIBLE &value) { value.dwChannelMask = 0; },
    [](WAVEFORMATEXTENSIBLE &value) { value.SubFormat = KSDATAFORMAT_SUBTYPE_PCM; },
  };

  for (const auto &mutate : mutations) {
    auto mutated = candidate;
    mutate(mutated);
    EXPECT_FALSE(atmos_pcm_probe::is_exact_candidate_format(
      reinterpret_cast<const WAVEFORMATEX *>(&mutated)));
  }
}

TEST(AtmosPcmSpatialProbe, ParsesJsonHelpAndOpaqueEndpointSelection) {
  const std::array arguments {
    std::string_view {"--json"},
    std::string_view {"--endpoint-id"},
    std::string_view {"opaque-mmdevice-id"},
  };
  const auto parsed = atmos_pcm_probe::parse_cli(arguments);
  ASSERT_TRUE(parsed.options.has_value());
  EXPECT_TRUE(parsed.options->json);
  EXPECT_FALSE(parsed.options->help);
  ASSERT_TRUE(parsed.options->endpoint_id.has_value());
  EXPECT_EQ(*parsed.options->endpoint_id, "opaque-mmdevice-id");
  EXPECT_TRUE(atmos_pcm_probe::help_text().contains("atmos-pcm-spatial-probe"));
}

TEST(AtmosPcmSpatialProbe, SerializesDeterministicSchemaAndTreatsUnsupportedAsObservation) {
  const std::array arguments {
    std::string_view {"--json"},
    std::string_view {"--endpoint-id"},
    std::string_view {"opaque-mmdevice-id"},
  };
  const auto result = atmos_pcm_probe::run_probe(
    arguments,
    [](const atmos_pcm_probe::probe_options &) {
      atmos_pcm_probe::probe_observation observation {};
      atmos_pcm_probe::endpoint_observation endpoint {
        .id = "opaque-mmdevice-id",
        .friendly_name = "Synthetic endpoint",
        .state = DEVICE_STATE_ACTIVE,
      };
      endpoint.exact_candidate.format_support_hresult =
        static_cast<atmos_pcm_probe::hresult_code>(0x88890008U);
      endpoint.exact_candidate.loopback_initialize_hresult =
        static_cast<atmos_pcm_probe::hresult_code>(0x88890008U);
      endpoint.spatial.activation_hresult =
        static_cast<atmos_pcm_probe::hresult_code>(0x80004002U);
      observation.endpoints.push_back(std::move(endpoint));
      return observation;
    });

  ASSERT_EQ(result.exit_code, 0);
  const auto report = nlohmann::json::parse(result.standard_output);
  EXPECT_EQ(report.at("schema_version"), 1);
  EXPECT_FALSE(report.at("audio_bytes_written").get<bool>());
  EXPECT_EQ(report.at("selection").at("requested_endpoint_id"), "opaque-mmdevice-id");
  EXPECT_EQ(report.at("endpoints").size(), 1U);
  EXPECT_EQ(report.at("probe_errors").size(), 0U);
}

TEST(AtmosPcmSpatialProbe, ProductionProbeFilesRemainStrictlyNoWrite) {
  const auto source_root = std::filesystem::path {SUNSHINE_SOURCE_DIR};
  const std::array relative_paths {
    std::string_view {"tools/atmos_pcm_spatial_probe.h"},
    std::string_view {"tools/atmos_pcm_spatial_probe.cpp"},
    std::string_view {"tools/atmos_pcm_spatial_probe_windows.cpp"},
    std::string_view {"tools/atmos_pcm_spatial_probe_main.cpp"},
  };
  const std::array forbidden_tokens {
    std::string_view {"IAudioRenderClient"},
    std::string_view {"GetBuffer"},
    std::string_view {"Start("},
    std::string_view {"SetDeviceFormat"},
    std::string_view {"SetDefaultEndpoint"},
    std::string_view {"SetEndpointVisibility"},
    std::string_view {"AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM"},
    std::string_view {"AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY"},
  };

  for (const auto relative_path : relative_paths) {
    SCOPED_TRACE(std::string {relative_path});
    std::ifstream input {source_root / relative_path, std::ios::binary};
    ASSERT_TRUE(input.good());
    const std::string source {
      std::istreambuf_iterator<char> {input},
      std::istreambuf_iterator<char> {},
    };
    for (const auto forbidden : forbidden_tokens) {
      EXPECT_EQ(source.find(forbidden), std::string::npos)
        << "forbidden audio-write token: " << forbidden;
    }
  }
}

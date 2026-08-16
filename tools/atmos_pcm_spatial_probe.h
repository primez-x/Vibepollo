#pragma once

#include <windows.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atmos_pcm_probe {
  using hresult_code = std::int32_t;

  inline constexpr std::uint32_t exact_candidate_channel_mask = 0x2D63FU;
  inline constexpr std::uint32_t required_static_object_mask = 0x1FFEU;

  struct format_details {
    std::uint16_t format_tag {};
    std::uint16_t channels {};
    std::uint32_t samples_per_second {};
    std::uint32_t average_bytes_per_second {};
    std::uint16_t block_align {};
    std::uint16_t bits_per_sample {};
    std::uint16_t extra_size {};
    std::uint16_t valid_bits_per_sample {};
    std::uint32_t channel_mask {};
    std::string subformat;
  };

  struct api_error {
    std::string operation;
    hresult_code hresult {};
  };

  struct exact_candidate_observation {
    std::optional<hresult_code> format_support_hresult;
    std::optional<format_details> closest_format;
    std::optional<hresult_code> loopback_initialize_hresult;
  };

  struct supported_object_format_observation {
    std::optional<hresult_code> get_format_hresult;
    std::optional<format_details> format;
  };

  struct spatial_observation {
    std::optional<hresult_code> activation_hresult;
    std::optional<hresult_code> native_static_object_mask_hresult;
    std::optional<std::uint32_t> native_static_object_mask;
    std::optional<hresult_code> max_dynamic_object_count_hresult;
    std::optional<std::uint32_t> max_dynamic_object_count;
    std::optional<hresult_code> supported_format_enumerator_hresult;
    std::optional<hresult_code> supported_format_count_hresult;
    std::optional<std::uint32_t> supported_format_count;
    std::vector<supported_object_format_observation> supported_formats;
    bool has_required_static_object_mask {};
    bool has_exact_static_bed_format {};
  };

  struct endpoint_observation {
    std::string id;
    std::string friendly_name;
    std::uint32_t state {};
    bool console_default {};
    bool multimedia_default {};
    bool communications_default {};
    std::optional<format_details> mix_format;
    std::optional<hresult_code> mix_format_hresult;
    exact_candidate_observation exact_candidate;
    spatial_observation spatial;
  };

  struct probe_options {
    std::optional<std::string> endpoint_id;
  };

  struct probe_observation {
    bool probe_complete {true};
    std::vector<endpoint_observation> endpoints;
    std::vector<api_error> errors;
  };

  struct cli_options {
    bool json {};
    bool help {};
    std::optional<std::string> endpoint_id;
  };

  struct cli_parse_result {
    std::optional<cli_options> options;
    std::string error;
  };

  struct app_result {
    int exit_code {};
    std::string standard_output;
    std::string standard_error;
  };

  using observation_provider =
    std::function<probe_observation(const probe_options &)>;

  WAVEFORMATEXTENSIBLE make_exact_candidate_format();
  bool is_exact_candidate_format(const WAVEFORMATEX *format);

  inline bool is_exact_candidate_format(const WAVEFORMATEX &format) {
    return is_exact_candidate_format(&format);
  }

  cli_parse_result parse_cli(std::span<const std::string_view> arguments);
  std::string help_text();
  std::string serialize_report(
    const cli_options &options,
    const probe_observation &observation);
  app_result run_probe(
    std::span<const std::string_view> arguments,
    const observation_provider &provider);
  std::string state_name(std::uint32_t state);
  std::string hresult_text(hresult_code value);
  std::string hresult_label(hresult_code value);

  probe_observation collect_windows_observation(const probe_options &options);
}  // namespace atmos_pcm_probe

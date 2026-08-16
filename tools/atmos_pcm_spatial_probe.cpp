#include "tools/atmos_pcm_spatial_probe.h"

extern "C" const GUID KSDATAFORMAT_SUBTYPE_PCM {
  0x00000001,
  0x0000,
  0x0010,
  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};

extern "C" const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {
  0x00000003,
  0x0000,
  0x0010,
  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71},
};

#include <mmdeviceapi.h>
#include <nlohmann/json.hpp>

#include <iomanip>
#include <sstream>
#include <utility>

namespace {
  using atmos_pcm_probe::api_error;
  using atmos_pcm_probe::endpoint_observation;
  using atmos_pcm_probe::format_details;
  using atmos_pcm_probe::hresult_code;
  using atmos_pcm_probe::probe_observation;
  using atmos_pcm_probe::spatial_observation;
  using atmos_pcm_probe::supported_object_format_observation;
  using ordered_json = nlohmann::ordered_json;

  std::uint32_t hresult_bits(const hresult_code value) {
    return static_cast<std::uint32_t>(value);
  }

  ordered_json optional_hresult(const std::optional<hresult_code> &value) {
    if (!value) {
      return nullptr;
    }
    return atmos_pcm_probe::hresult_text(*value);
  }

  std::string optional_hresult_label(const std::optional<hresult_code> &value) {
    return value ? atmos_pcm_probe::hresult_label(*value) : "NOT_PROBED";
  }

  ordered_json format_json(const format_details &format) {
    return {
      {"format_tag", format.format_tag},
      {"channels", format.channels},
      {"samples_per_second", format.samples_per_second},
      {"average_bytes_per_second", format.average_bytes_per_second},
      {"block_align", format.block_align},
      {"bits_per_sample", format.bits_per_sample},
      {"extra_size", format.extra_size},
      {"valid_bits_per_sample", format.valid_bits_per_sample},
      {"channel_mask", format.channel_mask},
      {"subformat", format.subformat},
    };
  }

  ordered_json format_json(const std::optional<format_details> &format) {
    if (!format) {
      return nullptr;
    }
    return format_json(*format);
  }

  format_details exact_candidate_details() {
    return {
      .format_tag = WAVE_FORMAT_EXTENSIBLE,
      .channels = 12,
      .samples_per_second = 48000,
      .average_bytes_per_second = 2304000,
      .block_align = 48,
      .bits_per_sample = 32,
      .extra_size = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX),
      .valid_bits_per_sample = 32,
      .channel_mask = atmos_pcm_probe::exact_candidate_channel_mask,
      .subformat = "{00000003-0000-0010-8000-00AA00389B71}",
    };
  }

  ordered_json supported_format_json(
    const supported_object_format_observation &observation) {
    return {
      {"get_format_hresult", optional_hresult(observation.get_format_hresult)},
      {"get_format_hresult_label", optional_hresult_label(observation.get_format_hresult)},
      {"format", format_json(observation.format)},
    };
  }

  ordered_json spatial_json(const spatial_observation &observation) {
    ordered_json supported_formats = ordered_json::array();
    for (const auto &format : observation.supported_formats) {
      supported_formats.push_back(supported_format_json(format));
    }
    return {
      {"activation_hresult", optional_hresult(observation.activation_hresult)},
      {"activation_hresult_label", optional_hresult_label(observation.activation_hresult)},
      {"native_static_object_mask_hresult", optional_hresult(observation.native_static_object_mask_hresult)},
      {"native_static_object_mask_hresult_label", optional_hresult_label(observation.native_static_object_mask_hresult)},
      {"native_static_object_mask", observation.native_static_object_mask},
      {"max_dynamic_object_count_hresult", optional_hresult(observation.max_dynamic_object_count_hresult)},
      {"max_dynamic_object_count_hresult_label", optional_hresult_label(observation.max_dynamic_object_count_hresult)},
      {"max_dynamic_object_count", observation.max_dynamic_object_count},
      {"supported_format_enumerator_hresult", optional_hresult(observation.supported_format_enumerator_hresult)},
      {"supported_format_enumerator_hresult_label", optional_hresult_label(observation.supported_format_enumerator_hresult)},
      {"supported_format_count_hresult", optional_hresult(observation.supported_format_count_hresult)},
      {"supported_format_count_hresult_label", optional_hresult_label(observation.supported_format_count_hresult)},
      {"supported_format_count", observation.supported_format_count},
      {"supported_formats", std::move(supported_formats)},
      {"has_required_static_object_mask", observation.has_required_static_object_mask},
      {"has_exact_static_bed_format", observation.has_exact_static_bed_format},
    };
  }

  ordered_json endpoint_json(const endpoint_observation &endpoint) {
    return {
      {"id", endpoint.id},
      {"friendly_name", endpoint.friendly_name},
      {"state", endpoint.state},
      {"state_name", atmos_pcm_probe::state_name(endpoint.state)},
      {"default_roles", {
        {"console", endpoint.console_default},
        {"multimedia", endpoint.multimedia_default},
        {"communications", endpoint.communications_default},
      }},
      {"console_default", endpoint.console_default},
      {"multimedia_default", endpoint.multimedia_default},
      {"communications_default", endpoint.communications_default},
      {"mix_format_hresult", optional_hresult(endpoint.mix_format_hresult)},
      {"mix_format_hresult_label", optional_hresult_label(endpoint.mix_format_hresult)},
      {"mix_format", format_json(endpoint.mix_format)},
      {"exact_candidate", {
        {"format", format_json(exact_candidate_details())},
        {"is_exact", true},
        {"format_support_hresult", optional_hresult(endpoint.exact_candidate.format_support_hresult)},
        {"format_support_hresult_label", optional_hresult_label(endpoint.exact_candidate.format_support_hresult)},
        {"closest_format", format_json(endpoint.exact_candidate.closest_format)},
        {"loopback_initialize_hresult", optional_hresult(endpoint.exact_candidate.loopback_initialize_hresult)},
        {"loopback_initialize_hresult_label", optional_hresult_label(endpoint.exact_candidate.loopback_initialize_hresult)},
      }},
      {"spatial", spatial_json(endpoint.spatial)},
    };
  }

  ordered_json error_json(const api_error &error) {
    return {
      {"operation", error.operation},
      {"hresult", atmos_pcm_probe::hresult_text(error.hresult)},
      {"hresult_label", atmos_pcm_probe::hresult_label(error.hresult)},
    };
  }

  std::string human_output(const atmos_pcm_probe::cli_options &options,
                           const probe_observation &observation) {
    std::ostringstream output;
    output << (observation.probe_complete && observation.errors.empty() ? "OBSERVED" : "INCOMPLETE")
           << "\n";
    if (options.endpoint_id) {
      output << "selection=explicit endpoint_id=" << *options.endpoint_id << "\n";
    } else {
      output << "selection=all_active_render_endpoints\n";
    }
    for (const auto &endpoint : observation.endpoints) {
      output << endpoint.id << " | " << endpoint.friendly_name
             << " | state=" << endpoint.state << " ("
             << atmos_pcm_probe::state_name(endpoint.state) << ")\n";
      output << "  exact shared support="
             << optional_hresult_label(endpoint.exact_candidate.format_support_hresult)
             << ", loopback initialize="
             << optional_hresult_label(endpoint.exact_candidate.loopback_initialize_hresult)
             << "\n";
    }
    for (const auto &error : observation.errors) {
      output << "error " << error.operation << " "
             << atmos_pcm_probe::hresult_text(error.hresult) << "\n";
    }
    return output.str();
  }
}  // namespace

namespace atmos_pcm_probe {
  WAVEFORMATEXTENSIBLE make_exact_candidate_format() {
    WAVEFORMATEXTENSIBLE format {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = 12;
    format.Format.nSamplesPerSec = 48000;
    format.Format.nAvgBytesPerSec = 2304000;
    format.Format.nBlockAlign = 48;
    format.Format.wBitsPerSample = 32;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = exact_candidate_channel_mask;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
  }

  bool is_exact_candidate_format(const WAVEFORMATEX *format) {
    if (format == nullptr || format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize != sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
      return false;
    }
    const auto &extensible = *reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
    return extensible.Format.nChannels == 12 &&
           extensible.Format.nSamplesPerSec == 48000 &&
           extensible.Format.nAvgBytesPerSec == 2304000 &&
           extensible.Format.nBlockAlign == 48 &&
           extensible.Format.wBitsPerSample == 32 &&
           extensible.Samples.wValidBitsPerSample == 32 &&
           extensible.dwChannelMask == exact_candidate_channel_mask &&
           IsEqualGUID(extensible.SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }

  cli_parse_result parse_cli(const std::span<const std::string_view> arguments) {
    cli_options options {};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto argument = arguments[index];
      if (argument == "--json") {
        options.json = true;
        continue;
      }
      if (argument == "--help") {
        options.help = true;
        continue;
      }
      if (argument == "--endpoint-id") {
        if (options.endpoint_id) {
          return {.error = "--endpoint-id may be specified once"};
        }
        if (index + 1 == arguments.size()) {
          return {.error = "--endpoint-id requires a value"};
        }
        options.endpoint_id = std::string {arguments[++index]};
        if (options.endpoint_id->empty()) {
          return {.error = "--endpoint-id requires a non-empty value"};
        }
        continue;
      }
      return {.error = "unknown option: " + std::string {argument}};
    }
    return {.options = std::move(options)};
  }

  std::string help_text() {
    return
      "Usage: atmos-pcm-spatial-probe [--json] [--endpoint-id <opaque MMDevice id>]\n"
      "Enumerates active render endpoints and performs no-write 7.1.4 PCM probes.\n"
      "The exact candidate is float32, 48 kHz, 12-channel WAVEFORMATEXTENSIBLE.\n"
      "The probe never starts a client, obtains a render buffer, or writes audio.\n";
  }

  std::string serialize_report(
    const cli_options &options,
    const probe_observation &observation) {
    ordered_json endpoints = ordered_json::array();
    for (const auto &endpoint : observation.endpoints) {
      endpoints.push_back(endpoint_json(endpoint));
    }
    ordered_json errors = ordered_json::array();
    for (const auto &error : observation.errors) {
      errors.push_back(error_json(error));
    }
    ordered_json requested_endpoint_id = nullptr;
    if (options.endpoint_id) {
      requested_endpoint_id = *options.endpoint_id;
    }
    ordered_json report {
      {"schema_version", 1},
      {"audio_bytes_written", false},
      {"probe_complete", observation.probe_complete && observation.errors.empty()},
      {"selection", {
        {"kind", options.endpoint_id ? "explicit" : "all_active_render"},
        {"requested_endpoint_id", std::move(requested_endpoint_id)},
      }},
      {"endpoints", std::move(endpoints)},
      {"probe_errors", std::move(errors)},
    };
    return report.dump();
  }

  app_result run_probe(
    const std::span<const std::string_view> arguments,
    const observation_provider &provider) {
    const auto parsed = parse_cli(arguments);
    if (!parsed.options) {
      return {.exit_code = 2, .standard_error = parsed.error + "\n"};
    }
    if (parsed.options->help) {
      return {.exit_code = 0, .standard_output = help_text()};
    }

    probe_options probe_options_value {.endpoint_id = parsed.options->endpoint_id};
    probe_observation observation {};
    try {
      observation = provider(probe_options_value);
    } catch (...) {
      observation.probe_complete = false;
      observation.errors.push_back(api_error {
        .operation = "probe provider",
        .hresult = static_cast<hresult_code>(0x80004005L),
      });
    }

    const int exit_code = observation.probe_complete && observation.errors.empty() ? 0 : 3;
    if (parsed.options->json) {
      return {
        .exit_code = exit_code,
        .standard_output = serialize_report(*parsed.options, observation) + "\n",
      };
    }
    return {
      .exit_code = exit_code,
      .standard_output = human_output(*parsed.options, observation),
    };
  }

  std::string state_name(const std::uint32_t state) {
    switch (state) {
      case DEVICE_STATE_ACTIVE:
        return "ACTIVE";
      case DEVICE_STATE_DISABLED:
        return "DISABLED";
      case DEVICE_STATE_NOTPRESENT:
        return "NOT_PRESENT";
      case DEVICE_STATE_UNPLUGGED:
        return "UNPLUGGED";
      default:
        return "UNKNOWN";
    }
  }

  std::string hresult_text(const hresult_code value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(8)
           << hresult_bits(value);
    return stream.str();
  }

  std::string hresult_label(const hresult_code value) {
    switch (hresult_bits(value)) {
      case 0x00000000U: return "S_OK";
      case 0x00000001U: return "S_FALSE";
      case 0x80004002U: return "E_NOINTERFACE";
      case 0x80004003U: return "E_POINTER";
      case 0x80004005U: return "E_FAIL";
      case 0x8007000EU: return "E_OUTOFMEMORY";
      case 0x80070057U: return "E_INVALIDARG";
      case 0x80070490U: return "E_NOTFOUND";
      case 0x88890004U: return "AUDCLNT_E_DEVICE_INVALIDATED";
      case 0x88890008U: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
      case 0x8889000AU: return "AUDCLNT_E_DEVICE_IN_USE";
      case 0x8889000EU: return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
      default: return "UNKNOWN_HRESULT";
    }
  }
}  // namespace atmos_pcm_probe

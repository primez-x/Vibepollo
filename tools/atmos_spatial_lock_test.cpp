#include "tools/atmos_spatial_lock_test.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>

namespace {
  using atmos_spatial_lock::diagnostic;
  using atmos_spatial_lock::route_observation;

  std::optional<double> parse_finite_number(const std::string_view raw) {
    if (raw.empty()) {
      return std::nullopt;
    }
    const std::string text {raw};
    char *end {};
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end != text.c_str() + text.size() ||
        !std::isfinite(value)) {
      return std::nullopt;
    }
    return value;
  }

  bool route_ready(
    const route_observation &route,
    std::vector<diagnostic> *diagnostics) {
    bool ready = true;
    const auto append = [&](const diagnostic value) {
      ready = false;
      if (diagnostics != nullptr) {
        diagnostics->push_back(value);
      }
    };

    if (!route.sample_complete) {
      append(diagnostic::route_observation_failed);
    }
    if (!route.endpoint.present || route.endpoint.id.empty()) {
      append(diagnostic::endpoint_not_found);
      return false;
    }
    if (route.endpoint.state != 1) {
      append(diagnostic::endpoint_not_active);
    }
    if (!route.endpoint.display_audio) {
      append(diagnostic::endpoint_not_display_audio);
    }
    if (!route.endpoint.hdmi) {
      append(diagnostic::endpoint_not_hdmi);
    }

    if (!std::ranges::all_of(
          route.core_default_ids,
          [&](const std::string &id) {
            return !id.empty() && id == route.endpoint.id;
          })) {
      append(diagnostic::core_defaults_not_exact);
    }
    if (route.winrt_default_id.empty() ||
        route.winrt_communications_id.empty() ||
        route.winrt_default_id != route.winrt_communications_id ||
        route.winrt_default_id == route.endpoint.id) {
      append(diagnostic::winrt_defaults_not_exact);
    }
    if (!route.spatial_configuration_available) {
      append(diagnostic::spatial_configuration_unavailable);
    } else if (route.spatial_configuration_id.empty() ||
               route.spatial_configuration_id != route.winrt_default_id ||
               route.spatial_configuration_id == route.endpoint.id) {
      append(diagnostic::spatial_configuration_id_mismatch);
    }
    if (!route.spatial_audio_supported) {
      append(diagnostic::spatial_audio_unsupported);
    }
    if (!route.atmos_home_theater_supported) {
      append(diagnostic::atmos_home_theater_unsupported);
    }
    if (route.active_spatial_format !=
        atmos_spatial_lock::atmos_home_theater_format) {
      append(diagnostic::active_spatial_format_not_atmos);
    }
    return ready;
  }

  nlohmann::json endpoint_json(
    const atmos_spatial_lock::endpoint_observation &endpoint) {
    return {
      {"present", endpoint.present},
      {"id", endpoint.id},
      {"friendly_name", endpoint.friendly_name},
      {"state", endpoint.state},
      {"display_audio", endpoint.display_audio},
      {"hdmi", endpoint.hdmi},
    };
  }

  nlohmann::json format_json(
    const atmos_spatial_lock::object_format &format) {
    return {
      {"format_tag", format.format_tag},
      {"channels", format.channels},
      {"samples_per_second", format.samples_per_second},
      {"average_bytes_per_second", format.average_bytes_per_second},
      {"block_align", format.block_align},
      {"bits_per_sample", format.bits_per_sample},
      {"extra_size", format.extra_size},
      {"float_pcm", format.float_pcm},
      {"subformat", format.subformat},
    };
  }

  nlohmann::json route_json(const route_observation &route) {
    return {
      {"sample_complete", route.sample_complete},
      {"endpoint", endpoint_json(route.endpoint)},
      {"core_default_ids", route.core_default_ids},
      {"winrt_default_id", route.winrt_default_id},
      {"winrt_communications_id", route.winrt_communications_id},
      {"spatial_configuration_available", route.spatial_configuration_available},
      {"spatial_configuration_id", route.spatial_configuration_id},
      {"spatial_audio_supported", route.spatial_audio_supported},
      {"atmos_home_theater_supported", route.atmos_home_theater_supported},
      {"active_spatial_format", route.active_spatial_format},
    };
  }

  template<class T>
  nlohmann::json optional_json(const std::optional<T> &value) {
    if (value) {
      return *value;
    }
    return nullptr;
  }

  nlohmann::json capabilities_json(
    const atmos_spatial_lock::object_capabilities &capabilities) {
    nlohmann::json result {
      {"spatial_client_hresult", optional_json(capabilities.spatial_client_hresult)},
      {"stream_available_hresult", optional_json(capabilities.stream_available_hresult)},
      {"native_static_object_mask", optional_json(capabilities.native_static_object_mask)},
      {"required_static_object_mask", atmos_spatial_lock::required_static_object_mask},
      {"max_dynamic_object_count", optional_json(capabilities.max_dynamic_object_count)},
      {"required_dynamic_object_count", atmos_spatial_lock::required_dynamic_object_count},
      {"supported_format_count", optional_json(capabilities.supported_format_count)},
      {"format_support_hresult", optional_json(capabilities.format_support_hresult)},
      {"max_frame_count", optional_json(capabilities.max_frame_count)},
    };
    result["negotiated_format"] = capabilities.negotiated_format ?
      format_json(*capabilities.negotiated_format) : nlohmann::json {nullptr};
    return result;
  }
}  // namespace

namespace atmos_spatial_lock {
  bool detail::object_update_lifecycle::begin_update() noexcept {
    if (phase_ != phase::closed) {
      return false;
    }
    phase_ = phase::metadata;
    return true;
  }

  bool detail::object_update_lifecycle::accept(
    const object_update_action action) noexcept {
    switch (action) {
      case object_update_action::set_position:
      case object_update_action::set_volume:
        return phase_ == phase::metadata;
      case object_update_action::get_buffer:
        if (phase_ != phase::metadata && phase_ != phase::buffers) {
          return false;
        }
        phase_ = phase::buffers;
        return true;
      case object_update_action::set_end_of_stream:
        return phase_ == phase::buffers;
    }
    return false;
  }

  bool detail::object_update_lifecycle::fail_update() noexcept {
    if (phase_ == phase::closed) {
      return false;
    }
    phase_ = phase::failed;
    return true;
  }

  bool detail::object_update_lifecycle::end_update() noexcept {
    if (phase_ == phase::closed) {
      return false;
    }
    phase_ = phase::closed;
    return true;
  }

#ifdef _WIN32
  WAVEFORMATEXTENSIBLE_IEC61937 make_mat10_format() {
    WAVEFORMATEXTENSIBLE_IEC61937 format {};
    format.FormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.FormatExt.Format.nChannels = 8;
    format.FormatExt.Format.nSamplesPerSec = 192000;
    format.FormatExt.Format.nAvgBytesPerSec = 3072000;
    format.FormatExt.Format.nBlockAlign = 16;
    format.FormatExt.Format.wBitsPerSample = 16;
    format.FormatExt.Format.cbSize = 34;
    format.FormatExt.Samples.wValidBitsPerSample = 16;
    format.FormatExt.dwChannelMask = KSAUDIO_SPEAKER_7POINT1;
    format.FormatExt.SubFormat = mat10_mlp_subformat;
    format.dwEncodedSamplesPerSec = 96000;
    format.dwEncodedChannelCount = 8;
    format.dwAverageBytesPerSec = 0;
    return format;
  }
#endif

  parse_result parse_options(const std::span<const std::string_view> arguments) {
    options parsed;
    bool seen_json {};
    bool seen_duration {};
    bool seen_frequency {};
    bool seen_amplitude {};

    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const std::string_view argument = arguments[index];
      if (argument == "--json") {
        if (seen_json) {
          return {.error = "duplicate option: --json"};
        }
        seen_json = true;
        parsed.json = true;
        continue;
      }

      const auto equals = argument.find('=');
      const std::string_view name = argument.substr(0, equals);
      std::string_view raw_value;
      if (equals != std::string_view::npos) {
        raw_value = argument.substr(equals + 1);
      } else {
        if (index + 1 >= arguments.size()) {
          return {.error = "missing value for option: " + std::string {name}};
        }
        raw_value = arguments[++index];
      }

      bool *seen {};
      if (name == "--duration-ms") {
        seen = &seen_duration;
      } else if (name == "--frequency-hz") {
        seen = &seen_frequency;
      } else if (name == "--amplitude") {
        seen = &seen_amplitude;
      } else {
        return {.error = "unknown option: " + std::string {name}};
      }
      if (*seen) {
        return {.error = "duplicate option: " + std::string {name}};
      }
      *seen = true;

      const auto numeric = parse_finite_number(raw_value);
      if (!numeric) {
        return {.error = "invalid numeric value for option: " + std::string {name}};
      }
      if (name == "--duration-ms") {
        const double bounded = std::clamp(
          *numeric,
          static_cast<double>(minimum_duration_ms),
          static_cast<double>(maximum_duration_ms));
        parsed.duration_ms = static_cast<std::uint32_t>(std::llround(bounded));
      } else if (name == "--frequency-hz") {
        parsed.frequency_hz = std::clamp(
          *numeric,
          minimum_frequency_hz,
          maximum_frequency_hz);
      } else {
        parsed.amplitude = static_cast<float>(std::clamp(
          *numeric,
          static_cast<double>(minimum_amplitude),
          static_cast<double>(maximum_amplitude)));
      }
    }
    return {.value = parsed};
  }

  bool routes_stable(
    const route_observation &before,
    const route_observation &after) {
    return route_ready(before, nullptr) &&
           route_ready(after, nullptr) &&
           before.endpoint.id == after.endpoint.id &&
           before.endpoint.state == after.endpoint.state &&
           before.endpoint.display_audio == after.endpoint.display_audio &&
           before.endpoint.hdmi == after.endpoint.hdmi &&
           before.core_default_ids == after.core_default_ids &&
           before.winrt_default_id == after.winrt_default_id &&
           before.winrt_communications_id == after.winrt_communications_id &&
           before.spatial_configuration_id == after.spatial_configuration_id &&
           before.active_spatial_format == after.active_spatial_format;
  }

  gate_result evaluate_preflight(const preflight_observation &observation) {
    gate_result result;
    route_ready(observation.initial_route, &result.diagnostics);
    if (!routes_stable(observation.initial_route, observation.pre_start_route)) {
      result.diagnostics.push_back(diagnostic::route_not_stable);
    }

    if (!observation.mat10.format_support_hresult) {
      result.diagnostics.push_back(diagnostic::mat10_format_probe_failed);
    } else if (*observation.mat10.format_support_hresult != 0) {
      result.diagnostics.push_back(diagnostic::mat10_format_unsupported);
    }
    if (!observation.mat10.initialize_hresult) {
      result.diagnostics.push_back(diagnostic::mat10_initialize_probe_failed);
    } else if (*observation.mat10.initialize_hresult != 0) {
      result.diagnostics.push_back(diagnostic::mat10_initialize_failed);
    }

    const auto &objects = observation.objects;
    if (!objects.spatial_client_hresult || *objects.spatial_client_hresult != 0) {
      result.diagnostics.push_back(diagnostic::spatial_client_unavailable);
    }
    if (!objects.stream_available_hresult || *objects.stream_available_hresult != 0) {
      result.diagnostics.push_back(diagnostic::spatial_stream_unavailable);
    }
    if (!objects.native_static_object_mask ||
        (*objects.native_static_object_mask & required_static_object_mask) !=
          required_static_object_mask ||
        (*objects.native_static_object_mask & dynamic_object_type_mask) != 0) {
      result.diagnostics.push_back(diagnostic::static_bed_unsupported);
    }
    if (!objects.max_dynamic_object_count ||
        *objects.max_dynamic_object_count != required_dynamic_object_count) {
      result.diagnostics.push_back(diagnostic::dynamic_object_count_not_20);
    }
    if (!objects.supported_format_count || *objects.supported_format_count == 0) {
      result.diagnostics.push_back(diagnostic::object_capabilities_unavailable);
    }
    const bool canonical_float_format = objects.negotiated_format &&
      ((objects.negotiated_format->format_tag == 3 &&
        objects.negotiated_format->extra_size == 0) ||
       (objects.negotiated_format->format_tag == 0xfffe &&
        objects.negotiated_format->extra_size == 22));
    if (!canonical_float_format || !objects.negotiated_format->float_pcm ||
        objects.negotiated_format->channels != 1 ||
        objects.negotiated_format->bits_per_sample != 32 ||
        objects.negotiated_format->block_align != sizeof(float) ||
        objects.negotiated_format->samples_per_second == 0) {
      result.diagnostics.push_back(diagnostic::float_object_format_unavailable);
    }
    if (!objects.format_support_hresult || *objects.format_support_hresult != 0) {
      result.diagnostics.push_back(diagnostic::object_format_unsupported);
    }
    if (!objects.max_frame_count || *objects.max_frame_count == 0) {
      result.diagnostics.push_back(diagnostic::max_frame_count_unavailable);
    }

    result.ready = result.diagnostics.empty();
    return result;
  }

  float fill_scene_object(
    const std::span<float> samples,
    const std::uint32_t samples_per_second,
    const double frequency_hz,
    const float scene_amplitude,
    const std::uint64_t first_frame,
    const std::uint32_t object_index,
    const std::uint32_t object_count) {
    std::ranges::fill(samples, 0.0f);
    if (samples.empty() || samples_per_second == 0 || object_count == 0 ||
        !std::isfinite(frequency_hz) || !std::isfinite(scene_amplitude)) {
      return 0.0f;
    }

    const double bounded_frequency = std::clamp(
      frequency_hz,
      minimum_frequency_hz,
      maximum_frequency_hz);
    const float bounded_amplitude = std::clamp(
      std::abs(scene_amplitude),
      0.0f,
      maximum_amplitude);
    const double gain = static_cast<double>(bounded_amplitude) /
                        std::sqrt(static_cast<double>(object_count));
    const double phase_offset =
      2.0 * std::numbers::pi * static_cast<double>(object_index) /
      static_cast<double>(object_count);
    float peak {};
    for (std::size_t index = 0; index < samples.size(); ++index) {
      const double frame = static_cast<double>(first_frame + index);
      const double phase =
        2.0 * std::numbers::pi * bounded_frequency * frame /
          static_cast<double>(samples_per_second) +
        phase_offset;
      samples[index] = static_cast<float>(gain * std::sin(phase));
      peak = std::max(peak, std::abs(samples[index]));
    }
    return peak;
  }

  std::optional<std::uint32_t> checked_float_buffer_bytes(
    const std::uint32_t frame_count,
    const std::uint32_t frames_to_write) {
    constexpr auto max_uint32 = std::numeric_limits<std::uint32_t>::max();
    constexpr auto max_frame_count = max_uint32 / sizeof(float);
    if (frame_count > max_frame_count) {
      return std::nullopt;
    }
    if (frames_to_write > frame_count) {
      return std::nullopt;
    }

    const auto wide_bytes = static_cast<std::uint64_t>(frame_count) *
                            static_cast<std::uint64_t>(sizeof(float));
    if (wide_bytes > max_uint32) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(wide_bytes);
  }

  std::string_view to_string(const diagnostic value) {
    switch (value) {
      case diagnostic::route_observation_failed: return "ROUTE_OBSERVATION_FAILED";
      case diagnostic::endpoint_not_found: return "ENDPOINT_NOT_FOUND";
      case diagnostic::endpoint_not_active: return "ENDPOINT_NOT_ACTIVE";
      case diagnostic::endpoint_not_display_audio: return "ENDPOINT_NOT_DISPLAY_AUDIO";
      case diagnostic::endpoint_not_hdmi: return "ENDPOINT_NOT_HDMI";
      case diagnostic::core_defaults_not_exact: return "CORE_DEFAULTS_NOT_EXACT";
      case diagnostic::winrt_defaults_not_exact: return "WINRT_DEFAULTS_NOT_EXACT";
      case diagnostic::spatial_configuration_unavailable: return "SPATIAL_CONFIGURATION_UNAVAILABLE";
      case diagnostic::spatial_configuration_id_mismatch: return "SPATIAL_CONFIGURATION_ID_MISMATCH";
      case diagnostic::spatial_audio_unsupported: return "SPATIAL_AUDIO_UNSUPPORTED";
      case diagnostic::atmos_home_theater_unsupported: return "ATMOS_HOME_THEATER_UNSUPPORTED";
      case diagnostic::active_spatial_format_not_atmos: return "ACTIVE_SPATIAL_FORMAT_NOT_ATMOS";
      case diagnostic::route_not_stable: return "ROUTE_NOT_STABLE";
      case diagnostic::mat10_format_probe_failed: return "MAT10_FORMAT_PROBE_FAILED";
      case diagnostic::mat10_format_unsupported: return "MAT10_FORMAT_UNSUPPORTED";
      case diagnostic::mat10_initialize_probe_failed: return "MAT10_INITIALIZE_PROBE_FAILED";
      case diagnostic::mat10_initialize_failed: return "MAT10_INITIALIZE_FAILED";
      case diagnostic::spatial_client_unavailable: return "SPATIAL_CLIENT_UNAVAILABLE";
      case diagnostic::spatial_stream_unavailable: return "SPATIAL_STREAM_UNAVAILABLE";
      case diagnostic::object_capabilities_unavailable: return "OBJECT_CAPABILITIES_UNAVAILABLE";
      case diagnostic::dynamic_object_count_not_20: return "DYNAMIC_OBJECT_COUNT_NOT_20";
      case diagnostic::static_bed_unsupported: return "STATIC_BED_UNSUPPORTED";
      case diagnostic::float_object_format_unavailable: return "FLOAT_OBJECT_FORMAT_UNAVAILABLE";
      case diagnostic::object_format_unsupported: return "OBJECT_FORMAT_UNSUPPORTED";
      case diagnostic::max_frame_count_unavailable: return "MAX_FRAME_COUNT_UNAVAILABLE";
      case diagnostic::stream_activation_failed: return "STREAM_ACTIVATION_FAILED";
      case diagnostic::static_object_activation_failed: return "STATIC_OBJECT_ACTIVATION_FAILED";
      case diagnostic::dynamic_object_activation_failed: return "DYNAMIC_OBJECT_ACTIVATION_FAILED";
      case diagnostic::stream_start_failed: return "STREAM_START_FAILED";
      case diagnostic::render_update_failed: return "RENDER_UPDATE_FAILED";
      case diagnostic::stream_stop_failed: return "STREAM_STOP_FAILED";
      case diagnostic::final_route_observation_failed: return "FINAL_ROUTE_OBSERVATION_FAILED";
    }
    return "UNKNOWN";
  }

  std::string hresult_label(const hresult_code value) {
    switch (static_cast<std::uint32_t>(value)) {
      case 0x00000000u: return "S_OK";
      case 0x00000001u: return "S_FALSE";
      case 0x80004002u: return "E_NOINTERFACE";
      case 0x80004003u: return "E_POINTER";
      case 0x80004005u: return "E_FAIL";
      case 0x8007000eu: return "E_OUTOFMEMORY";
      case 0x80070057u: return "E_INVALIDARG";
      case 0x80070490u: return "E_NOTFOUND";
      case 0x88890008u: return "AUDCLNT_E_UNSUPPORTED_FORMAT";
      case 0x8889000au: return "AUDCLNT_E_DEVICE_IN_USE";
      case 0x88890101u: return "SPTLAUDCLNT_E_OUT_OF_ORDER";
      case 0x88890103u: return "SPTLAUDCLNT_E_NO_MORE_OBJECTS";
      case 0x88890107u: return "SPTLAUDCLNT_E_STREAM_NOT_AVAILABLE";
      case 0x8889010du: return "SPTLAUDCLNT_E_INTERNAL";
      default: {
        std::ostringstream label;
        label << "HRESULT_0x" << std::uppercase << std::hex <<
          std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(value);
        return label.str();
      }
    }
  }

  std::string help_text() {
    std::ostringstream text;
    text <<
      "Usage: atmos-spatial-lock-test [--json] [--duration-ms N] "
      "[--frequency-hz N] [--amplitude N]\n\n"
      "Submits a bounded, quiet float-PCM 7.1.4-plus-object scene.\n"
      "Duration evidence distinguishes submitted frames from host-observed wall time.\n"
      "The tool does not observe render drain or downstream playback completion.\n"
      "Proof scope: " << proof_scope << ".\n"
      "A ready result means only that " << ready_meaning << ".\n"
      "This does not prove:\n";
    for (const auto limitation : proof_limitations) {
      text << "  - " << limitation << '\n';
    }
    return text.str();
  }

  std::string serialize_report(const run_report &report) {
    nlohmann::json hresults = nlohmann::json::array();
    for (const auto &result : report.hresults) {
      std::ostringstream hex;
      hex << "0x" << std::uppercase << std::hex << std::setw(8) <<
        std::setfill('0') << static_cast<std::uint32_t>(result.hresult);
      hresults.push_back({
        {"operation", result.operation},
        {"signed", result.hresult},
        {"hex", hex.str()},
        {"label", hresult_label(result.hresult)},
      });
    }

    nlohmann::json diagnostics = nlohmann::json::array();
    for (const auto value : report.diagnostics) {
      diagnostics.push_back(to_string(value));
    }
    nlohmann::json limitations = nlohmann::json::array();
    for (const auto limitation : proof_limitations) {
      limitations.push_back(limitation);
    }

    nlohmann::json result {
      {"schema_version", 1},
      {"tool", "atmos-spatial-lock-test"},
      {"proof_scope", proof_scope},
      {"ready_meaning", ready_meaning},
      {"physical_receiver_lock_requires_external_observation", true},
      {"does_not_prove", std::move(limitations)},
      {"ready", report.ready},
      {"endpoint", endpoint_json(report.endpoint)},
      {"object_capabilities", capabilities_json(report.object_capabilities)},
      {"route_bookends", {
        {"initial", route_json(report.initial_route)},
        {"pre_start", route_json(report.pre_start_route)},
        {"final", route_json(report.final_route)},
      }},
      {"update_count", report.update_count},
      {"frame_count", report.frame_count},
      {"requested_duration_ms", report.requested_duration_ms},
      {"submitted_duration_ms", report.submitted_duration_ms},
      {"wall_elapsed_ms", report.wall_elapsed_ms},
      {"drain_completion_observed", false},
      {"peak_sample", report.peak_sample},
      {"hresults", std::move(hresults)},
      {"diagnostics", std::move(diagnostics)},
      {"started", report.started},
      {"stopped", report.stopped},
      {"route_stable", report.route_stable},
      {"audio_generated", report.audio_generated},
      {"dynamic_object_activated", report.dynamic_object_activated},
    };
    result["negotiated_object_format"] = report.negotiated_format ?
      format_json(*report.negotiated_format) : nlohmann::json {nullptr};
    return result.dump(2) + '\n';
  }
}  // namespace atmos_spatial_lock

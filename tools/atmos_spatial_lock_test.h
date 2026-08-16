#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <ksmedia.h>
#endif

namespace atmos_spatial_lock {
  using hresult_code = std::int32_t;

  namespace detail {
    using cleanup_function = void (*)(void *) noexcept;

    enum class object_update_action {
      set_position,
      set_volume,
      get_buffer,
      set_end_of_stream,
    };

    class object_update_lifecycle {
    public:
      bool begin_update() noexcept;
      bool accept(object_update_action action) noexcept;
      bool fail_update() noexcept;
      bool end_update() noexcept;

    private:
      enum class phase {
        closed,
        metadata,
        buffers,
        failed,
      };

      phase phase_ {phase::closed};
    };

    class noexcept_cleanup_guard {
    public:
      noexcept_cleanup_guard(
        void *context,
        const cleanup_function cleanup) noexcept:
          context_(context),
          cleanup_(cleanup) {}

      noexcept_cleanup_guard(const noexcept_cleanup_guard &) = delete;
      noexcept_cleanup_guard &operator=(const noexcept_cleanup_guard &) = delete;

      ~noexcept_cleanup_guard() noexcept {
        invoke();
      }

      void arm() noexcept {
        armed_ = true;
      }

      void invoke() noexcept {
        if (!armed_) {
          return;
        }
        armed_ = false;
        if (cleanup_ != nullptr) {
          cleanup_(context_);
        }
      }

    private:
      void *context_ {};
      cleanup_function cleanup_ {};
      bool armed_ {};
    };
  }  // namespace detail

  inline constexpr std::uint32_t minimum_duration_ms = 250;
  inline constexpr std::uint32_t maximum_duration_ms = 30000;
  inline constexpr double minimum_frequency_hz = 20.0;
  inline constexpr double maximum_frequency_hz = 2000.0;
  inline constexpr float minimum_amplitude = 0.0001f;
  inline constexpr float maximum_amplitude = 0.05f;
  inline constexpr std::uint32_t required_dynamic_object_count = 20;
  inline constexpr std::uint32_t required_static_object_mask = 0x1ffe;
  // AudioObjectType_Dynamic is a stream activation flag, not a static-bed
  // capability. It must never be accepted in the native static mask.
  inline constexpr std::uint32_t dynamic_object_type_mask = 0x1;
  inline constexpr std::string_view atmos_home_theater_format =
    "{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
  inline constexpr std::string_view proof_scope =
    "machine-side Windows spatial generated-scene API submission evidence for the active HDMI/eARC route";
  inline constexpr std::string_view ready_meaning =
    "the exact machine-side API and route contract completed successfully";
  inline constexpr std::array<std::string_view, 6> proof_limitations {
    "physical receiver lock state without external observation",
    "native-content provenance",
    "upmixed versus non-spatial content discrimination",
    "bit-exact relay",
    "end-to-end host-to-client relay integrity",
    "render drain or downstream playback completion",
  };

  struct options {
    bool json {true};
    std::uint32_t duration_ms {8000};
    double frequency_hz {440.0};
    float amplitude {0.02f};
  };

  struct parse_result {
    std::optional<options> value;
    std::string error;
  };

  enum class diagnostic {
    route_observation_failed,
    endpoint_not_found,
    endpoint_not_active,
    endpoint_not_display_audio,
    endpoint_not_hdmi,
    core_defaults_not_exact,
    winrt_defaults_not_exact,
    spatial_configuration_unavailable,
    spatial_configuration_id_mismatch,
    spatial_audio_unsupported,
    atmos_home_theater_unsupported,
    active_spatial_format_not_atmos,
    route_not_stable,
    mat10_format_probe_failed,
    mat10_format_unsupported,
    mat10_initialize_probe_failed,
    mat10_initialize_failed,
    spatial_client_unavailable,
    spatial_stream_unavailable,
    object_capabilities_unavailable,
    dynamic_object_count_not_20,
    static_bed_unsupported,
    float_object_format_unavailable,
    object_format_unsupported,
    max_frame_count_unavailable,
    stream_activation_failed,
    static_object_activation_failed,
    dynamic_object_activation_failed,
    stream_start_failed,
    render_update_failed,
    stream_stop_failed,
    final_route_observation_failed,
  };

  struct endpoint_observation {
    bool present {};
    std::string id;
    std::string friendly_name;
    std::uint32_t state {};
    bool display_audio {};
    bool hdmi {};
  };

  struct route_observation {
    bool sample_complete {};
    endpoint_observation endpoint;
    // Fixed order: console, multimedia, communications.
    std::array<std::string, 3> core_default_ids;
    std::string winrt_default_id;
    std::string winrt_communications_id;
    bool spatial_configuration_available {};
    std::string spatial_configuration_id;
    bool spatial_audio_supported {};
    bool atmos_home_theater_supported {};
    std::string active_spatial_format;
  };

  struct mat10_observation {
    std::optional<hresult_code> format_support_hresult;
    std::optional<hresult_code> initialize_hresult;
  };

  struct object_format {
    std::uint16_t format_tag {};
    std::uint16_t channels {};
    std::uint32_t samples_per_second {};
    std::uint32_t average_bytes_per_second {};
    std::uint16_t block_align {};
    std::uint16_t bits_per_sample {};
    std::uint16_t extra_size {};
    bool float_pcm {};
    std::string subformat;
  };

  struct object_capabilities {
    std::optional<hresult_code> spatial_client_hresult;
    std::optional<hresult_code> stream_available_hresult;
    std::optional<std::uint32_t> native_static_object_mask;
    std::optional<std::uint32_t> max_dynamic_object_count;
    std::optional<std::uint32_t> supported_format_count;
    std::optional<object_format> negotiated_format;
    std::optional<hresult_code> format_support_hresult;
    std::optional<std::uint32_t> max_frame_count;
  };

  struct preflight_observation {
    route_observation initial_route;
    route_observation pre_start_route;
    mat10_observation mat10;
    object_capabilities objects;
  };

  struct gate_result {
    bool ready {};
    std::vector<diagnostic> diagnostics;
  };

  struct api_result {
    std::string operation;
    hresult_code hresult {};
  };

  struct run_report {
    bool ready {};
    endpoint_observation endpoint;
    std::optional<object_format> negotiated_format;
    object_capabilities object_capabilities;
    route_observation initial_route;
    route_observation pre_start_route;
    route_observation final_route;
    std::uint64_t update_count {};
    std::uint64_t frame_count {};
    std::uint32_t requested_duration_ms {};
    double submitted_duration_ms {};
    double wall_elapsed_ms {};
    float peak_sample {};
    std::vector<api_result> hresults;
    std::vector<diagnostic> diagnostics;
    bool started {};
    bool stopped {};
    bool route_stable {};
    bool audio_generated {};
    bool dynamic_object_activated {};
  };

  parse_result parse_options(std::span<const std::string_view> arguments);
  gate_result evaluate_preflight(const preflight_observation &observation);
  bool routes_stable(
    const route_observation &before,
    const route_observation &after);
  float fill_scene_object(
    std::span<float> samples,
    std::uint32_t samples_per_second,
    double frequency_hz,
    float scene_amplitude,
    std::uint64_t first_frame,
    std::uint32_t object_index,
    std::uint32_t object_count);
  std::optional<std::uint32_t> checked_float_buffer_bytes(
    std::uint32_t frame_count,
    std::uint32_t frames_to_write);
  std::string_view to_string(diagnostic value);
  std::string hresult_label(hresult_code value);
  std::string help_text();
  std::string serialize_report(const run_report &report);
  inline constexpr int failure_exit_code(const bool started) noexcept {
    return started ? 3 : 2;
  }

#ifdef _WIN32
  inline constexpr GUID mat10_mlp_subformat {
    0x0000000c, 0x0cea, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}
  };

  WAVEFORMATEXTENSIBLE_IEC61937 make_mat10_format();
  int run_windows(const options &parsed_options);
#endif
}  // namespace atmos_spatial_lock

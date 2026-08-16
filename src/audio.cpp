/**
 * @file src/audio.cpp
 * @brief Definitions for audio capture and encoding.
 */
#include <algorithm>
#include <thread>

// lib includes
#include <opus/opus_multistream.h>

// local includes
#include "audio.h"
#include "audio_policy.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_safe.h"
#include "tools/atmos_mat_host_relay.h"
#include "utility.h"
#include "webrtc_stream.h"

namespace audio {
  using namespace std::literals;
  using opus_t = util::safe_ptr<OpusMSEncoder, opus_multistream_encoder_destroy>;
  using sample_queue_t = std::shared_ptr<safe::queue_t<std::vector<float>>>;

  static int start_audio_control(audio_ctx_t &ctx);
  static void stop_audio_control(audio_ctx_t &);
  static void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params);
  static void capture_mat10(safe::mail_t mail, const config_t &config, void *channel_data);

  int map_stream(int channels, bool quality);

  constexpr auto SAMPLE_RATE = 48000;

  // NOTE: If you adjust the bitrates listed here, make sure to update the
  // corresponding bitrate adjustment logic in rtsp_stream::cmd_announce()
  opus_stream_config_t stream_configs[MAX_STREAM_CONFIG] {
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      96000,
    },
    {
      SAMPLE_RATE,
      2,
      1,
      1,
      platf::speaker::map_stereo,
      512000,
    },
    {
      SAMPLE_RATE,
      6,
      4,
      2,
      platf::speaker::map_surround51,
      256000,
    },
    {
      SAMPLE_RATE,
      6,
      6,
      0,
      platf::speaker::map_surround51,
      1536000,
    },
    {
      SAMPLE_RATE,
      8,
      5,
      3,
      platf::speaker::map_surround71,
      450000,
    },
    {
      SAMPLE_RATE,
      8,
      8,
      0,
      platf::speaker::map_surround71,
      2048000,
    },
  };

  void encodeThread(sample_queue_t samples, config_t config, void *channel_data) {
    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    // Encoding takes place on this thread
    platf::set_thread_name("audio::encode");
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    opus_t opus {opus_multistream_encoder_create(
      stream.sampleRate,
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      stream.mapping,
      OPUS_APPLICATION_RESTRICTED_LOWDELAY,
      nullptr
    )};

    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_BITRATE(stream.bitrate));
    opus_multistream_encoder_ctl(opus.get(), OPUS_SET_VBR(0));

    BOOST_LOG(info) << "Opus initialized: "sv << stream.sampleRate / 1000 << " kHz, "sv
                    << stream.channelCount << " channels, "sv
                    << stream.bitrate / 1000 << " kbps (total), LOWDELAY"sv;

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    while (auto sample = samples->pop()) {
      buffer_t packet {1400};

      if (webrtc_stream::has_active_sessions() && channel_data == nullptr) {
        webrtc_stream::submit_audio_frame(*sample, stream.sampleRate, stream.channelCount, frame_size);
      }

      int bytes = opus_multistream_encode_float(opus.get(), sample->data(), frame_size, std::begin(packet), (opus_int32) packet.size());
      if (bytes < 0) {
        BOOST_LOG(error) << "Couldn't encode audio: "sv << opus_strerror(bytes);
        packets->stop();

        return;
      }

      packet.fake_resize(bytes);
      packets->raise(channel_data, std::move(packet));
    }
  }

  void capture_mat10(safe::mail_t mail, const config_t &config, void *channel_data) {
#ifdef _WIN32
    if (channel_data == nullptr) {
      BOOST_LOG(error) << "MAT10 cannot be routed through the WebRTC audio path"sv;
      return;
    }

    atmos_mat_host::descriptor_identity descriptor {};
    descriptor.bytes = config.mat10_descriptor;
    atmos_mat_host::relay relay {descriptor, 30};
    auto device = atmos_mat_host::make_windows_control_device_reader(nullptr, descriptor);
    if (!device) {
      BOOST_LOG(error) << "MAT10 control device is unavailable; refusing opaque audio"sv;
      return;
    }

    auto packets = mail::man->queue<packet_t>(mail::audio_packets);
    atmos_mat_host::reader reader {relay, *device};
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    std::uint32_t record_sequence {};
    while (!shutdown_event->peek()) {
      const auto terminal = reader.pump_once();
      if (terminal != atmos_mat_host::terminal_reason::none) {
        BOOST_LOG(error) << "MAT10 relay stopped with terminal reason "sv << static_cast<int>(terminal);
        packets->stop();
        return;
      }

      while (auto block = relay.pop()) {
        atmos_mat_direct::record record {
          block->generation,
          block->stream_id,
          block->first_carrier_frame,
          block->host_qpc,
          block->host_qpc_frequency,
          block->descriptor,
          block->flags,
          std::move(block->bytes),
        };
        const auto wire = atmos_mat_direct::encode(record);
        if (!wire) {
          BOOST_LOG(error) << "MAT10 relay produced a malformed record"sv;
          packets->stop();
          return;
        }
        const auto fragments = mat10_fragment_record(record_sequence++, *wire);
        if (fragments.empty()) {
          BOOST_LOG(error) << "MAT10 record exceeded fixed RTP fragment bounds"sv;
          packets->stop();
          return;
        }
        for (const auto &fragment : fragments) {
          buffer_t packet {fragment.size()};
          std::copy(fragment.begin(), fragment.end(), std::begin(packet));
          packet.fake_resize(fragment.size());
          packets->raise(channel_data, std::move(packet));
        }
      }
    }
    reader.cancel();
#else
    (void) mail;
    (void) config;
    (void) channel_data;
    BOOST_LOG(error) << "MAT10 transport is only implemented on Windows"sv;
#endif
  }

  void capture(safe::mail_t mail, config_t config, void *channel_data) {
    auto shutdown_event = mail->event<bool>(mail::shutdown);
    if (!config::audio.stream || config.input_only) {
      shutdown_event->view();
      return;
    }
    if (config.transport == transport_e::mat10) {
      capture_mat10(mail, config, channel_data);
      return;
    }
    auto stream = stream_configs[map_stream(config.channels, config.flags[config_t::HIGH_QUALITY])];
    if (config.flags[config_t::CUSTOM_SURROUND_PARAMS]) {
      apply_surround_params(stream, config.customStreamParams);
    }

    auto ref = get_audio_ctx_ref();
    if (!ref) {
      return;
    }

    auto init_failure_fg = util::fail_guard([&shutdown_event]() {
      BOOST_LOG(error) << "Unable to initialize audio capture. The stream will not have audio."sv;

      // Wait for shutdown to be signalled if we fail init.
      // This allows streaming to continue without audio.
      shutdown_event->view();
      return;
    });

    auto &control = ref->control;
    if (!control) {
      return;
    }

    // Order of priority:
    // 1. Virtual sink
    // 2. Audio sink
    // 3. Host
    policy::sink_catalog_t sinks {ref->sink.host, std::nullopt, std::nullopt, std::nullopt};
    if (ref->sink.null) {
      sinks.stereo = ref->sink.null->stereo;
      sinks.surround51 = ref->sink.null->surround51;
      sinks.surround71 = ref->sink.null->surround71;
    }
    const auto sink = policy::select_sink(
      sinks,
      config::audio.sink,
      stream.channelCount,
      config.flags[config_t::HOST_AUDIO]
    );

    BOOST_LOG(info) << "Selected audio sink: "sv << sink;

    // Only the first to start a session may change the default sink
    if (!ref->sink_flag->exchange(true, std::memory_order_acquire)) {
      // If the selected sink is different than the current one, change sinks.
      ref->restore_sink = ref->sink.host != sink;
      if (ref->restore_sink) {
        if (control->set_sink(sink)) {
          return;
        }
      }
    }

    auto frame_size = config.packetDuration * stream.sampleRate / 1000;
    bool host_audio = config.flags[config_t::HOST_AUDIO];
    bool continuous_audio = config.flags[config_t::CONTINUOUS_AUDIO];
    auto mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
    if (!mic) {
      return;
    }

    // Audio is initialized, so we don't want to print the failure message
    init_failure_fg.disable();

    // Capture takes place on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::critical);

    std::shared_ptr<sample_queue_t::element_type> samples;
    std::thread thread;
    if (!config.bypass_opus) {
      samples = std::make_shared<sample_queue_t::element_type>(30);
      thread = std::thread {encodeThread, samples, config, channel_data};
    }

    auto fg = util::fail_guard([&]() {
      if (samples) {
        samples->stop();
        if (thread.joinable()) {
          thread.join();
        }
      }

      shutdown_event->view();
    });

    int samples_per_frame = frame_size * stream.channelCount;

    while (!shutdown_event->peek()) {
      std::vector<float> sample_buffer;
      sample_buffer.resize(samples_per_frame);

      auto status = mic->sample(sample_buffer);
      policy::sample_status_e policy_status;
      switch (status) {
        case platf::capture_e::ok:
          policy_status = policy::sample_status_e::ok;
          break;
        case platf::capture_e::timeout:
          policy_status = policy::sample_status_e::timeout;
          break;
        case platf::capture_e::reinit:
          policy_status = config::audio.auto_capture ?
                            policy::sample_status_e::reinitialize :
                            policy::sample_status_e::timeout;
          break;
        case platf::capture_e::interrupted:
          policy_status = policy::sample_status_e::interrupted;
          break;
        case platf::capture_e::error:
          policy_status = policy::sample_status_e::error;
          break;
      }
      const auto action = policy::sample_action(policy_status);
      switch (action) {
        case policy::sample_action_e::emit:
          break;
        case policy::sample_action_e::retry:
          continue;
        case policy::sample_action_e::reacquire:
          BOOST_LOG(info) << "Reinitializing audio capture"sv;
          mic.reset();
          do {
            mic = control->microphone(stream.mapping, stream.channelCount, stream.sampleRate, frame_size, continuous_audio, host_audio);
            if (!mic) {
              BOOST_LOG(warning) << "Couldn't re-initialize audio input"sv;
            }
          } while (!mic && !shutdown_event->view(5s));
          continue;
        case policy::sample_action_e::stop:
          return;
      }

      if (config.bypass_opus) {
        if (channel_data == nullptr) {
          webrtc_stream::submit_audio_frame(sample_buffer, stream.sampleRate, stream.channelCount, frame_size);
        }
      } else {
        samples->raise(std::move(sample_buffer));
      }
    }
  }

  audio_ctx_ref_t get_audio_ctx_ref() {
    static auto control_shared {safe::make_shared<audio_ctx_t>(start_audio_control, stop_audio_control)};
    return control_shared.ref();
  }

  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx) {
    if (!ctx.control) {
      return false;
    }

    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (sink.empty()) {
      return false;
    }

    return ctx.control->is_sink_available(sink);
  }

  int map_stream(int channels, bool quality) {
    return policy::stream_index(channels, quality);
  }

  int start_audio_control(audio_ctx_t &ctx) {
    auto fg = util::fail_guard([]() {
      BOOST_LOG(warning) << "There will be no audio"sv;
    });

    ctx.sink_flag = std::make_unique<std::atomic_bool>(false);

    // The default sink has not been replaced yet.
    ctx.restore_sink = false;

    if (!(ctx.control = platf::audio_control())) {
      return 0;
    }

    auto sink = ctx.control->sink_info();
    if (!sink) {
      // Let the calling code know it failed
      ctx.control.reset();
      return 0;
    }

    ctx.sink = std::move(*sink);

    fg.disable();
    return 0;
  }

  void stop_audio_control(audio_ctx_t &ctx) {
    // restore audio-sink if applicable
    if (!ctx.restore_sink) {
      return;
    }

    // Change back to the host sink, unless there was none
    const std::string &sink = ctx.sink.host.empty() ? config::audio.sink : ctx.sink.host;
    if (!sink.empty()) {
      // Best effort, it's allowed to fail. Windows restores the endpoint
      // captured for each role instead of applying the console sink to all of
      // them.
      ctx.control->restore_sink(sink);
    }

    // Ensure Steam Streaming Speakers aren't left as the default device.
    // If the original device is temporarily unavailable (e.g., DisplayPort audio
    // reconnecting after virtual display teardown), the platform layer can keep
    // retrying that exact device in the background after moving away from Steam.
    ctx.control->reset_default_device(sink);
  }

  void apply_surround_params(opus_stream_config_t &stream, const stream_params_t &params) {
    policy::stream_layout_t base {
      stream.channelCount,
      stream.streams,
      stream.coupledStreams,
      {}
    };
    policy::stream_layout_t custom {
      params.channelCount,
      params.streams,
      params.coupledStreams,
      {}
    };
    std::copy_n(params.mapping, custom.mapping.size(), custom.mapping.begin());
    const auto selected = policy::apply_custom_layout(base, custom);
    stream.channelCount = selected.channels;
    stream.streams = selected.streams;
    stream.coupledStreams = selected.coupled_streams;
    stream.mapping = params.mapping;
  }
}  // namespace audio

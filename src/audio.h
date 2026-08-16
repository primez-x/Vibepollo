/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace audio {
  enum class transport_e : std::uint8_t {
    opus,
    mat10,
  };

  // MAT is deliberately opt-in and never shares a legacy Opus payload type.
  constexpr std::uint8_t mat10_rtp_payload_type = 98;
  constexpr std::uint32_t mat10_fragment_magic = 0x3146544dU;  // "MTF1", little-endian on wire.
  constexpr std::uint16_t mat10_fragment_version = 1;
  constexpr std::size_t mat10_fragment_header_bytes = 24;
  constexpr std::size_t mat10_fragment_bytes = 1328;
  constexpr std::size_t mat10_fragment_data_bytes = mat10_fragment_bytes - mat10_fragment_header_bytes;
  constexpr std::uint16_t mat10_fragment_first = 1;
  constexpr std::uint16_t mat10_fragment_last = 2;
  constexpr std::size_t mat10_max_record_bytes = 32'884;

  struct mat10_fragment_view_t {
    std::uint32_t record_sequence {};
    std::uint32_t record_bytes {};
    std::uint32_t fragment_offset {};
    std::uint16_t fragment_bytes {};
    std::uint16_t flags {};
    std::span<const std::uint8_t> data;
  };

  inline void mat10_put_u16(std::span<std::uint8_t> out, const std::size_t offset, const std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value);
    out[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  }
  inline void mat10_put_u32(std::span<std::uint8_t> out, const std::size_t offset, const std::uint32_t value) {
    for (std::size_t index = 0; index != 4; ++index) out[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
  }
  [[nodiscard]] inline std::uint16_t mat10_get_u16(const std::span<const std::uint8_t> in, const std::size_t offset) {
    return static_cast<std::uint16_t>(in[offset]) | (static_cast<std::uint16_t>(in[offset + 1]) << 8);
  }
  [[nodiscard]] inline std::uint32_t mat10_get_u32(const std::span<const std::uint8_t> in, const std::size_t offset) {
    std::uint32_t value {};
    for (int index = 3; index >= 0; --index) value = (value << 8) | in[offset + index];
    return value;
  }
  [[nodiscard]] inline std::optional<mat10_fragment_view_t> parse_mat10_fragment(const std::span<const std::uint8_t> wire) {
    if (wire.size() != mat10_fragment_bytes ||
        mat10_get_u32(wire, 0) != mat10_fragment_magic ||
        mat10_get_u16(wire, 4) != mat10_fragment_version ||
        mat10_get_u16(wire, 6) != mat10_fragment_header_bytes) return std::nullopt;
    const auto record_bytes = mat10_get_u32(wire, 12);
    const auto offset = mat10_get_u32(wire, 16);
    const auto bytes = mat10_get_u16(wire, 20);
    const auto flags = mat10_get_u16(wire, 22);
    if (!record_bytes || record_bytes > mat10_max_record_bytes || bytes > mat10_fragment_data_bytes ||
        offset > record_bytes || bytes > record_bytes - offset || flags == 0 || flags > (mat10_fragment_first | mat10_fragment_last) ||
        ((flags & mat10_fragment_first) && offset != 0) ||
        ((flags & mat10_fragment_last) && offset + bytes != record_bytes)) return std::nullopt;
    for (std::size_t index = mat10_fragment_header_bytes + bytes; index != wire.size(); ++index) {
      if (wire[index] != 0) return std::nullopt;
    }
    return mat10_fragment_view_t {mat10_get_u32(wire, 8), record_bytes, offset, bytes, flags,
                                  wire.subspan(mat10_fragment_header_bytes, bytes)};
  }
  // Used on both ends before an opaque record is accepted. A discontinuity is
  // terminal for the epoch; accepting a later shard would corrupt MAT timing.
  class mat10_fragment_sequence_policy_t {
  public:
    [[nodiscard]] bool admit(const mat10_fragment_view_t &fragment) {
      if (poisoned_) return false;
      if (!active_) {
        if (!(fragment.flags & mat10_fragment_first)) return poison();
        if ((fragment.flags & mat10_fragment_last) && fragment.fragment_bytes != fragment.record_bytes) return poison();
        if (fragment.flags & mat10_fragment_last) return true;
        active_ = true;
        sequence_ = fragment.record_sequence;
        record_bytes_ = fragment.record_bytes;
        next_offset_ = fragment.fragment_bytes;
        return true;
      }
      if (fragment.record_sequence != sequence_ || fragment.record_bytes != record_bytes_ ||
          fragment.fragment_offset != next_offset_ || (fragment.flags & mat10_fragment_first)) return poison();
      next_offset_ += fragment.fragment_bytes;
      if (fragment.flags & mat10_fragment_last) {
        if (next_offset_ != record_bytes_) return poison();
        active_ = false;
      } else if (next_offset_ == record_bytes_) {
        return poison();
      }
      return true;
    }
    [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }
    void reset() noexcept { active_ = poisoned_ = false; sequence_ = record_bytes_ = next_offset_ = 0; }
  private:
    [[nodiscard]] bool poison() noexcept { poisoned_ = true; return false; }
    bool active_ {};
    bool poisoned_ {};
    std::uint32_t sequence_ {};
    std::uint32_t record_bytes_ {};
    std::uint32_t next_offset_ {};
  };
  [[nodiscard]] inline std::vector<std::vector<std::uint8_t>> mat10_fragment_record(
    const std::uint32_t sequence,
    const std::span<const std::uint8_t> record) {
    if (record.empty() || record.size() > mat10_max_record_bytes) return {};
    std::vector<std::vector<std::uint8_t>> result;
    for (std::size_t offset = 0; offset < record.size(); offset += mat10_fragment_data_bytes) {
      const auto bytes = std::min(mat10_fragment_data_bytes, record.size() - offset);
      auto &fragment = result.emplace_back(mat10_fragment_bytes, 0);
      const auto flags = static_cast<std::uint16_t>((offset == 0 ? mat10_fragment_first : 0) |
                                                    (offset + bytes == record.size() ? mat10_fragment_last : 0));
      mat10_put_u32(fragment, 0, mat10_fragment_magic);
      mat10_put_u16(fragment, 4, mat10_fragment_version);
      mat10_put_u16(fragment, 6, mat10_fragment_header_bytes);
      mat10_put_u32(fragment, 8, sequence);
      mat10_put_u32(fragment, 12, static_cast<std::uint32_t>(record.size()));
      mat10_put_u32(fragment, 16, static_cast<std::uint32_t>(offset));
      mat10_put_u16(fragment, 20, static_cast<std::uint16_t>(bytes));
      mat10_put_u16(fragment, 22, flags);
      std::copy_n(record.begin() + offset, bytes, fragment.begin() + mat10_fragment_header_bytes);
    }
    return result;
  }
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  struct opus_stream_config_t {
    std::int32_t sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    const std::uint8_t *mapping;
    int bitrate;
  };

  struct stream_params_t {
    int channelCount;
    int streams;
    int coupledStreams;
    std::uint8_t mapping[8];
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG];

  struct config_t {
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      CONTINUOUS_AUDIO,  ///< Continuous audio
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;
    int channels;
    int mask;
    transport_e transport {transport_e::opus};
    std::array<std::uint8_t, 52> mat10_descriptor {};
    bool bypass_opus = false;

    stream_params_t customStreamParams;

    std::bitset<MAX_FLAGS> flags;

    // Who TF knows what Sunshine did
    // putting input_only at the end of flags will always be over written to true
    uint64_t __padding;

    bool input_only;
  };

  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;

    std::unique_ptr<platf::audio_control_t> control;

    bool restore_sink;
    platf::sink_t sink;
  };

  using buffer_t = util::buffer_t<std::uint8_t>;
  using packet_t = std::pair<void *, buffer_t>;
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;

  void capture(safe::mail_t mail, config_t config, void *channel_data);

  /**
   * @brief Get the reference to the audio context.
   * @returns A shared pointer reference to audio context.
   * @note Aside from the configuration purposes, it can be used to extend the
   *       audio sink lifetime to capture sink earlier and restore it later.
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * @examples_end
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);
}  // namespace audio

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace atmos_mat_direct {
  constexpr std::uint16_t protocol_major = 1;
  // The relay binds the full negotiated WAVEFORMATEXTENSIBLE_IEC61937 form.
  constexpr std::size_t descriptor_bytes = 52;
  constexpr std::size_t carrier_frame_bytes = 16;
  constexpr std::size_t header_bytes = 116;
  constexpr std::size_t max_payload_bytes = 32 * 1024;

  enum record_flags : std::uint32_t { start = 1, discontinuity = 2, format_change = 4, stop = 8 };
  struct descriptor { std::array<std::uint8_t, descriptor_bytes> bytes {}; };
  struct record {
    std::uint64_t generation {}, stream_id {}, first_carrier_frame {}, host_qpc {}, host_qpc_frequency {};
    descriptor descriptor_value {}; std::uint32_t flags {};
    std::vector<std::uint8_t> payload;
  };
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> encode(const record &record);
  [[nodiscard]] std::optional<record> decode(std::span<const std::uint8_t> wire);
}

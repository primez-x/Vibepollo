#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atmos_mat_client {
  inline constexpr std::size_t descriptor_bytes = 52;
  inline constexpr std::uint32_t carrier_frame_bytes = 16;
  inline constexpr std::uint32_t carrier_rate = 192000;
  inline constexpr std::uint32_t feedback_period_hz = 4;

  struct descriptor_identity {
    std::array<std::uint8_t, descriptor_bytes> bytes {};
    friend bool operator==(const descriptor_identity &, const descriptor_identity &) = default;
  };

  struct carrier_block {
    std::uint64_t generation {};
    std::uint64_t first_carrier_frame {};
    std::uint64_t host_qpc {};
    std::uint64_t host_qpc_frequency {};
    descriptor_identity descriptor {};
    std::vector<std::uint8_t> bytes;
  };

  enum class poison_reason {
    none,
    invalid_block,
    generation_mismatch,
    descriptor_mismatch,
    carrier_gap,
    queue_full,
    sink_underrun,
    route_failure,
    format_failure,
    device_clock_failure,
  };
  enum class enqueue_result {
    accepted,
    invalid_block_poisoned,
    generation_mismatch_poisoned,
    descriptor_mismatch_poisoned,
    gap_poisoned,
    queue_full_poisoned,
    poisoned,
  };
  enum class copy_status { copied, empty, insufficient_output, poisoned };

  struct copy_result { copy_status status {}; std::uint32_t frames {}; };
  struct device_clock_feedback {
    std::uint64_t generation {};
    std::uint64_t sequence {};
    std::uint64_t submitted_frames {};
    std::uint64_t played_frames {};
    std::uint64_t device_position_frames {};
    std::uint64_t client_qpc {};
    std::uint64_t client_qpc_frequency {};
  };

  // Pure bounded queue. It never creates carrier bytes: discontinuity, capacity,
  // identity, generation, or sink faults latch poison until reset().
  class writer {
  public:
    writer(descriptor_identity descriptor, std::uint32_t capacity_frames);
    enqueue_result enqueue(carrier_block block);
    copy_result copy_next(std::span<std::uint8_t> destination);
    void acknowledge_played(std::uint64_t device_position_frames) noexcept;
    void poison_sink_underrun() noexcept;
    void poison_route_failure() noexcept;
    void poison_format_failure() noexcept;
    void reset(std::uint64_t generation) noexcept;
    std::optional<device_clock_feedback> feedback(
      std::uint64_t client_qpc, std::uint64_t client_qpc_frequency) noexcept;
    poison_reason poison() const noexcept;
    std::uint64_t submitted_frames() const noexcept;
    std::uint64_t played_frames() const noexcept;
    std::uint64_t queued_frames() const noexcept;
    std::optional<std::uint64_t> generation() const noexcept;

  private:
    struct queued_block { carrier_block block; std::size_t byte_offset {}; };
    void latch(poison_reason reason) noexcept;
    mutable std::mutex mutex_;
    descriptor_identity descriptor_;
    std::uint32_t capacity_frames_ {};
    std::deque<queued_block> queue_;
    std::optional<std::uint64_t> generation_;
    std::optional<std::uint64_t> next_frame_;
    std::uint64_t queued_frames_ {};
    std::uint64_t submitted_frames_ {};
    std::uint64_t played_frames_ {};
    std::uint64_t feedback_sequence_ {};
    std::uint64_t last_feedback_qpc_ {};
    poison_reason poison_ {poison_reason::none};
  };

#ifdef _WIN32
  using device_clock_feedback_callback = void (*)(const device_clock_feedback &, void *context);
  struct windows_render_result {
    std::int32_t hresult {};
    poison_reason poison {poison_reason::none};
    std::uint64_t submitted_frames {};
    std::uint64_t played_frames {};
  };

  // Performs the full probe gate again, then opens only the negotiated MAT10
  // endpoint in event-driven WASAPI exclusive mode.  `keep_running` is polled
  // after each event; a false result is a clean stop, never a concealment path.
  windows_render_result render_windows_mat10_exclusive(
    writer &queue,
    std::wstring_view endpoint_id,
    const descriptor_identity &descriptor,
    bool (*keep_running)(void *),
    void *keep_running_context,
    device_clock_feedback_callback feedback_callback,
    void *feedback_context) noexcept;
#endif
}  // namespace atmos_mat_client

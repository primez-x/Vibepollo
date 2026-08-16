#include "tools/atmos_mat_client_writer.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace atmos_mat_client {
  writer::writer(const descriptor_identity descriptor, const std::uint32_t capacity_frames):
      descriptor_(descriptor), capacity_frames_(capacity_frames) {}

  void writer::latch(const poison_reason reason) noexcept {
    if (poison_ == poison_reason::none) {
      poison_ = reason;
    }
    queue_.clear();
    queued_frames_ = 0;
  }

  enqueue_result writer::enqueue(carrier_block block) {
    const std::lock_guard lock {mutex_};
    if (poison_ != poison_reason::none) return enqueue_result::poisoned;
    if (capacity_frames_ == 0 || block.bytes.empty() ||
        block.bytes.size() % carrier_frame_bytes != 0 ||
        block.host_qpc_frequency == 0) {
      latch(poison_reason::invalid_block);
      return enqueue_result::invalid_block_poisoned;
    }
    const auto frames = static_cast<std::uint64_t>(block.bytes.size() / carrier_frame_bytes);
    if (frames > capacity_frames_ || queued_frames_ > capacity_frames_ - frames) {
      latch(poison_reason::queue_full);
      return enqueue_result::queue_full_poisoned;
    }
    if (block.descriptor != descriptor_) {
      latch(poison_reason::descriptor_mismatch);
      return enqueue_result::descriptor_mismatch_poisoned;
    }
    if (!generation_) generation_ = block.generation;
    if (*generation_ != block.generation) {
      latch(poison_reason::generation_mismatch);
      return enqueue_result::generation_mismatch_poisoned;
    }
    if (!next_frame_) next_frame_ = block.first_carrier_frame;
    if (*next_frame_ != block.first_carrier_frame ||
        block.first_carrier_frame > std::numeric_limits<std::uint64_t>::max() - frames) {
      latch(poison_reason::carrier_gap);
      return enqueue_result::gap_poisoned;
    }
    *next_frame_ += frames;
    queued_frames_ += frames;
    queue_.push_back({std::move(block), 0});
    return enqueue_result::accepted;
  }

  copy_result writer::copy_next(const std::span<std::uint8_t> destination) {
    const std::lock_guard lock {mutex_};
    if (poison_ != poison_reason::none) return {copy_status::poisoned, 0};
    if (queue_.empty()) return {copy_status::empty, 0};
    if (destination.empty() || destination.size() % carrier_frame_bytes != 0) {
      return {copy_status::insufficient_output, 0};
    }
    std::size_t written {};
    while (written != destination.size() && !queue_.empty()) {
      auto &front = queue_.front();
      const auto available = front.block.bytes.size() - front.byte_offset;
      const auto bytes = std::min(destination.size() - written, available);
      std::copy_n(front.block.bytes.data() + front.byte_offset, bytes, destination.data() + written);
      front.byte_offset += bytes;
      written += bytes;
      if (front.byte_offset == front.block.bytes.size()) queue_.pop_front();
    }
    const auto frames = static_cast<std::uint32_t>(written / carrier_frame_bytes);
    queued_frames_ -= frames;
    submitted_frames_ += frames;
    return {copy_status::copied, frames};
  }

  void writer::acknowledge_played(const std::uint64_t device_position_frames) noexcept {
    const std::lock_guard lock {mutex_};
    if (poison_ != poison_reason::none) return;
    if (device_position_frames < played_frames_ || device_position_frames > submitted_frames_) {
      latch(poison_reason::device_clock_failure);
      return;
    }
    played_frames_ = device_position_frames;
  }
  void writer::poison_sink_underrun() noexcept { const std::lock_guard lock {mutex_}; latch(poison_reason::sink_underrun); }
  void writer::poison_route_failure() noexcept { const std::lock_guard lock {mutex_}; latch(poison_reason::route_failure); }
  void writer::poison_format_failure() noexcept { const std::lock_guard lock {mutex_}; latch(poison_reason::format_failure); }
  void writer::reset(const std::uint64_t generation) noexcept {
    const std::lock_guard lock {mutex_};
    queue_.clear(); queued_frames_ = submitted_frames_ = played_frames_ = 0;
    next_frame_.reset(); generation_ = generation; poison_ = poison_reason::none;
    feedback_sequence_ = last_feedback_qpc_ = 0;
  }
  std::optional<device_clock_feedback> writer::feedback(
      const std::uint64_t client_qpc, const std::uint64_t client_qpc_frequency) noexcept {
    const std::lock_guard lock {mutex_};
    if (poison_ != poison_reason::none || !generation_ || client_qpc_frequency == 0) return std::nullopt;
    const auto interval = client_qpc_frequency / feedback_period_hz;
    if (interval == 0 || (last_feedback_qpc_ != 0 && client_qpc - last_feedback_qpc_ < interval)) return std::nullopt;
    last_feedback_qpc_ = client_qpc;
    return device_clock_feedback {*generation_, ++feedback_sequence_, submitted_frames_, played_frames_, played_frames_, client_qpc, client_qpc_frequency};
  }
  poison_reason writer::poison() const noexcept { const std::lock_guard lock {mutex_}; return poison_; }
  std::uint64_t writer::submitted_frames() const noexcept { const std::lock_guard lock {mutex_}; return submitted_frames_; }
  std::uint64_t writer::played_frames() const noexcept { const std::lock_guard lock {mutex_}; return played_frames_; }
  std::uint64_t writer::queued_frames() const noexcept { const std::lock_guard lock {mutex_}; return queued_frames_; }
  std::optional<std::uint64_t> writer::generation() const noexcept { const std::lock_guard lock {mutex_}; return generation_; }
}  // namespace atmos_mat_client

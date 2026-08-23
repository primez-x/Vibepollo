/**
 * @file src/cursor_policy.h
 * @brief Thread-safe runtime cursor capture policy helpers.
 */
#pragma once

#include <atomic>

namespace cursor_policy {
  using state_t = std::atomic_bool;

  inline bool toggle(state_t &state) noexcept {
    bool current = state.load(std::memory_order_relaxed);
    while (!state.compare_exchange_weak(
      current,
      !current,
      std::memory_order_acq_rel,
      std::memory_order_relaxed)) {
    }
    return !current;
  }
}  // namespace cursor_policy

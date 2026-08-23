#include "../../src/cursor_policy.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

TEST(CursorPolicy, ToggleReturnsTheNewValue) {
  cursor_policy::state_t state {true};

  EXPECT_FALSE(cursor_policy::toggle(state));
  EXPECT_FALSE(state.load());
  EXPECT_TRUE(cursor_policy::toggle(state));
  EXPECT_TRUE(state.load());
}

TEST(CursorPolicy, ConcurrentTogglesDoNotLoseUpdates) {
  cursor_policy::state_t state {true};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&state]() {
      for (int j = 0; j < 1000; ++j) {
        cursor_policy::toggle(state);
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }

  EXPECT_TRUE(state.load());
}

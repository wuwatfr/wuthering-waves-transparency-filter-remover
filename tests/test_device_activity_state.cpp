// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "device_activity_state.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "test_check.hpp"
#include <chrono>
#include <iostream>
#include <thread>

int main() {
  wuwa_tfr::DeviceActivityState<int> state;
  CHECK(state.Activate(1));
  CHECK(state.Activate(2));
  CHECK(!state.Activate(1));

  std::atomic<bool> teardown_started{false};
  std::atomic<bool> teardown_acquired{false};
  std::thread teardown;
  {
    auto active = state.Acquire(1);
    CHECK(active);
    teardown = std::thread([&] {
      teardown_started.store(true, std::memory_order_release);
      auto guard = state.Deactivate(1);
      teardown_acquired.store(static_cast<bool>(guard), std::memory_order_release);
    });
    while (!teardown_started.load(std::memory_order_acquire))
      std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!teardown_acquired.load(std::memory_order_acquire));
  }
  teardown.join();
  CHECK(teardown_acquired.load(std::memory_order_acquire));
  CHECK(!state.Acquire(1));
  CHECK(state.Acquire(2));

  CHECK(state.Activate(1));
  CHECK(!state.Activate(1));
  CHECK(state.Acquire(1));
  std::cout << "device activity lifecycle tests passed\n";
  return 0;
}

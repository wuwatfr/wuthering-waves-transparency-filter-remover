// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "pipeline_replacement_state.hpp"

#include <atomic>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "test_check.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using TestReplacementState = wuwa_tfr::PipelineReplacementState<int, int>;

void test_insert_remove_and_toggle_selection() {
  TestReplacementState state;
  int selected = 0;

  CHECK(!state.WithSelected(1, 7, false, true, [&](int value) { selected = value; }));
  CHECK(!state.PutOriginal(1, 7, 11));
  CHECK(!state.PutFinalAntiFade(1, 7, 22));

  CHECK(state.WithSelected(1, 7, false, true, [&](int value) { selected = value; }));
  CHECK(selected == 11);
  CHECK(state.WithSelected(1, 7, true, true, [&](int value) { selected = value; }));
  CHECK(selected == 22);

  // Anti-Fade ON with the DEV final override suppressed keeps the application's
  // base pipeline; it must not select the original (Anti-Fade OFF) clone.
  selected = 0;
  CHECK(!state.WithSelected(1, 7, true, false, [&](int value) { selected = value; }));
  CHECK(selected == 0);

  const auto previous = state.PutOriginal(1, 7, 33);
  CHECK(previous && *previous == 11);
  CHECK(state.WithSelected(1, 7, false, true, [&](int value) { selected = value; }));
  CHECK(selected == 33);

  const auto removed = state.Remove(1, 7);
  CHECK(removed.original && *removed.original == 33);
  CHECK(removed.final_antifade && *removed.final_antifade == 22);
  const auto [originals, finals] = state.Sizes();
  CHECK(originals == 0 && finals == 0);
}

void test_writer_waits_for_inflight_bind_reader() {
  TestReplacementState state;
  CHECK(!state.PutFinalAntiFade(1, 9, 44));

  std::atomic<bool> reader_entered{false};
  std::atomic<bool> release_reader{false};
  std::atomic<bool> writer_finished{false};

  std::thread reader([&] {
    const bool found = state.WithSelected(1, 9, true, true, [&](int value) {
      CHECK(value == 44);
      reader_entered.store(true, std::memory_order_release);
      while (!release_reader.load(std::memory_order_acquire))
        std::this_thread::yield();
    });
    CHECK(found);
  });

  while (!reader_entered.load(std::memory_order_acquire))
    std::this_thread::yield();

  TestReplacementState::Removed removed;
  std::thread writer([&] {
    removed = state.Remove(1, 9);
    writer_finished.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(!writer_finished.load(std::memory_order_acquire));
  release_reader.store(true, std::memory_order_release);

  reader.join();
  writer.join();
  CHECK(writer_finished.load(std::memory_order_acquire));
  CHECK(removed.final_antifade && *removed.final_antifade == 44);
}

void test_concurrent_readers_with_recreation() {
  TestReplacementState state;
  CHECK(!state.PutFinalAntiFade(1, 12, 1));
  std::atomic<std::uint64_t> reads{0};

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      for (int iteration = 0; iteration < 10000; ++iteration) {
        state.WithSelected(1, 12, true, true, [&](int value) {
          CHECK(value > 0);
          reads.fetch_add(1, std::memory_order_relaxed);
        });
      }
    });
  }

  for (int generation = 2; generation < 200; ++generation) {
    auto previous = state.PutFinalAntiFade(1, 12, generation);
    CHECK(previous && *previous > 0);
  }
  for (auto& reader : readers) reader.join();
  CHECK(reads.load(std::memory_order_relaxed) != 0);
}

void test_device_scoped_identity_and_drain() {
  TestReplacementState state;
  CHECK(!state.PutOriginal(1, 77, 10));
  CHECK(!state.PutFinalAntiFade(1, 77, 11));
  CHECK(!state.PutFinalAntiFade(2, 77, 21));

  int selected = 0;
  CHECK(state.WithSelected(1, 77, true, true, [&](int value) { selected = value; }));
  CHECK(selected == 11);
  CHECK(state.WithSelected(2, 77, true, true, [&](int value) { selected = value; }));
  CHECK(selected == 21);

  const auto drained = state.DrainOwner(1);
  CHECK(drained.size() == 1);
  CHECK(drained[0].application_pipeline == 77);
  CHECK(drained[0].replacements.original &&
      *drained[0].replacements.original == 10);
  CHECK(drained[0].replacements.final_antifade &&
      *drained[0].replacements.final_antifade == 11);
  CHECK(!state.WithSelected(1, 77, true, true, [&](int) {}));
  CHECK(state.WithSelected(2, 77, true, true, [&](int value) { selected = value; }));
  CHECK(selected == 21);
  const auto [originals, finals] = state.Sizes();
  CHECK(originals == 0 && finals == 1);
}

void test_device_drain_waits_for_inflight_bind() {
  TestReplacementState state;
  CHECK(!state.PutFinalAntiFade(1, 88, 31));
  CHECK(!state.PutFinalAntiFade(2, 88, 41));

  std::atomic<bool> reader_entered{false};
  std::atomic<bool> release_reader{false};
  std::atomic<bool> drain_finished{false};
  std::vector<TestReplacementState::Drained> drained;

  std::thread reader([&] {
    CHECK(state.WithSelected(1, 88, true, true, [&](int value) {
      CHECK(value == 31);
      reader_entered.store(true, std::memory_order_release);
      while (!release_reader.load(std::memory_order_acquire))
        std::this_thread::yield();
    }));
  });
  while (!reader_entered.load(std::memory_order_acquire))
    std::this_thread::yield();

  std::thread teardown([&] {
    drained = state.DrainOwner(1);
    drain_finished.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(!drain_finished.load(std::memory_order_acquire));
  release_reader.store(true, std::memory_order_release);

  reader.join();
  teardown.join();
  CHECK(drained.size() == 1);
  CHECK(drained[0].replacements.final_antifade &&
      *drained[0].replacements.final_antifade == 31);
  int selected = 0;
  CHECK(!state.WithSelected(1, 88, true, true, [&](int) {}));
  CHECK(state.WithSelected(2, 88, true, true,
      [&](int value) { selected = value; }));
  CHECK(selected == 41);
}

int main() {
  test_insert_remove_and_toggle_selection();
  test_writer_waits_for_inflight_bind_reader();
  test_concurrent_readers_with_recreation();
  test_device_scoped_identity_and_drain();
  test_device_drain_waits_for_inflight_bind();
  std::cout << "pipeline replacement reader/writer tests passed\n";
  return 0;
}

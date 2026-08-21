// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "preparation_context_pool.hpp"
#include "single_flight_cache.hpp"
#include "test_check.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

struct Result {
  int value = -1;
  bool success = false;
};

class Gate {
 public:
  void ArriveAndWait() {
    std::unique_lock lock(mutex_);
    ++arrivals_;
    changed_.notify_all();
    changed_.wait(lock, [&] { return open_; });
  }

  void WaitFor(std::size_t count) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return arrivals_ >= count; });
  }

  void Open() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t arrivals_ = 0;
  bool open_ = false;
};

void DifferentKeysPrepareConcurrently() {
  wuwa_tfr::SingleFlightCache<int, Result> cache;
  Gate entered;
  std::array<Result, 2> results;
  std::thread first([&] {
    results[0] = cache.GetOrPrepare(1, [&] {
      entered.ArriveAndWait();
      return Result{1, true};
    }, [] { return Result{}; });
  });
  std::thread second([&] {
    results[1] = cache.GetOrPrepare(2, [&] {
      entered.ArriveAndWait();
      return Result{2, true};
    }, [] { return Result{}; });
  });
  // A globally serialized expensive callback could not reach this barrier.
  entered.WaitFor(2);
  entered.Open();
  first.join();
  second.join();
  CHECK(results[0].success && results[0].value == 1);
  CHECK(results[1].success && results[1].value == 2);
}

void SameKeyIsSingleFlight() {
  wuwa_tfr::SingleFlightCache<int, Result> cache;
  constexpr std::size_t kCallers = 16;
  Gate entered;
  std::atomic<int> calls{0};
  std::array<Result, kCallers> results;
  std::vector<std::thread> callers;
  callers.reserve(kCallers);
  for (std::size_t index = 0; index < kCallers; ++index) {
    callers.emplace_back([&, index] {
      results[index] = cache.GetOrPrepare(7, [&] {
        calls.fetch_add(1, std::memory_order_relaxed);
        entered.ArriveAndWait();
        return Result{77, true};
      }, [] { return Result{}; });
    });
  }
  entered.WaitFor(1);
  entered.Open();
  for (auto& caller : callers) caller.join();
  CHECK(calls.load(std::memory_order_relaxed) == 1);
  for (const Result& result : results)
    CHECK(result.success && result.value == 77);
}

void FailureAndThrownOwnerAreCachedAndWakeWaiters() {
  {
    wuwa_tfr::SingleFlightCache<int, Result> cache;
    constexpr std::size_t kCallers = 12;
    std::atomic<int> calls{0};
    Gate owner_entered;
    std::array<Result, kCallers> results;
    std::vector<std::thread> callers;
    for (std::size_t index = 0; index < kCallers; ++index) {
      callers.emplace_back([&, index] {
        results[index] = cache.GetOrPrepare(3, [&] {
          calls.fetch_add(1, std::memory_order_relaxed);
          owner_entered.ArriveAndWait();
          return Result{3, false};
        }, [] { return Result{}; });
      });
    }
    owner_entered.WaitFor(1);
    owner_entered.Open();
    for (auto& caller : callers) caller.join();
    CHECK(calls.load(std::memory_order_relaxed) == 1);
    for (const Result& result : results) CHECK(!result.success);
    int retries = 0;
    const Result cached = cache.GetOrPrepare(3, [&] {
      ++retries;
      return Result{3, true};
    }, [] { return Result{}; });
    CHECK(!cached.success && retries == 0);
  }
  {
    wuwa_tfr::SingleFlightCache<int, Result> cache;
    Gate owner_entered;
    std::atomic<int> calls{0};
    std::array<Result, 8> results;
    std::vector<std::thread> callers;
    for (std::size_t index = 0; index < results.size(); ++index) {
      callers.emplace_back([&, index] {
        results[index] = cache.GetOrPrepare(4, [&] {
          calls.fetch_add(1, std::memory_order_relaxed);
          owner_entered.ArriveAndWait();
          throw std::runtime_error("owner failed");
          return Result{4, true};
        }, [] { return Result{-4, false}; });
      });
    }
    owner_entered.WaitFor(1);
    owner_entered.Open();
    for (auto& caller : callers) caller.join();
    CHECK(calls.load(std::memory_order_relaxed) == 1);
    for (const Result& result : results)
      CHECK(!result.success && result.value == -4);
    int retries = 0;
    const Result cached = cache.GetOrPrepare(4, [&] {
      ++retries;
      return Result{4, true};
    }, [] { return Result{}; });
    CHECK(!cached.success && retries == 0);
  }
}

void MixedTrafficHasOnePublicationPerKey() {
  for (int round = 0; round < 32; ++round) {
    wuwa_tfr::SingleFlightCache<int, Result> cache;
    constexpr std::size_t kCallers = 24;
    Gate start;
    std::array<std::atomic<int>, 4> calls{};
    std::array<Result, kCallers> results;
    std::vector<std::thread> callers;
    callers.reserve(kCallers);
    for (std::size_t index = 0; index < kCallers; ++index) {
      callers.emplace_back([&, index] {
        const int key = static_cast<int>(index % calls.size());
        start.ArriveAndWait();
        results[index] = cache.GetOrPrepare(key, [&] {
          calls[key].fetch_add(1, std::memory_order_relaxed);
          return Result{key, true};
        }, [] { return Result{}; });
      });
    }
    start.WaitFor(kCallers);
    start.Open();
    for (auto& caller : callers) caller.join();
    for (std::size_t key = 0; key < calls.size(); ++key)
      CHECK(calls[key].load(std::memory_order_relaxed) == 1);
    for (std::size_t index = 0; index < results.size(); ++index)
      CHECK(results[index].success && results[index].value ==
          static_cast<int>(index % calls.size()));
    CHECK(cache.CompletedSize() == calls.size());
  }
}

void ContextPoolDrainWaitsForActiveLease() {
  std::atomic<int> created{0};
  wuwa_tfr::PreparationContextPool<int> pool(2, [&] {
    created.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<int>(42);
  });
  Gate lease_active;
  std::thread worker([&] {
    auto context = pool.Acquire();
    CHECK(context && *context == 42);
    lease_active.ArriveAndWait();
  });
  lease_active.WaitFor(1);
  std::atomic<bool> drain_started{false};
  std::atomic<bool> drained{false};
  std::thread teardown([&] {
    drain_started.store(true, std::memory_order_release);
    pool.Drain();
    drained.store(true, std::memory_order_release);
  });
  while (!drain_started.load(std::memory_order_acquire))
    std::this_thread::yield();
  // The active Lease is a deterministic reason Drain cannot complete yet.
  CHECK(!drained.load(std::memory_order_acquire));
  lease_active.Open();
  worker.join();
  teardown.join();
  CHECK(drained.load(std::memory_order_acquire));
  auto recreated = pool.Acquire();
  CHECK(recreated && *recreated == 42);
  CHECK(created.load(std::memory_order_relaxed) >= 2);
}

} // namespace

int main() {
  DifferentKeysPrepareConcurrently();
  SameKeyIsSingleFlight();
  FailureAndThrownOwnerAreCachedAndWakeWaiters();
  MixedTrafficHasOnePublicationPerKey();
  ContextPoolDrainWaitsForActiveLease();
  return 0;
}

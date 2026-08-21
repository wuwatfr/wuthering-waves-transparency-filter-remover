// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace wuwa_tfr {

template <typename Value>
struct NoRetainedPayloadBytes {
  std::size_t operator()(const Value&) const noexcept { return 0; }
};

// A synchronous per-key single-flight cache. The caller that installs a new
// in-flight entry runs Work on its own thread; callers for that key wait and
// receive the exact cached completion value. Work must return a default
// constructible, copyable Value. Abort is used if Work throws so waiters are
// always released and the failure becomes the cached completion.
template <typename Key, typename Value, typename Hash = std::hash<Key>,
    typename RetainedPayloadBytes = NoRetainedPayloadBytes<Value>>
class SingleFlightCache {
 public:
  static_assert(std::is_default_constructible_v<Value>);
  static_assert(std::is_nothrow_move_assignable_v<Value>);
  static_assert(noexcept(std::declval<const RetainedPayloadBytes&>()(
      std::declval<const Value&>())));

  struct Snapshot {
    std::size_t completed_entries = 0;
    std::size_t in_flight_entries = 0;
    std::size_t retained_payload_bytes = 0;
  };

  template <typename Work, typename Abort>
  Value GetOrPrepare(const Key& key, Work&& work, Abort&& abort) {
    std::shared_ptr<InFlight> flight;
    {
      std::unique_lock lock(mutex_);
      if (const auto completed = completed_.find(key); completed != completed_.end())
        return completed->second;
      if (const auto waiting = in_flight_.find(key); waiting != in_flight_.end()) {
        flight = waiting->second;
        flight->completed.wait(lock, [&] { return flight->ready; });
        return flight->value;
      }
      flight = std::make_shared<InFlight>();
      in_flight_.emplace(key, flight);
    }

    Value value{};
    try {
      value = std::forward<Work>(work)();
    } catch (...) {
      // Abort is intentionally called outside the cache lock. It must return
      // a fail-closed value; either path reaches Publish below.
      try {
        value = std::forward<Abort>(abort)();
      } catch (...) {
        value = Value{};
      }
    }

    {
      std::lock_guard lock(mutex_);
      // This owner is the only publisher for key until it removes in_flight_.
      // Publish to waiters before attempting a cache allocation, so even an
      // allocation failure cannot leave the entry permanently in progress.
      flight->value = std::move(value);
      in_flight_.erase(key);
      flight->ready = true;
      try {
        // Store failures too, preserving cache semantics for all normal
        // completions. An allocation failure has already been published as a
        // fail-closed non-stuck result and may be retried later.
        const auto [completed, inserted] = completed_.emplace(key, flight->value);
        if (inserted)
          retained_payload_bytes_ += payload_bytes_(completed->second);
      } catch (...) {
      }
    }
    flight->completed.notify_all();
    return flight->value;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    completed_.clear();
    retained_payload_bytes_ = 0;
  }

  std::size_t CompletedSize() const {
    std::lock_guard lock(mutex_);
    return completed_.size();
  }

  // Obtains all telemetry-relevant cache state under this one short lock.
  // Retained bytes are maintained during publication and Clear(), so this
  // never scans the completed cache during a telemetry sample.
  Snapshot GetSnapshot() const {
    std::lock_guard lock(mutex_);
    return {completed_.size(), in_flight_.size(), retained_payload_bytes_};
  }

 private:
  struct InFlight {
    std::condition_variable completed;
    bool ready = false;
    Value value{};
  };

  mutable std::mutex mutex_;
  std::unordered_map<Key, Value, Hash> completed_;
  std::unordered_map<Key, std::shared_ptr<InFlight>, Hash> in_flight_;
  RetainedPayloadBytes payload_bytes_;
  std::size_t retained_payload_bytes_ = 0;
};

} // namespace wuwa_tfr

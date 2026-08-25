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
      try {
        value = std::forward<Abort>(abort)();
      } catch (...) {
        value = Value{};
      }
    }

    {
      std::lock_guard lock(mutex_);
      flight->value = std::move(value);
      in_flight_.erase(key);
      flight->ready = true;
      try {
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

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace wuwa_tfr {

// Recycles independently owned preparation contexts. Context methods are only
// used through a Lease, so no Context instance is used concurrently. Drain
// waits for active leases before destroying idle contexts; callers execute on
// their existing threads and this class never owns worker threads.
template <typename Context>
class PreparationContextPool {
 public:
  using Factory = std::function<std::unique_ptr<Context>()>;

  class Lease {
   public:
    Lease() = default;
    Lease(Lease&& other) noexcept
        : pool_(std::exchange(other.pool_, nullptr)),
          context_(std::move(other.context_)) {}
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        Reset();
        pool_ = std::exchange(other.pool_, nullptr);
        context_ = std::move(other.context_);
      }
      return *this;
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { Reset(); }

    Context* operator->() const noexcept { return context_.get(); }
    Context& operator*() const noexcept { return *context_; }
    explicit operator bool() const noexcept { return context_ != nullptr; }

   private:
    friend class PreparationContextPool;
    Lease(PreparationContextPool* pool, std::unique_ptr<Context> context)
        : pool_(pool), context_(std::move(context)) {}

    void Reset() noexcept {
      if (pool_) pool_->Release(std::move(context_));
      pool_ = nullptr;
    }

    PreparationContextPool* pool_ = nullptr;
    std::unique_ptr<Context> context_;
  };

  PreparationContextPool(std::size_t capacity, Factory factory)
      : capacity_(capacity), factory_(std::move(factory)) {
    idle_.reserve(capacity_);
  }
  PreparationContextPool(const PreparationContextPool&) = delete;
  PreparationContextPool& operator=(const PreparationContextPool&) = delete;

  Lease Acquire() {
    std::unique_ptr<Context> context;
    bool create = false;
    {
      std::unique_lock lock(mutex_);
      available_.wait(lock, [&] {
        return !draining_ && (!idle_.empty() || contexts_ < capacity_);
      });
      if (!idle_.empty()) {
        context = std::move(idle_.back());
        idle_.pop_back();
      } else {
        ++contexts_;
        create = true;
      }
      ++leases_;
    }
    if (create) {
      try {
        context = factory_();
      } catch (...) {
        Release(nullptr, true);
        throw;
      }
      if (!context) {
        Release(nullptr, true);
        return {};
      }
    }
    return Lease(this, std::move(context));
  }

  // Called only after the device activity layer has blocked new preparation
  // callbacks. It waits for every active Lease before unloading Context state.
  void Drain() {
    std::unique_lock lock(mutex_);
    draining_ = true;
    available_.wait(lock, [&] { return leases_ == 0; });
    idle_.clear();
    contexts_ = 0;
    draining_ = false;
    lock.unlock();
    available_.notify_all();
  }

 private:
  void Release(std::unique_ptr<Context> context, bool creation_failed = false) noexcept {
    {
      std::lock_guard lock(mutex_);
      if (context && !draining_) idle_.push_back(std::move(context));
      if (creation_failed) --contexts_;
      --leases_;
    }
    available_.notify_all();
  }

  const std::size_t capacity_;
  Factory factory_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::vector<std::unique_ptr<Context>> idle_;
  std::size_t contexts_ = 0;
  std::size_t leases_ = 0;
  bool draining_ = false;
};

} // namespace wuwa_tfr

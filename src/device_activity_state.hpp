// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <mutex>
#include <shared_mutex>
#include <unordered_set>
#include <utility>

namespace wuwa_tfr {

// Serializes create/init/destroy callbacks against teardown for a logical
// graphics device. Bind callbacks deliberately do not use this gate; pipeline
// replacement state provides their shorter reader lifetime protection.
template <typename DeviceId>
class DeviceActivityState {
 public:
  class ActiveGuard {
   public:
    ActiveGuard(ActiveGuard&&) noexcept = default;
    ActiveGuard& operator=(ActiveGuard&&) noexcept = default;
    ActiveGuard(const ActiveGuard&) = delete;
    ActiveGuard& operator=(const ActiveGuard&) = delete;

    explicit operator bool() const noexcept { return active_; }

   private:
    friend class DeviceActivityState;
    ActiveGuard(std::shared_lock<std::shared_mutex> lock, bool active) noexcept
        : lock_(std::move(lock)), active_(active) {}

    std::shared_lock<std::shared_mutex> lock_;
    bool active_ = false;
  };

  class DeactivationGuard {
   public:
    DeactivationGuard(DeactivationGuard&&) noexcept = default;
    DeactivationGuard& operator=(DeactivationGuard&&) noexcept = default;
    DeactivationGuard(const DeactivationGuard&) = delete;
    DeactivationGuard& operator=(const DeactivationGuard&) = delete;

    explicit operator bool() const noexcept { return was_active_; }

   private:
    friend class DeviceActivityState;
    DeactivationGuard(
        std::unique_lock<std::shared_mutex> lock,
        bool was_active) noexcept
        : lock_(std::move(lock)), was_active_(was_active) {}

    std::unique_lock<std::shared_mutex> lock_;
    bool was_active_ = false;
  };

  bool Activate(DeviceId device) {
    std::unique_lock lock(mutex_);
    return active_.insert(device).second;
  }

  ActiveGuard Acquire(DeviceId device) const {
    std::shared_lock lock(mutex_);
    return ActiveGuard(std::move(lock), active_.contains(device));
  }

  DeactivationGuard Deactivate(DeviceId device) {
    std::unique_lock lock(mutex_);
    const bool was_active = active_.erase(device) != 0;
    return DeactivationGuard(std::move(lock), was_active);
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_set<DeviceId> active_;
};

} // namespace wuwa_tfr

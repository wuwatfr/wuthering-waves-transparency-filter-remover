// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/experiments/experiments_common.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

std::atomic<bool> g_target_bypass_enabled{false};
std::atomic<TargetBypassMode> g_target_bypass_mode{
    TargetBypassMode::AllVerifiedV1};
std::atomic<std::uint64_t> g_target_bypass_selected_hash{0};
std::mutex g_target_bypass_mutex;
std::unordered_map<DeviceIdentity, device*> g_target_bypass_devices;

thread_local bool g_target_bypass_internal_create = false;
thread_local bool g_target_bypass_internal_bind = false;
thread_local bool g_target_bypass_internal_destroy = false;

std::uint64_t SelectedTargetBypassHash() noexcept {
  return g_target_bypass_selected_hash.load(std::memory_order_relaxed);
}

void DestroyTargetBypassReplacement(device* owner, pipeline replacement) {
  if (!owner || replacement.handle == 0) return;
  ScopedThreadFlag internal_destroy(g_target_bypass_internal_destroy);
  owner->destroy_pipeline(replacement);
}

}  // namespace wuwa_tfr::dev

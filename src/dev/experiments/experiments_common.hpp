// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// State and helpers shared between the two Dev-only pipeline-replacement
// experiments that both target a device's cached pipeline recipes: the dead
// legacy single-target bypass (experiments_legacy_bypass.*) and the live
// Fade Primitive execution-set (experiments_fade_primitive.*).

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <reshade.hpp>

#include "production/addon_shared.hpp"

namespace wuwa_tfr::dev {

enum class TargetBypassMode : std::uint8_t {
  AllVerifiedV1,
  ManualShaderList,
  // Retained only so existing single-target helper code stays inert while the
  // Dev UI migrates to hash-group target modes. It is not selectable.
  LegacySelectedShader,
};

// RAII guard for the thread-local internal-call re-entrancy flags below.
class ScopedThreadFlag {
 public:
  explicit ScopedThreadFlag(bool& flag) : flag_(flag), previous_(flag) {
    flag_ = true;
  }
  ~ScopedThreadFlag() { flag_ = previous_; }

 private:
  bool& flag_;
  bool previous_;
};

extern std::atomic<bool> g_target_bypass_enabled;
extern std::atomic<TargetBypassMode> g_target_bypass_mode;
extern std::atomic<std::uint64_t> g_target_bypass_selected_hash;
extern std::mutex g_target_bypass_mutex;
extern std::unordered_map<DeviceIdentity, reshade::api::device*>
    g_target_bypass_devices;

extern thread_local bool g_target_bypass_internal_create;
extern thread_local bool g_target_bypass_internal_bind;
extern thread_local bool g_target_bypass_internal_destroy;

std::uint64_t SelectedTargetBypassHash() noexcept;

// Destroys a replacement pipeline created by one of the experiments below,
// marking the internal-destroy flag so the Dev trace subsystem's
// destroy_pipeline hook does not treat it as an application PSO teardown.
void DestroyTargetBypassReplacement(
    reshade::api::device* owner, reshade::api::pipeline replacement);

}  // namespace wuwa_tfr::dev

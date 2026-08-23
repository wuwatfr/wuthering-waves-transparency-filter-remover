// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Bridges the small set of always-compiled addon.cpp bootstrap state (device
// activity tracking, process/config resolution, logging) that every variant
// module -- production/production_module.cpp and dev/dev_module.cpp -- needs
// to reference. This is genuinely generic: it owns no variant-specific
// behavior, and neither variant's replacement logic depends on it
// (fade_primitive_runtime.cpp does not include this header at all).
//
// Definitions for everything declared here live in addon.cpp, since
// addon.cpp is compiled into BOTH the WuwaTFR and WuwaTFRDev targets and is
// the only place these entities can have a single, shared definition. This
// header merely gives them external, not anonymous-namespace, linkage so the
// variant modules can reference the very same instances.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <reshade.hpp>

#include "device_activity_state.hpp"

namespace wuwa_tfr {

using DeviceIdentity = std::uintptr_t;

inline DeviceIdentity DeviceKey(reshade::api::device* owner) noexcept {
  return reinterpret_cast<DeviceIdentity>(owner);
}

// Defined in addon.cpp.
extern DeviceActivityState<DeviceIdentity> g_device_activity;
extern bool g_target_process;
extern std::filesystem::path g_addon_directory;

void Log(reshade::log::level level, const std::string& message);
std::filesystem::path ConfigPath();
bool ConfigFlag(const wchar_t* key, bool fallback);
void SaveConfigFlag(const wchar_t* key, bool value);
std::filesystem::path ConfigPathValue(const wchar_t* key);

// Generic device-activity bootstrap, registered unconditionally by
// addon.cpp's DllMain for every target. Owns no variant-specific behavior.
void OnInitDevice(reshade::api::device* owner);
void OnDestroyDevice(reshade::api::device* owner);

}  // namespace wuwa_tfr

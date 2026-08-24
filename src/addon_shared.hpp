// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

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

extern DeviceActivityState<DeviceIdentity> g_device_activity;
extern bool g_target_process;
extern std::filesystem::path g_addon_directory;

void Log(reshade::log::level level, const std::string& message);
std::filesystem::path ConfigPath();
bool ConfigFlag(const wchar_t* key, bool fallback);
void SaveConfigFlag(const wchar_t* key, bool value);
std::filesystem::path ConfigPathValue(const wchar_t* key);

void OnInitDevice(reshade::api::device* owner);
void OnDestroyDevice(reshade::api::device* owner);

}  // namespace wuwa_tfr

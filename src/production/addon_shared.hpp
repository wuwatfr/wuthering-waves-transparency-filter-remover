// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Bridges the small set of always-compiled addon.cpp infrastructure (shader
// inspection cache, DXC bridge ownership, device activity tracking, and a
// handful of formatting/logging helpers) that both the Production runtime
// and the Dev-only modules under src/dev/ need to reference.
//
// Definitions for everything declared here live in addon.cpp itself, since
// addon.cpp is compiled into BOTH the WuwaTFR and WuwaTFRDev targets and is
// the only place these entities can have a single, shared definition. This
// header merely gives them external linkage (instead of the anonymous
// namespace's internal linkage) so the Dev-only translation units under
// src/dev/ can reference the very same instances.
//
// This header intentionally contains no Dev-only *behavior* -- only shared
// state and shared helper declarations that already existed, unconditionally
// compiled, in addon.cpp before this refactor.

#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include <reshade.hpp>

#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"

#if WUWA_TFR_DEVTOOLS
#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"
#endif

namespace wuwa_tfr {

using DeviceIdentity = std::uintptr_t;

inline DeviceIdentity DeviceKey(reshade::api::device* owner) noexcept {
  return reinterpret_cast<DeviceIdentity>(owner);
}

// One entry per unique observed DXIL pixel shader. Dev-only fields carry the
// independent structural diagnostics; Production never reads them.
struct InspectionRecord {
  bool success = false;
  bool dumped = false;
  std::size_t bytecode_size = 0;
#if WUWA_TFR_DEVTOOLS
  wuwa_tfr::SpatialDitherDiagnostic dither;
  wuwa_tfr::FadePrimitiveDiagnostic fade_primitive;
#endif
  std::string error;
};

// Defined in addon.cpp.
extern std::mutex g_inspection_mutex;
extern DxcBridge* g_dxc;
extern std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;
extern DeviceActivityState<DeviceIdentity> g_device_activity;
extern bool g_target_process;
extern std::filesystem::path g_addon_directory;

std::string Hex64(std::uint64_t value);
void Log(reshade::log::level level, const std::string& message);
std::filesystem::path DumpDir();
void InspectPixelShader(const reshade::api::shader_desc& descriptor);
bool LooksLikeDxil(const reshade::api::shader_desc& descriptor);
std::uint64_t Fnv1a64(const void* data, std::size_t size);

}  // namespace wuwa_tfr

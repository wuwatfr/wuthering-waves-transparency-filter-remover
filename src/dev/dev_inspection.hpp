// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The Dev-only shader-inspection cache: the DXC-backed disassembly used for
// diagnostic capture/dump, and the create_pipeline callback that feeds it.
// FadePrimitiveRuntime does not depend on any of this -- it has its own,
// entirely separate DXC pool (fade_primitive_runtime.cpp) -- this module
// exists purely to serve Dev's own capture/diagnostic tooling.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <reshade.hpp>

#include "dxc_bridge.hpp"
#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

// One entry per unique observed DXIL pixel shader.
struct InspectionRecord {
  bool success = false;
  bool dumped = false;
  std::size_t bytecode_size = 0;
  wuwa_tfr::SpatialDitherDiagnostic dither;
  wuwa_tfr::FadePrimitiveDiagnostic fade_primitive;
  std::string error;
};

extern std::mutex g_inspection_mutex;
extern DxcBridge* g_dxc;
extern std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;

extern std::atomic<std::uint64_t> g_seen_shader_callbacks;
extern std::atomic<std::uint64_t> g_unique_dxil_shaders;
extern std::atomic<std::uint64_t> g_disassembly_successes;
extern std::atomic<std::uint64_t> g_disassembly_failures;
extern std::atomic<std::uint64_t> g_dumped_shaders;

// Read from WuwaTFR.ini's [General] Diagnostic/Dump keys (or their
// WUWA_TFR_DIAGNOSTIC/WUWA_TFR_DUMP environment-variable fallbacks) by
// InitializeInspectionConfig().
extern bool g_diagnostic;
extern bool g_dump;

bool LooksLikeDxil(const reshade::api::shader_desc& descriptor);
std::uint64_t Fnv1a64(const void* data, std::size_t size);
void InspectPixelShader(const reshade::api::shader_desc& descriptor);

// Only registered as an event in the Dev build; Production never wires this
// up, so it must not be compiled into that binary.
bool OnCreatePipeline(
    reshade::api::device* owner,
    reshade::api::pipeline_layout layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects);

// Called from the Dev variant's InitializeVariant(), before any event can
// fire.
void InitializeInspectionConfig();

// Called from the Dev variant's OnLastDeviceDestroyed().
void TeardownInspectionOnLastDevice();

}  // namespace wuwa_tfr::dev

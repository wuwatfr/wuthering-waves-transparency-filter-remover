// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only shader-inspection counters and the DXIL-pixel-shader lookup used
// while walking a pipeline's creation subobjects. This is the Dev-only slice
// of the shader inspection subsystem; the always-compiled parts (the
// inspection cache itself, InspectPixelShader, WriteCapture, ...) remain in
// addon.cpp and are shared via production/addon_shared.hpp, because
// Production also needs them.

#pragma once

#include <atomic>
#include <cstdint>

#include <reshade.hpp>

namespace wuwa_tfr::dev {

// Read only by DrawTraceOverlay; written only by InspectPixelShader's
// Dev-only analysis branch (addon.cpp).
extern std::atomic<std::uint64_t> g_discard_shader_count;
extern std::atomic<std::uint64_t> g_strict_spatial_dither_count;
extern std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count;
extern std::atomic<std::uint64_t> g_fade_primitive_shader_count;
extern std::atomic<std::uint64_t> g_fade_primitive_instance_count;

// Scans a pipeline's creation subobjects for the single DXIL pixel shader,
// returning false if none is found or if more than one distinct DXIL pixel
// shader is present (ambiguous).
bool FindDxilPixelShader(
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    const reshade::api::shader_desc*& descriptor,
    std::uint64_t& shader_hash);

}  // namespace wuwa_tfr::dev

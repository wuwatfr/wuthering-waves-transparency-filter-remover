// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-wide shared utilities: the shader-inspection diagnostic counters, the
// DXIL-pixel-shader lookup used while walking a pipeline's creation
// subobjects, read-only Fade Primitive v1 diagnostic display helpers, and
// the two small output-formatting/directory helpers used across the trace,
// inspection, and overlay modules (Hex64, DumpDir).
//
// This module owns no replacement state: the sole fade-primitive replacement
// owner is wuwa_tfr::FadePrimitiveRuntime (see dev/dev_runtime.hpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

#include <reshade.hpp>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

// Read only by DrawTraceOverlay; written only by InspectPixelShader
// (dev/dev_inspection.*).
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

// Comma-separated, de-duplicated list of a verified Fade Primitive v1
// diagnostic's consumer kinds, for display only.
std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic);

// Set from the Dev variant's InitializeVariant() (WuwaTFR.ini's DumpPath).
// Read by both the shader-capture subsystem and the trace TSV exporters.
extern std::filesystem::path g_dump_path;
std::filesystem::path DumpDir();

std::string Hex64(std::uint64_t value);

}  // namespace wuwa_tfr::dev

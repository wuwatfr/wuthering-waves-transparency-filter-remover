// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-wide shared utilities: the shader-inspection diagnostic counters,
// read-only Fade Primitive v1 diagnostic display helpers, and the two small
// output-formatting/directory helpers used across the trace, inspection, and
// overlay modules (Hex64, DumpDir).
//
// DXIL pixel-shader identification is not this module's concern: both
// Production and Dev trace call the single canonical implementation in
// pixel_shader_identity.hpp.
//
// This module owns no replacement state: the sole fade-primitive replacement
// owner is wuwa_tfr::FadePrimitiveRuntime (see dev/dev_runtime.hpp).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <reshade.hpp>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

// Read only by DrawTraceOverlay; written only by the Dev inspection
// observer's OnShaderPrepared() (dev/dev_inspection.*).
extern std::atomic<std::uint64_t> g_discard_shader_count;
extern std::atomic<std::uint64_t> g_strict_spatial_dither_count;
extern std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count;
extern std::atomic<std::uint64_t> g_fade_primitive_shader_count;
extern std::atomic<std::uint64_t> g_fade_primitive_instance_count;

// Comma-separated, de-duplicated list of a verified Fade Primitive v1
// instance list's consumer kinds, for display only.
std::string FadePrimitiveConsumers(
    const std::vector<wuwa_tfr::FadePrimitiveInstance>& instances);

// Set from the Dev variant's InitializeVariant() (WuwaTFR.ini's DumpPath).
// Read by both the shader-capture subsystem and the trace TSV exporters.
extern std::filesystem::path g_dump_path;
std::filesystem::path DumpDir();

std::string Hex64(std::uint64_t value);

}  // namespace wuwa_tfr::dev

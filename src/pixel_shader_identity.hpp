// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The single canonical implementation for identifying the DXIL pixel shader
// (and its content hash) among a pipeline's creation subobjects. Extracted
// unchanged from fade_primitive_runtime.cpp: Production's own DXBC/DXIL
// container validation and FNV-1a hash, now shared by both the Production
// and Dev FadePrimitiveRuntime and Dev trace's own pipeline identification,
// so there is exactly one place that decides what counts as "the" DXIL
// pixel shader.

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstddef>
#include <cstdint>

namespace wuwa_tfr {

// Scans a pipeline's creation subobjects for the single DXIL pixel shader.
// Clears `shader`/`hash` on entry. A subobject whose bytecode is not a
// well-formed DXBC container carrying a DXIL chunk is silently ignored, not
// treated as a failure. A repeated DXIL pixel-shader subobject is accepted
// only when every occurrence hashes identically; differing hashes fail the
// whole call (returns false, `shader` left null).
bool FindDxilPixelShader(std::uint32_t count,
    const reshade::api::pipeline_subobject* subobjects,
    const reshade::api::shader_desc*& shader, std::uint64_t& hash);

}  // namespace wuwa_tfr
#endif

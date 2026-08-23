// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, read-only observation of Fade Primitive v1 matches. This module
// owns no replacement state: the sole fade-primitive replacement owner is
// wuwa_tfr::FadePrimitiveRuntime (see dev/dev_runtime.hpp). Everything here
// only queries the always-compiled shader-inspection cache
// (production/addon_shared.hpp) for display purposes.

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

// Pure display data for one shader hash's Fade Primitive v1 match. Carries
// no replacement-lifecycle fields.
struct FadePrimitiveDiagnosticSummary {
  std::uint32_t verified_instance_count = 0;
  std::string consumers;
};

std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic);

std::optional<FadePrimitiveDiagnosticSummary> VerifiedFadePrimitiveTarget(
    std::uint64_t shader_hash);

// Cosmetic-only per-hash highlight set for the diagnostics table below in
// dev/dev_overlay.cpp. This has no effect on replacement correctness:
// FadePrimitiveRuntime evaluates every observed DXIL pixel shader itself and
// has no per-hash selection concept to plug this into.
extern std::mutex g_fade_primitive_diagnostics_mutex;
extern std::unordered_map<std::uint64_t, bool>
    g_fade_primitive_highlighted_hashes;

void ImportCurrentCapturedPsosIntoHighlights();
void SetFadePrimitiveHighlighted(std::uint64_t shader_hash, bool highlighted);
void ClearFadePrimitiveHighlights();

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The Dev-owned canonical-evidence cache. FadeInstanceObservation and
// InspectionRecord are populated exclusively from facts the shared
// FadePrimitiveRuntime (fade_primitive_runtime.cpp/.hpp) already computes on
// its own canonical shader-preparation and pipeline-init path, delivered
// through the read-only FadePrimitiveRuntimeObserver seam
// (fade_primitive_runtime_observer.hpp) that InspectionObserver() implements
// and dev_runtime.cpp's InitializeDevRuntime() installs on
// g_dev_antifade_runtime. This module runs no DXC of its own and no
// independent Fade Primitive, pre-Fade, or gate-predicate matcher/analyzer;
// the only analysis it still performs synchronously, on the observation's
// original_ir, is genuinely Dev-only (spatial/dither diagnostics, the
// shader-dump file write) plus diagnostic (space, register) enrichment of
// canonical evidence the matcher already extracted -- see OnShaderPrepared's
// own comment for exactly which calls those are.
//
// DXIL pixel-shader identification is not this module's concern either: it
// is delivered here as an already-resolved hash on the observation, and
// trace's own independent identification calls the same canonical
// implementation in pixel_shader_identity.hpp.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <reshade.hpp>

#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"
#include "fade_primitive_runtime_observer.hpp"
#include "pre_fade_fmin_analysis.hpp"

namespace wuwa_tfr::dev {

// One verified Fade Primitive instance, merged with the canonical pre-Fade
// evidence for it when Production's patch reached one -- see
// TargetDitherBypassResult::instance_evidence's own documented semantics for
// exactly when pre_fade is, and is not, present. Never fabricated: an
// instance that was verified but never reached a Matched pre-Fade analysis
// (e.g. the shader failed closed on a later instance, or on a later
// structural check) carries std::nullopt here, never a synthesized failure.
// `instance.gate_predicate` and (when pre_fade is present)
// `pre_fade->operand_one`/`operand_two` are enriched in place with
// (space, register), when resolvable, by OnShaderPrepared -- see its own
// comment.
struct FadeInstanceObservation {
  wuwa_tfr::FadePrimitiveInstance instance;
  std::optional<wuwa_tfr::PreFadeFMinAnalysis> pre_fade;
};

// One entry per unique observed DXIL pixel shader, built entirely from one
// FadePrimitiveRuntimeObserver::ShaderPreparationObservation plus the
// genuinely Dev-only analyses this module runs on that observation's
// original_ir. Every stage outcome below is independent of the others; none
// is inferred from another, and none is fabricated when its own upstream
// stage did not reach it.
struct InspectionRecord {
  // ---- from the canonical ShaderPreparationObservation, unmodified except
  // for each instance's own diagnostic (space, register) enrichment ----
  bool inspection_succeeded = false;
  std::string inspection_error;
  std::size_t bytecode_size = 0;
  // One entry per wuwa_tfr::FadePrimitiveDiagnostic::instances element from
  // the observation, same order. The runtime sampling channel
  // (dev/capture/fade_control_runtime.cpp) reads gate-predicate and
  // pre-Fade-operand sources directly from here -- there is no separate
  // control-source vector.
  std::vector<FadeInstanceObservation> fade_instances;
  bool patch_succeeded = false;
  std::string patch_failure;
  bool prepared_succeeded = false;
  std::string prepared_failure;

  // ---- genuinely Dev-only, computed synchronously from original_ir ----
  wuwa_tfr::SpatialDitherDiagnostic dither;
  bool dumped = false;
};

extern std::mutex g_inspection_mutex;
extern std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;

extern std::atomic<std::uint64_t> g_seen_shader_callbacks;
extern std::atomic<std::uint64_t> g_unique_dxil_shaders;
extern std::atomic<std::uint64_t> g_disassembly_successes;
extern std::atomic<std::uint64_t> g_disassembly_failures;
extern std::atomic<std::uint64_t> g_dumped_shaders;

// Read from WuwaTFR.ini's [General] Diagnostic/Dump keys (or their
// WUWA_TFR_DIAGNOSTIC/WUWA_TFR_DUMP environment-variable fallbacks) by
// InitializeInspectionConfig(). g_diagnostic_config_flag gates no behavior
// in this module: Dev inspection is populated for shaders that reach the
// canonical FadePrimitiveRuntime preparation path (see OnShaderPrepared()
// below), independent of this flag. Retained only so the ini/environment
// key keeps parsing and LogVariantStartup() can echo its value.
extern bool g_diagnostic_config_flag;
extern bool g_dump;

// The Dev-owned FadePrimitiveRuntimeObserver implementation: its
// OnShaderPrepared()/OnPipelineInit() overrides are the sole writers of
// g_inspections and the counters above. Returns the address of a function-
// local static singleton; dev_runtime.cpp's InitializeDevRuntime() installs
// it on g_dev_antifade_runtime before any device can activate.
wuwa_tfr::FadePrimitiveRuntimeObserver* InspectionObserver();

// Called from the Dev variant's InitializeVariant(), before any event can
// fire.
void InitializeInspectionConfig();

}  // namespace wuwa_tfr::dev

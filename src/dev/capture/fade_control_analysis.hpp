// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, diagnostic-only static analysis: for each fully verified Fade
// Primitive v1 instance (fade_primitive_detector.hpp), conservatively
// identify which cbuffer value -- if any, and only when structurally
// provable -- directly feeds (1) the predicate deciding whether the fade
// arm is entered, and (2) the non-identity coverage/fade value itself.
//
// This is independent, self-contained IR-scanning code: it deliberately
// does NOT reuse or modify fade_primitive_detector.cpp/.hpp (compiled into
// Production) so that Dev-only tracing requirements never contaminate the
// Production matcher. It never affects AnalyzeFadePrimitiveV1() or patch
// eligibility; it exists to answer one investigation question and nothing
// else.
//
// Supported DXIL form (see fade_control_analysis.cpp's top comment for the
// exact, currently-recognized pattern): a single literal-indexed
// dx.op.cbufferLoadLegacy.f32 call, reached from the handle produced by a
// single literal dx.op.createHandle(resourceClass=CBV) call, consumed by
// exactly one extractvalue component, resolved to space/register via the
// module's !dx.resources CBuffer metadata list. Every other DXIL resource
// or load form (SM6.6 dynamic-resource binding, non-legacy cbufferLoad,
// dynamically indexed rows/ranges, multiple candidate cbuffer roots)
// deliberately fails closed to "unresolved" rather than guessing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr::dev {

// A single, structurally proven (or not) cbuffer value identity. Never a
// runtime value -- see fade_control_state.hpp/fade_control_runtime.* for
// how a resolved source is combined with a live CBV binding and sampled.
struct FadeControlSource {
  bool resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
  // The cbufferLoadLegacy "row" operand: a 16-byte-aligned vector slot
  // inside the cbuffer, not a byte offset.
  std::uint32_t vector_index = 0;
  // Which 4-byte lane of that 16-byte vector: 0=x, 1=y, 2=z, 3=w.
  std::uint32_t component = 0;

  friend bool operator==(const FadeControlSource&, const FadeControlSource&) =
      default;
};

struct FadeControlInstanceSources {
  FadeControlSource predicate;
  FadeControlSource coverage;
};

// One entry per wuwa_tfr::FadePrimitiveDiagnostic::instances element, same
// order -- the entry's index is that instance's primitive_index elsewhere
// in this feature.
std::vector<FadeControlInstanceSources> AnalyzeFadeControlSources(
    const std::string& llvm_ir,
    const wuwa_tfr::FadePrimitiveDiagnostic& fade_primitive);

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wuwa_tfr {

enum class FadePrimitiveConsumer : std::uint8_t {
  Unknown,
  Discard,
  SvTargetAlpha,
  SvTargetRgb,
  DiscardAndSvTargetAlpha,
  OtherVisibilityOrOutput,
};

// Best-effort, read-only evidence about a verified instance's gate-branch
// predicate: the CBV-controlled condition the matcher's own boolean gate
// criterion already proves exists (see fade_primitive_detector.cpp's
// VerifyCbufferControlledGate). Extracted from the exact same gate branch
// and backward slice that boolean check itself used -- never a second,
// independent analysis pass -- and never a matching criterion of its own:
// an instance is verified purely by the existing boolean semantics,
// unaffected by whether this evidence resolves.
//
// `resolved` -- a single, unambiguous direct scalar CBV load (a bare
// dx.op.cbufferLoad.f32 call, or a dx.op.cbufferLoadLegacy.f32 call
// consumed by exactly one extractvalue, behind a literally-typed
// createHandle(resourceClass=CBV)) reachable in the gate condition's
// backward slice -- is the only field that is ever definitively true or
// false. Every coordinate below it is diagnostic-only enrichment that may
// legitimately stay unavailable (a dynamically indexed row, for instance)
// without disqualifying an otherwise resolved source. Zero or more than
// one candidate load leaves `resolved` false: ambiguity is never guessed
// through, mirroring pre_fade_fmin_analysis.hpp's PreFadeOperandSource
// contract.
struct FadePrimitiveGatePredicateEvidence {
  // The gate branch's own condition SSA value, when the branch gating
  // entry into the enabled arm was itself unambiguous. Independent of
  // `resolved` below -- the branch can be identified even when its
  // condition does not resolve to a direct CBV load.
  bool condition_identified = false;
  std::string condition_value;

  bool resolved = false;      // structural: a direct scalar CBV load
  std::string handle_value;   // structural: SSA name of the %dx.types.Handle
  bool legacy_form = false;   // structural: legacy row form vs byte form

  // The createHandle's own literal range id -- raw material for a later,
  // separate !dx.resources walk (pre_fade_fmin_analysis.hpp's
  // ResolveGatePredicateCbvRegister). Never resolved to (space, register)
  // here -- that walk costs several passes over the whole IR text and must
  // never enter the matcher's own acceptance path.
  bool range_id_resolved = false;
  std::uint32_t range_id = 0;

  // Diagnostic-only, and populated only by an explicit
  // pre_fade_fmin_analysis.hpp::ResolveGatePredicateCbvRegister() call --
  // never by the canonical matcher, which does not walk module metadata.
  // Mirrors PreFadeOperandSource::register_resolved.
  bool register_resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;

  bool row_resolved = false;          // diagnostic-only, legacy form
  std::uint32_t row = 0;
  bool component_resolved = false;    // diagnostic-only, legacy form
  std::uint32_t component = 0;
  bool byte_offset_resolved = false;  // diagnostic-only, byte form
  std::uint32_t byte_offset = 0;
};

struct FadePrimitiveInstance {
  FadePrimitiveConsumer consumer = FadePrimitiveConsumer::Unknown;
  // An SSA name is local to a defined LLVM function.  The source range is the
  // exact original phi line verified by the matcher and is the patch target.
  std::string function_identity;
  std::size_t phi_start = 0;
  std::size_t phi_end = 0;
  std::string merge_value;
  // Populated for every verified instance (the matcher's own boolean gate
  // criterion already requires a CBV-controlled gate to reach this point);
  // never itself a matching criterion -- see
  // FadePrimitiveGatePredicateEvidence's own comment for exactly what
  // `resolved == false` here does and does not mean.
  FadePrimitiveGatePredicateEvidence gate_predicate;
};

struct FadePrimitiveDiagnostic {
  std::vector<FadePrimitiveInstance> instances;
};

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir);

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept;

} // namespace wuwa_tfr

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Canonical structural analysis of the pre-Fade combination that feeds an
// already-verified Fade Primitive v1 instance's non-identity (enabled) path.
// This is the single, shared source of truth for that analysis: both the
// Production patch (target_dither_bypass.cpp) and Dev's read-only reporting
// call into it. Neither owns a separate parser/graph implementation for this
// structure.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr {

enum class PreFadeAdjacency { Unknown, SameRow, CrossRow, NonAdjacent };

// A single FMin operand's resolved origin. `resolved` is the only field that
// participates in a Production matching decision; every other field
// (handle_value, register/space, row/component/byte_offset, adjacency in the
// enclosing PreFadeFMinAnalysis) is diagnostic-only, for Dev reporting and
// for the audit tool, and must never become a Production matching criterion.
struct PreFadeOperandSource {
  bool resolved = false;
  std::string handle_value;   // SSA name of the %dx.types.Handle, diagnostic-only
  bool legacy_form = false;
  std::uint32_t row = 0;         // legacy form: cbufferLoadLegacy row index
  std::uint32_t component = 0;   // legacy form: extractvalue component (0..3)
  std::uint32_t byte_offset = 0; // byte form: cbufferLoad byte offset
  bool register_resolved = false;  // best-effort, via !dx.resources; diagnostic-only
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
  // The exact operand token's byte range in the analyzed IR string, and its
  // exact text -- what a patch rewrites and revalidates against.
  std::size_t source_start = 0;
  std::size_t source_end = 0;
  std::string source_text;
};

struct PreFadeFMinAnalysis {
  // True only when exactly one qualifying FMin was found in a complete
  // backward slice and both its operands resolved directly to scalar loads
  // from the same constant-buffer handle.
  bool success = false;
  bool backward_slice_complete = false;
  std::size_t qualifying_fmin_count = 0;
  std::string fmin_result_identity;  // the FMin's own SSA name; diagnostic-only
  PreFadeOperandSource operand_one;  // the rewrite target on success
  PreFadeOperandSource operand_two;
  PreFadeAdjacency adjacency = PreFadeAdjacency::Unknown;  // diagnostic-only
  // The fail-closed reason when success == false: absent, ambiguous,
  // incomplete slice, unresolved operand, mismatched handle, or an
  // inconsistent function/source identity.
  std::string error;
};

// Independently re-locates `instance.function_identity`'s block in
// `llvm_ir`, re-derives the enabled (non-identity) phi arm from
// `instance.phi_start/phi_end/merge_value` (cross-checked, never trusted
// from a prior pass), takes its complete backward slice, and searches that
// slice for the unique FMin (dx.op.binary.f32 opcode 36) whose two operands
// both resolve *directly* -- no bitcast/phi/select copy-chain -- to scalar
// loads (cbufferLoadLegacy.f32's extractvalue, or a bare cbufferLoad.f32)
// from the same constant-buffer handle.
//
// Fails closed (success == false, `error` set) on: no qualifying FMin, more
// than one qualifying FMin, an incomplete backward slice, an operand that
// cannot be structurally resolved, operands from different CBV handles, or
// an inconsistent/invalid function or phi source identity. Never uses a
// shader hash, a fixed CBV register/row/component, adjacency as a
// requirement, a runtime CBV read, or per-Draw logic.
PreFadeFMinAnalysis AnalyzePreFadeFMinForInstance(
    const std::string& llvm_ir, const FadePrimitiveInstance& instance);

}  // namespace wuwa_tfr

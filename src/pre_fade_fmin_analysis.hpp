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
#include <string_view>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr {

// The exact text operand 1 of a matched pre-Fade FMin is rewritten to, and
// the exact text post-patch verification requires to find there. Declared
// once so the patch and its proof can never drift apart.
inline constexpr std::string_view kPreFadeRewriteLiteral = "1.000000e+00";

enum class PreFadeAdjacency { Unknown, SameRow, CrossRow, NonAdjacent };

// The outcome of one instance's analysis, as a structured status rather than
// a bare bool -- post-patch verification has to tell "the structure was fully
// re-derived and the qualifying candidate set is exactly empty" apart from
// "the analysis itself failed and therefore proves nothing".
//
// The first three values all imply the whole structural chain succeeded:
// instance identity re-derived, function uniquely located, function parsed,
// backward slice complete. Only then is `qualifying_fmin_count` a trustworthy
// census. The remaining values mean the analysis aborted before that point.
enum class PreFadeFMinStatus : std::uint8_t {
  // Exactly one qualifying FMin. `operand_one`/`operand_two` are valid.
  Matched,
  // Structurally complete, and the qualifying candidate set is exactly empty.
  // This is the only status that positively proves absence.
  NoQualifyingCandidate,
  // Structurally complete, but more than one candidate qualified: ambiguous.
  AmbiguousCandidates,
  // ---- structural failures: these prove nothing, in either direction ----
  // The instance's function/merge/phi source identity did not re-derive.
  InvalidInstanceIdentity,
  // The instance's function could not be uniquely located.
  FunctionNotUniquelyLocated,
  // The located function could not be parsed (e.g. duplicate SSA definitions).
  FunctionNotParsable,
  // The backward slice hit the traversal limit and is incomplete.
  IncompleteBackwardSlice,
};

const char* PreFadeFMinStatusName(PreFadeFMinStatus status) noexcept;

// A single FMin operand's resolved origin.
//
// `resolved` -- the operand directly loads a scalar from `handle_value`'s
// constant buffer -- is, together with the two operands' handles being equal,
// the ONLY part of this struct that participates in a Production matching
// decision.
//
// Every coordinate below it (row, component, byte_offset, register/space, and
// the adjacency classification in the enclosing PreFadeFMinAnalysis) is
// diagnostic enrichment for Dev reporting and the offline audit. Each carries
// its own `*_resolved` flag and each may legitimately be unavailable -- a
// dynamically indexed row, for instance -- without that making an otherwise
// valid direct same-CBV source fail to match. None of them may ever become a
// Production matching criterion.
struct PreFadeOperandSource {
  bool resolved = false;      // structural: a direct scalar CBV load
  std::string handle_value;   // structural: SSA name of the %dx.types.Handle
  bool legacy_form = false;   // structural: legacy row form vs byte form
  bool row_resolved = false;         // diagnostic-only
  std::uint32_t row = 0;             // legacy form: cbufferLoadLegacy row index
  bool component_resolved = false;   // diagnostic-only
  std::uint32_t component = 0;       // legacy form: extractvalue component (0..3)
  bool byte_offset_resolved = false; // diagnostic-only
  std::uint32_t byte_offset = 0;     // byte form: cbufferLoad byte offset
  // Diagnostic-only, and populated only by an explicit
  // ResolvePreFadeCbvRegisters() call -- never by the canonical analysis,
  // which does not walk module metadata.
  bool register_resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
  // The exact operand token's byte range in the analyzed IR string, and its
  // exact text -- what a patch rewrites and revalidates against.
  std::size_t source_start = 0;
  std::size_t source_end = 0;
  std::string source_text;
};

struct PreFadeFMinAnalysis {
  PreFadeFMinStatus status = PreFadeFMinStatus::InvalidInstanceIdentity;
  // Convenience alias for status == Matched: exactly one qualifying FMin in a
  // complete backward slice, both operands resolving directly to scalar loads
  // from the same constant-buffer handle.
  bool success = false;
  // Each stage of the structural chain, recorded independently so that a
  // caller can require the exact expected state instead of inferring it.
  bool instance_identity_verified = false;  // enabled arm re-derived
  bool function_located = false;
  bool function_parsed = false;
  bool backward_slice_complete = false;
  // Trustworthy only when the whole structural chain above succeeded; see
  // PreFadeFMinAnalysisIsStructurallyComplete().
  std::size_t qualifying_fmin_count = 0;
  std::string fmin_result_identity;  // the FMin's own SSA name; diagnostic-only
  PreFadeOperandSource operand_one;  // the rewrite target on success
  PreFadeOperandSource operand_two;
  PreFadeAdjacency adjacency = PreFadeAdjacency::Unknown;  // diagnostic-only
  // Human-readable detail for `status`; for reporting only. Callers must
  // branch on `status`, never on this text.
  std::string error;
};

// True only when instance identity re-derivation, function location, function
// parsing and the backward slice all succeeded, so `qualifying_fmin_count` is
// a trustworthy census of the qualifying candidate set.
bool PreFadeFMinAnalysisIsStructurallyComplete(
    const PreFadeFMinAnalysis& analysis) noexcept;

// True only when the analysis is structurally complete AND the qualifying
// candidate set is exactly empty. This -- not `!success` -- is what proves a
// qualifying pre-Fade FMin is absent; an analysis that merely failed proves
// nothing and must never be accepted as evidence of absence.
bool PreFadeFMinProvesNoQualifyingCandidate(
    const PreFadeFMinAnalysis& analysis) noexcept;

// Diagnostic enrichment, for Dev reporting and the offline audit only: fills
// in `operand_one`/`operand_two`'s cbuffer_space/cbuffer_register from
// !dx.resources when they can be resolved unambiguously, leaving
// `register_resolved` false when they cannot.
//
// Deliberately not part of AnalyzePreFadeFMinForInstance: it walks module
// metadata, which costs several passes over the whole IR text and dominates
// the analysis on large shaders, while contributing nothing Production reads.
// `analysis` must be a Matched result for `instance` in `llvm_ir`; anything
// else leaves it untouched.
void ResolvePreFadeCbvRegisters(const std::string& llvm_ir,
    const FadePrimitiveInstance& instance, PreFadeFMinAnalysis& analysis);

// Post-patch proof that the intended rewrite -- and only it -- happened.
// `matched` must be a Matched analysis of the *pre-patch* IR, and
// `patched_instance` the same Fade Primitive instance re-detected in
// `patched_llvm_ir`. Re-locates that instance's function in the patched IR
// and requires that `matched.fmin_result_identity` still names an FMin
// there, that its operand 1 is now exactly kPreFadeRewriteLiteral, and that
// its operand 2 is byte-identical to the operand 2 recorded before the patch.
//
// This is what distinguishes "the intended FMin was rewritten" from "the old
// candidate merely stopped being recognized": a rewrite that landed on some
// other token would leave this FMin unchanged and fail here, even in the case
// where the qualifying candidate set became empty as a side effect.
bool VerifyPreFadeFMinOperandOneRewritten(const std::string& patched_llvm_ir,
    const FadePrimitiveInstance& patched_instance, const PreFadeFMinAnalysis& matched,
    std::string& error);

// Independently re-locates `instance.function_identity`'s block in
// `llvm_ir`, re-derives the enabled (non-identity) phi arm from
// `instance.phi_start/phi_end/merge_value` (cross-checked, never trusted
// from a prior pass), takes its complete backward slice, and searches that
// slice for the unique FMin (dx.op.binary.f32 opcode 36) whose two operands
// both resolve *directly* -- no bitcast/phi/select copy-chain -- to scalar
// loads (cbufferLoadLegacy.f32's extractvalue, or a bare cbufferLoad.f32)
// from the same constant-buffer handle.
//
// Fails closed on: no qualifying FMin, more than one qualifying FMin, an
// incomplete backward slice, an operand that cannot be structurally
// resolved, operands from different CBV handles, or an inconsistent/invalid
// function or phi source identity -- each reported as a distinct
// PreFadeFMinStatus. Never uses a shader hash, a fixed CBV
// register/row/component, adjacency as a requirement, a runtime CBV read, or
// per-Draw logic.
PreFadeFMinAnalysis AnalyzePreFadeFMinForInstance(
    const std::string& llvm_ir, const FadePrimitiveInstance& instance);

}  // namespace wuwa_tfr

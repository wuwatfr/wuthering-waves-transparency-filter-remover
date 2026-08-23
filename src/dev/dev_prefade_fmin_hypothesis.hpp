// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// EXPERIMENTAL, Dev-only investigation hypothesis. Not part of the verified
// Fade Primitive v1 detector or its Production patch (target_dither_bypass).
// This module never runs in the Production build and is never authorized to
// change the shipped identity-phi behavior.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "target_dither_bypass.hpp"

namespace wuwa_tfr::dev {

// For every already v1-verified Fade Primitive instance in
// `original_llvm_ir`, independently re-locates the unique pre-Fade FMin in
// that instance's non-identity (enabled) backward slice whose two operands
// both resolve *directly* -- no bitcast/phi/select copy-chain -- to scalar
// loads from the same constant buffer handle (cbufferLoadLegacy.f32's
// extractvalue, or cbufferLoad.f32 directly), and rewrites only that FMin's
// first operand to the literal 1.000000e+00. Operand 2 and every other
// instruction, including the phi itself, are left byte-identical.
//
// This is a hypothesis probe, not the Production identity-phi collapse:
// PatchAllVerifiedFadePrimitiveInstancesToIdentity is never called here, and
// the verified phi/gate/coverage/consumer structure is required to still be
// present and unchanged after the rewrite.
//
// Fails closed (returns success=false, no IR emitted, no partial rewrite) if
// any verified instance's qualifying FMin is absent, ambiguous (more than
// one candidate), unresolvable (either operand cannot be structurally
// revalidated), or if the instance's backward slice is incomplete. All
// instances in one shader are patched atomically: any single failure aborts
// the whole shader.
//
// Never uses a shader hash, a fixed CBV register/row/component, a runtime
// CBV read, or per-Draw logic -- purely structural, from the same
// already-verified phi's source range outward.
TargetDitherBypassResult PatchPreFadeFMinOperandOneHypothesis(
    const std::string& original_llvm_ir);

// Cumulative, process-lifetime diagnostics for the hypothesis patch above.
// Every field accumulates across every call to
// PatchPreFadeFMinOperandOneHypothesis on this process (both from the
// runtime plumbing and from tests, when tests run in-process).
struct PreFadeFMinHypothesisDiagnostics {
  // Shaders passed to the hypothesis patch that had at least one v1-verified
  // Fade Primitive instance (i.e. reached structural evaluation at all).
  std::uint64_t shaders_evaluated_total = 0;
  // Sum of AnalyzeFadePrimitiveV1's verified instance count, across every
  // evaluated shader.
  std::uint64_t verified_instances_total = 0;
  // Sum, across every evaluated instance, of instances that had exactly one
  // qualifying pre-Fade FMin (same-CBV, both operands direct) at the time of
  // structural verification.
  std::uint64_t qualifying_instances_total = 0;
  // Sum of instances actually rewritten (only nonzero on shaders where every
  // verified instance qualified and the whole shader's patch succeeded).
  std::uint64_t patched_instances_total = 0;
  // Shaders where the hypothesis patch failed closed for any reason.
  std::uint64_t shaders_failed_total = 0;
};

PreFadeFMinHypothesisDiagnostics PreFadeFMinHypothesisDiagnosticsSnapshot();

// The `error` string from the most recent failed call, or an empty string if
// none has failed yet (or the process has never called the patch). Cleared
// only by a subsequent failure; a later success does not clear it, so the
// last fail-closed reason stays visible for review after the fact.
std::string LastPreFadeFMinHypothesisFailureReason();

// Test-only: resets every counter and the last-failure string to their
// initial state. Never called by runtime/production code paths.
void ResetPreFadeFMinHypothesisDiagnosticsForTest();

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"

namespace wuwa_tfr {

// One verified instance's already-computed canonical pre-Fade evidence: the
// exact FadePrimitiveInstance it was matched against, paired with the exact
// PreFadeFMinAnalysis that authorized (or, for a not-yet-rewritten instance
// exposed mid-failure, would have authorized) its rewrite. Read-only
// diagnostic data -- this is the same analysis
// PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand() itself already
// computed and acted on, copied out rather than re-derived. Association with
// the instance is carried directly by value in `instance`, not by a shared
// index into some other vector, so it survives independently of ordering.
struct PreFadeFMinEvidence {
  FadePrimitiveInstance instance;
  PreFadeFMinAnalysis analysis;
};

struct TargetDitherBypassResult {
  bool success = false;
  bool structural_verification_succeeded = false;
  bool ir_patch_succeeded = false;
  std::size_t verified_instance_count = 0;
  // Count of instances whose unique qualifying pre-Fade FMin was found and
  // structurally revalidated (see pre_fade_fmin_analysis.hpp). Equal to
  // patched_instance_count on success; on failure it reflects how many
  // instances got that far before the first fail-closed instance, and is 0
  // for a whole-shader failure detected earlier (e.g. non-unique phi
  // identities).
  std::size_t qualifying_instance_count = 0;
  std::size_t patched_instance_count = 0;
  std::string llvm_ir;
  std::string error;

  // The already-computed evidence behind every instance that reached a
  // Matched pre-Fade analysis on the way to this result -- i.e. exactly the
  // instances staged for rewrite, each paired with the analysis that
  // authorized it. Exposed purely for read-only diagnostic consumers (see
  // FadePrimitiveRuntimeObserver in fade_primitive_runtime_observer.hpp);
  // this function never re-derives an analysis to populate it, and never
  // calls ResolvePreFadeCbvRegisters() -- register/space enrichment stays
  // diagnostic-only and outside this cost.
  //
  // On success, one entry per patched instance (size() == patched_instance_
  // count).
  //
  // On failure, this is exactly whichever prefix of that same set had
  // already reached a Matched analysis before the fail-closed rejection:
  // empty for a whole-shader failure detected before any instance was
  // analyzed (no verified instance, an unsupported consumer, or non-unique
  // instance identities), a strict prefix for a failure discovered while
  // still analyzing instances (an instance whose own analysis was not
  // Matched), or the complete matched set for a failure discovered only
  // after every instance was analyzed (two instances sharing one rewrite
  // range, a rewrite target that changed before patching, or any part of
  // post-patch verification). An instance whose own analysis was not Matched
  // is never included here, even on the failing shader: an unmatched
  // analysis is not patch evidence, and this vector never fabricates any.
  std::vector<PreFadeFMinEvidence> instance_evidence;
};

// The canonical Production patch. For every already v1-verified Fade
// Primitive instance with a supported Production consumer, independently
// re-locates and revalidates (via pre_fade_fmin_analysis.hpp) the unique
// pre-Fade FMin in that instance's non-identity backward slice whose two
// operands both resolve directly to scalar loads from the same constant-
// buffer handle, and rewrites only that FMin's first operand to
// 1.000000e+00. Operand 2 and every other instruction -- the phi itself, the
// gate, the coverage/dither expression, and the discard/output consumer --
// are left byte-identical.
//
// The caller must separately restrict the shader set; this function changes
// every and only every v1-verified instance's qualifying FMin in one shader.
//
// Post-patch it then proves, for every instance -- matched back by stable
// identity (function, merge SSA name, consumer), never by vector order or by
// byte offset -- all of: the instance is still present and classified
// identically; the canonical analysis still re-derives its enabled arm, still
// uniquely locates and parses its function, and still completes its backward
// slice; the qualifying candidate count is now exactly zero; and the FMin
// that was targeted still exists with operand 1 now the literal and operand 2
// byte-identical. An analysis that merely *fails* post-patch is never
// accepted as evidence: it proves nothing in either direction.
//
// Fails closed (returns success == false, no IR emitted) on: no verified
// instance, a non-unique instance source identity, an unsupported consumer, a
// missing/ambiguous/unresolvable qualifying FMin, an incomplete backward
// slice, two instances sharing one rewrite range, a rewrite target that
// changed before patching, or any part of the post-patch proof above not
// holding.
TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
    const std::string& original_llvm_ir);

} // namespace wuwa_tfr

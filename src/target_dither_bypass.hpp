// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <string>

namespace wuwa_tfr {

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

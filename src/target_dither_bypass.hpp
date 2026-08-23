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
// every and only every v1-verified instance's qualifying FMin in one shader,
// then verifies structurally that none remain resolvable post-patch.
//
// Fails closed (returns success == false, no IR emitted) on: no verified
// instance, a non-unique phi source identity, an unsupported consumer, a
// missing/ambiguous/unresolvable qualifying FMin, an incomplete backward
// slice, a rewrite target that changed before patching, or a post-patch
// structural re-verification failure.
TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
    const std::string& original_llvm_ir);

} // namespace wuwa_tfr

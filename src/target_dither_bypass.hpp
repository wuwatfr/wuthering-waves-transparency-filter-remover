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
  bool stage1_structural_verification_succeeded = false;
  bool stage1_ir_patch_succeeded = false;
  bool stage2_structural_verification_succeeded = false;
  bool stage2_ir_patch_succeeded = false;
  std::size_t verified_instance_count = 0;
  std::size_t patched_instance_count = 0;
  std::string llvm_ir;
  std::string error;
};

// This is deliberately a one-shader experimental rewrite, not a matcher. Its
// caller is responsible for restricting it to the selected target hash.
TargetDitherBypassResult PatchSelectedTargetDitherToIdentity(
    const std::string& original_llvm_ir);

// This is deliberately restricted to the one selected special-material
// experiment that has two independently verified dither identity merges.
TargetDitherBypassResult PatchSelectedDualDitherStagesToIdentity(
    const std::string& original_llvm_ir);

// The caller must separately restrict
// the shader set; this function changes every and only every v1-verified
// merge in one shader, then verifies that none remain.
TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesToIdentity(
    const std::string& original_llvm_ir);

} // namespace wuwa_tfr

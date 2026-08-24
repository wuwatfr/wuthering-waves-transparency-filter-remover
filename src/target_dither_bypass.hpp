// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"

namespace wuwa_tfr {

struct PreFadeFMinEvidence {
  FadePrimitiveInstance instance;
  PreFadeFMinAnalysis analysis;
};

struct TargetDitherBypassResult {
  bool success = false;
  bool structural_verification_succeeded = false;
  bool ir_patch_succeeded = false;
  std::size_t verified_instance_count = 0;
  std::size_t qualifying_instance_count = 0;
  std::size_t patched_instance_count = 0;
  std::string llvm_ir;
  std::string error;
  std::vector<PreFadeFMinEvidence> instance_evidence;
};

TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
    const std::string& original_llvm_ir);

}

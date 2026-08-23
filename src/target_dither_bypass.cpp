// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr {
namespace {

bool IsProductionConsumer(FadePrimitiveConsumer consumer) noexcept {
  return consumer == FadePrimitiveConsumer::Discard ||
      consumer == FadePrimitiveConsumer::SvTargetAlpha ||
      consumer == FadePrimitiveConsumer::SvTargetRgb ||
      consumer == FadePrimitiveConsumer::DiscardAndSvTargetAlpha;
}

struct PendingRewrite {
  std::size_t start = 0;
  std::size_t end = 0;
  std::string expected_text;
};

} // namespace

TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
    const std::string& original_llvm_ir) {
  TargetDitherBypassResult result;
  const FadePrimitiveDiagnostic diagnostic = AnalyzeFadePrimitiveV1(original_llvm_ir);
  if (diagnostic.instances.empty()) {
    result.error = "no verified transparency-filter primitive instance";
    return result;
  }
  result.verified_instance_count = diagnostic.instances.size();

  std::vector<PendingRewrite> rewrites;
  rewrites.reserve(diagnostic.instances.size());
  std::unordered_set<std::size_t> phi_starts_seen;

  for (const FadePrimitiveInstance& instance : diagnostic.instances) {
    if (!IsProductionConsumer(instance.consumer)) {
      result.error = "verified primitive has an unsupported production consumer";
      return result;
    }
    if (!phi_starts_seen.insert(instance.phi_start).second) {
      result.error = "verified primitive instances do not have unique source identities";
      return result;
    }

    const PreFadeFMinAnalysis analysis =
        AnalyzePreFadeFMinForInstance(original_llvm_ir, instance);
    if (!analysis.success) {
      result.error = "verified primitive pre-Fade structure rejected: " + analysis.error;
      return result;
    }
    ++result.qualifying_instance_count;

    PendingRewrite rewrite;
    rewrite.start = analysis.operand_one.source_start;
    rewrite.end = analysis.operand_one.source_end;
    rewrite.expected_text = analysis.operand_one.source_text;
    rewrites.push_back(std::move(rewrite));
  }

  std::sort(rewrites.begin(), rewrites.end(),
      [](const PendingRewrite& left, const PendingRewrite& right) {
        return left.start > right.start;
      });
  result.structural_verification_succeeded = true;
  std::string patched_llvm_ir = original_llvm_ir;
  for (const PendingRewrite& rewrite : rewrites) {
    const std::string_view current(
        patched_llvm_ir.data() + rewrite.start, rewrite.end - rewrite.start);
    if (current != rewrite.expected_text) {
      result.error = "verified pre-Fade FMin operand changed before rewrite";
      return result;
    }
    patched_llvm_ir.replace(rewrite.start, rewrite.end - rewrite.start, "1.000000e+00");
  }

  // Post-patch structural re-verification: the same set of verified
  // instances (by function identity, merge SSA name, and consumer -- raw
  // byte offsets are not compared, since the rewrite's replacement text is a
  // different length than the SSA name it replaces, which shifts every
  // absolute offset located textually after it) must still be present and
  // classify identically, proving nothing downstream moved and the old
  // identity-phi collapse was never triggered; and every instance's
  // qualifying FMin must now be gone, since operand 1 is a literal.
  const FadePrimitiveDiagnostic post_patch_diagnostic =
      AnalyzeFadePrimitiveV1(patched_llvm_ir);
  if (post_patch_diagnostic.instances.size() != diagnostic.instances.size()) {
    result.error = "post-patch verification: downstream Fade Primitive structure changed";
    return result;
  }
  for (std::size_t i = 0; i < diagnostic.instances.size(); ++i) {
    const auto& before = diagnostic.instances[i];
    const auto& after = post_patch_diagnostic.instances[i];
    if (before.function_identity != after.function_identity ||
        before.merge_value != after.merge_value || before.consumer != after.consumer) {
      result.error = "post-patch verification: downstream Fade Primitive structure changed";
      return result;
    }
  }
  for (const FadePrimitiveInstance& instance : post_patch_diagnostic.instances) {
    const PreFadeFMinAnalysis post_patch_analysis =
        AnalyzePreFadeFMinForInstance(patched_llvm_ir, instance);
    if (post_patch_analysis.success) {
      result.error = "post-patch verification: qualifying pre-Fade FMin still present";
      return result;
    }
  }

  result.llvm_ir = std::move(patched_llvm_ir);
  result.ir_patch_succeeded = true;
  result.patched_instance_count = rewrites.size();
  result.success = true;
  return result;
}

} // namespace wuwa_tfr

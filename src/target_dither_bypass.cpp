// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wuwa_tfr {
namespace {

bool IsProductionConsumer(FadePrimitiveConsumer consumer) noexcept {
  return consumer == FadePrimitiveConsumer::Discard ||
      consumer == FadePrimitiveConsumer::SvTargetAlpha ||
      consumer == FadePrimitiveConsumer::SvTargetRgb ||
      consumer == FadePrimitiveConsumer::DiscardAndSvTargetAlpha;
}

// A verified instance's stable identity: the enclosing function, the merge
// phi's function-local SSA name, and the classified consumer. None of the
// three is moved or renamed by an operand rewrite, so the same instance can
// be found again in the patched IR without relying on detector vector
// ordering or on absolute byte offsets -- both of which do shift, since the
// replacement literal has a different length than the SSA name it replaces.
std::string InstanceIdentityKey(const FadePrimitiveInstance& instance) {
  std::string key = instance.function_identity;
  key.push_back('\0');
  key += instance.merge_value;
  key.push_back('\0');
  key.push_back(static_cast<char>(instance.consumer));
  return key;
}

struct PendingRewrite {
  std::string identity_key;
  std::size_t start = 0;
  std::size_t end = 0;
  std::string expected_text;
  PreFadeFMinAnalysis analysis;
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
  std::unordered_set<std::string> identity_keys_seen;

  for (const FadePrimitiveInstance& instance : diagnostic.instances) {
    if (!IsProductionConsumer(instance.consumer)) {
      result.error = "verified primitive has an unsupported production consumer";
      return result;
    }
    std::string identity_key = InstanceIdentityKey(instance);
    if (!phi_starts_seen.insert(instance.phi_start).second ||
        !identity_keys_seen.insert(identity_key).second) {
      result.error = "verified primitive instances do not have unique source identities";
      return result;
    }

    PreFadeFMinAnalysis analysis =
        AnalyzePreFadeFMinForInstance(original_llvm_ir, instance);
    if (analysis.status != PreFadeFMinStatus::Matched) {
      result.error = "verified primitive pre-Fade structure rejected: " + analysis.error;
      return result;
    }
    ++result.qualifying_instance_count;

    PendingRewrite rewrite;
    rewrite.identity_key = std::move(identity_key);
    rewrite.start = analysis.operand_one.source_start;
    rewrite.end = analysis.operand_one.source_end;
    rewrite.expected_text = analysis.operand_one.source_text;
    rewrite.analysis = std::move(analysis);
    rewrites.push_back(std::move(rewrite));
  }

  // Descending, so each replacement's own offsets are still valid when it is
  // applied: every rewrite still pending sits strictly before it in the text.
  std::sort(rewrites.begin(), rewrites.end(),
      [](const PendingRewrite& left, const PendingRewrite& right) {
        return left.start > right.start;
      });
  // Two verified instances resolving to one rewrite range would mean the
  // second rewrite operates on text the first already replaced. Reject that
  // explicitly rather than leaving it to be caught as a stale-text mismatch.
  for (std::size_t i = 1; i < rewrites.size(); ++i) {
    if (rewrites[i].end > rewrites[i - 1].start) {
      result.error = "verified primitive instances share a pre-Fade rewrite range";
      return result;
    }
  }
  result.structural_verification_succeeded = true;

  std::string patched_llvm_ir = original_llvm_ir;
  for (const PendingRewrite& rewrite : rewrites) {
    const std::string_view current(
        patched_llvm_ir.data() + rewrite.start, rewrite.end - rewrite.start);
    if (current != rewrite.expected_text) {
      result.error = "verified pre-Fade FMin operand changed before rewrite";
      return result;
    }
    patched_llvm_ir.replace(
        rewrite.start, rewrite.end - rewrite.start, kPreFadeRewriteLiteral);
  }

  // ---- post-patch structural re-verification ----
  //
  // Every one of these must hold; nothing here is inferred from an analysis
  // merely failing, because a parser failure, an invalid source identity or
  // an incomplete slice is evidence of nothing at all.
  //
  //  1. the same set of verified instances is still present, matched by
  //     stable identity (function, merge SSA name, consumer) rather than by
  //     vector position or byte offset, proving nothing downstream moved and
  //     that no other structural collapse was triggered;
  //  2. for each of them the canonical analysis still re-derives the enabled
  //     arm, still locates and parses the function, still completes the
  //     backward slice, and now finds exactly zero qualifying candidates;
  //  3. and, positively, the FMin that was targeted is still there with
  //     operand 1 now the literal and operand 2 byte-identical -- so a
  //     rewrite that had landed somewhere else could not pass by emptying the
  //     candidate set as a side effect.
  const FadePrimitiveDiagnostic post_patch_diagnostic =
      AnalyzeFadePrimitiveV1(patched_llvm_ir);
  if (post_patch_diagnostic.instances.size() != diagnostic.instances.size()) {
    result.error = "post-patch verification: downstream Fade Primitive structure changed";
    return result;
  }
  std::unordered_map<std::string, const PendingRewrite*> rewrites_by_identity;
  rewrites_by_identity.reserve(rewrites.size());
  for (const PendingRewrite& rewrite : rewrites)
    rewrites_by_identity.emplace(rewrite.identity_key, &rewrite);

  std::unordered_set<std::string> matched_identities;
  for (const FadePrimitiveInstance& instance : post_patch_diagnostic.instances) {
    const std::string identity_key = InstanceIdentityKey(instance);
    const auto found = rewrites_by_identity.find(identity_key);
    if (found == rewrites_by_identity.end() ||
        !matched_identities.insert(identity_key).second) {
      result.error = "post-patch verification: downstream Fade Primitive structure changed";
      return result;
    }
    const PendingRewrite& rewrite = *found->second;

    const PreFadeFMinAnalysis post_patch_analysis =
        AnalyzePreFadeFMinForInstance(patched_llvm_ir, instance);
    if (!PreFadeFMinProvesNoQualifyingCandidate(post_patch_analysis)) {
      result.error =
          "post-patch verification: qualifying pre-Fade FMin absence not proven: " +
          (post_patch_analysis.error.empty()
                  ? std::string(PreFadeFMinStatusName(post_patch_analysis.status))
                  : post_patch_analysis.error);
      return result;
    }

    std::string rewrite_error;
    if (!VerifyPreFadeFMinOperandOneRewritten(
            patched_llvm_ir, instance, rewrite.analysis, rewrite_error)) {
      result.error = "post-patch verification: " + rewrite_error;
      return result;
    }
  }
  if (matched_identities.size() != rewrites.size()) {
    result.error = "post-patch verification: downstream Fade Primitive structure changed";
    return result;
  }

  result.llvm_ir = std::move(patched_llvm_ir);
  result.ir_patch_succeeded = true;
  result.patched_instance_count = rewrites.size();
  result.success = true;
  return result;
}

} // namespace wuwa_tfr

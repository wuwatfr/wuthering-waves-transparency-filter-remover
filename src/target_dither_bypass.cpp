// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr {
namespace {

constexpr std::string_view kThresholdTable = "@DITHER_THRESHOLDS_GLOBAL";
constexpr std::string_view kThresholdAccess =
    "getelementptr inbounds [9 x float], [9 x float]* "
    "@DITHER_THRESHOLDS_GLOBAL";
constexpr std::string_view kDiscard = "call void @dx.op.discard(";
constexpr std::string_view kDitherIdentityArm = "[ 1.000000e+00, ";

std::string_view TrimLeadingWhitespace(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  return first == std::string_view::npos ? std::string_view{} :
      text.substr(first);
}

std::size_t Count(const std::string& text, std::string_view needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  for (std::size_t offset = 0;;) {
    offset = text.find(needle, offset);
    if (offset == std::string::npos) return count;
    ++count;
    offset += needle.size();
  }
}

struct DitherIdentityPhi {
  std::size_t start = std::string::npos;
  std::size_t end = std::string::npos;
};

bool IsSsaNameCharacter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '-' ||
      value == '_' || value == '.' || value == '$';
}

std::vector<std::string> FindSsaValues(std::string_view text) {
  std::vector<std::string> values;
  for (std::size_t offset = 0; offset < text.size(); ++offset) {
    if (text[offset] != '%') continue;
    const std::size_t start = offset;
    ++offset;
    while (offset < text.size() && IsSsaNameCharacter(text[offset])) ++offset;
    values.emplace_back(text.substr(start, offset - start));
    if (offset == text.size()) break;
    // The loop increment advances to the delimiter after this SSA token.
    // Do not skip it here: a following '%' may be adjacent in malformed IR.
    --offset;
  }
  return values;
}

bool FindSsaDefinition(
    const std::string& text,
    std::string_view value,
    std::string_view& rhs) {
  for (std::size_t line_start = 0; line_start < text.size();) {
    const std::size_t line_end = text.find('\n', line_start);
    const std::size_t bounded_end =
        line_end == std::string::npos ? text.size() : line_end;
    const std::string_view line = TrimLeadingWhitespace(
        std::string_view(text.data() + line_start, bounded_end - line_start));
    if (line.starts_with(value) && line.size() > value.size() &&
        line.substr(value.size()).starts_with(" = ")) {
      rhs = line.substr(value.size() + 3);
      return true;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }
  return false;
}

bool DependsOnSsaValue(
    const std::string& text,
    std::string_view value,
    std::string_view source) {
  std::vector<std::string> pending{std::string(value)};
  std::unordered_set<std::string> visited;
  while (!pending.empty()) {
    std::string current = std::move(pending.back());
    pending.pop_back();
    if (current == source) return true;
    if (!visited.insert(current).second) continue;
    std::string_view rhs;
    if (!FindSsaDefinition(text, current, rhs)) continue;
    for (std::string& dependency : FindSsaValues(rhs))
      if (dependency != current) pending.push_back(std::move(dependency));
  }
  return false;
}

std::string ThresholdAccessValue(
    const std::string& text,
    std::size_t threshold_access) {
  const std::size_t prior_newline = text.rfind('\n', threshold_access);
  const std::size_t line_start = prior_newline == std::string::npos
      ? 0 : prior_newline + 1;
  const std::size_t line_end = text.find('\n', threshold_access);
  const std::size_t bounded_end =
      line_end == std::string::npos ? text.size() : line_end;
  const std::string_view line = TrimLeadingWhitespace(
      std::string_view(text.data() + line_start, bounded_end - line_start));
  const std::size_t equals = line.find(" = ");
  if (equals == std::string_view::npos || equals == 0 ||
      line.front() != '%')
    return {};
  return std::string(line.substr(0, equals));
}

bool FindUniqueDitherIdentityPhi(
    const std::string& text,
    std::size_t threshold_access,
    std::size_t limit,
    DitherIdentityPhi& result,
    std::string& error,
    std::string_view required_threshold_value = {}) {
  for (std::size_t line_start = text.find('\n', threshold_access);
       line_start != std::string::npos && line_start < limit;) {
    ++line_start;
    const std::size_t line_end = text.find('\n', line_start);
    const std::size_t bounded_end =
        line_end == std::string::npos ? limit : std::min(line_end, limit);
    const std::string_view raw_line(text.data() + line_start,
        bounded_end - line_start);
    const std::string_view line = TrimLeadingWhitespace(raw_line);
    if (line.find(" = phi float ") != std::string_view::npos &&
        line.find(kDitherIdentityArm) != std::string_view::npos) {
      const std::size_t equals = line.find(" = phi float ");
      if (!required_threshold_value.empty() &&
          !DependsOnSsaValue(text, line.substr(0, equals),
              required_threshold_value)) {
        if (line_end == std::string::npos) break;
        line_start = line_end;
        continue;
      }
      if (result.start != std::string::npos) {
        error = "dither threshold region has multiple identity phis";
        return false;
      }
      result.start = line_start;
      result.end = bounded_end;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end;
  }
  if (result.start == std::string::npos) {
    error = "dither threshold region has no identity phi";
    return false;
  }

  const std::string_view raw_candidate(
      text.data() + result.start, result.end - result.start);
  const std::string_view candidate = TrimLeadingWhitespace(raw_candidate);
  const std::size_t equals = candidate.find(" = phi float ");
  if (equals == std::string_view::npos || equals == 0 ||
      candidate.empty() || candidate.front() != '%') {
    error = "dither identity phi has an invalid SSA definition";
    return false;
  }
  return true;
}

std::string IdentityReplacement(
    const std::string& text,
    const DitherIdentityPhi& phi) {
  const std::string_view raw_candidate(
      text.data() + phi.start, phi.end - phi.start);
  const std::string_view candidate = TrimLeadingWhitespace(raw_candidate);
  const std::size_t equals = candidate.find(" = phi float ");
  const std::size_t leading_whitespace = raw_candidate.size() - candidate.size();
  return std::string(raw_candidate.substr(0, leading_whitespace)) +
      std::string(candidate.substr(0, equals)) +
      " = fadd fast float 1.000000e+00, 0.000000e+00";
}

std::vector<std::size_t> FindThresholdAccesses(
    const std::string& text,
    std::size_t limit) {
  std::vector<std::size_t> accesses;
  for (std::size_t offset = 0;;) {
    offset = text.find(kThresholdAccess, offset);
    if (offset == std::string::npos || offset >= limit) return accesses;
    accesses.push_back(offset);
    offset += kThresholdAccess.size();
  }
}

bool FindVerifiedPhiLine(
    const std::string& text,
    std::string_view merge_value,
    DitherIdentityPhi& result) {
  for (std::size_t line_start = 0; line_start < text.size();) {
    const std::size_t line_end = text.find('\n', line_start);
    const std::size_t bounded_end =
        line_end == std::string::npos ? text.size() : line_end;
    const std::string_view raw_line(
        text.data() + line_start, bounded_end - line_start);
    const std::string_view line = TrimLeadingWhitespace(raw_line);
    if (line.starts_with(merge_value) &&
        line.substr(merge_value.size()).starts_with(" = phi float ") &&
        line.find(kDitherIdentityArm) != std::string_view::npos) {
      result.start = line_start;
      result.end = bounded_end;
      return true;
    }
    if (line_end == std::string::npos) break;
    line_start = line_end + 1;
  }
  return false;
}

} // namespace

TargetDitherBypassResult PatchSelectedTargetDitherToIdentity(
    const std::string& original_llvm_ir) {
  TargetDitherBypassResult result;
  if (Count(original_llvm_ir, kDiscard) != 1) {
    result.error =
        "selected target must contain exactly one discard";
    return result;
  }

  const std::size_t discard = original_llvm_ir.find(kDiscard);
  const std::size_t table = original_llvm_ir.rfind(kThresholdTable, discard);
  if (table == std::string::npos || table >= discard) {
    result.error = "selected target has no dither threshold access before discard";
    return result;
  }

  DitherIdentityPhi candidate;
  if (!FindUniqueDitherIdentityPhi(
          original_llvm_ir, table, discard, candidate, result.error)) {
    result.error = "selected target " + result.error;
    return result;
  }

  result.structural_verification_succeeded = true;
  result.stage1_structural_verification_succeeded = true;
  result.llvm_ir = original_llvm_ir;
  const std::string replacement =
      IdentityReplacement(original_llvm_ir, candidate);
  result.llvm_ir.replace(
      candidate.start, candidate.end - candidate.start, replacement);
  result.ir_patch_succeeded = true;
  result.stage1_ir_patch_succeeded = true;
  result.success = true;
  return result;
}

TargetDitherBypassResult PatchSelectedDualDitherStagesToIdentity(
    const std::string& original_llvm_ir) {
  TargetDitherBypassResult result;
  if (Count(original_llvm_ir, kDiscard) != 1) {
    result.error = "selected dual-stage target must contain exactly one discard";
    return result;
  }
  const std::size_t discard = original_llvm_ir.find(kDiscard);
  const auto accesses = FindThresholdAccesses(original_llvm_ir, discard);
  if (accesses.size() != 2) {
    result.error = "selected dual-stage target must contain exactly two dither threshold accesses";
    return result;
  }

  DitherIdentityPhi stage1;
  const std::string stage1_threshold =
      ThresholdAccessValue(original_llvm_ir, accesses[0]);
  if (stage1_threshold.empty()) {
    result.error = "stage 1 structural verification failed: invalid threshold access";
    return result;
  }
  if (!FindUniqueDitherIdentityPhi(
          original_llvm_ir, accesses[0], accesses[1], stage1, result.error,
          stage1_threshold)) {
    result.error = "stage 1 structural verification failed: " + result.error;
    return result;
  }
  result.stage1_structural_verification_succeeded = true;

  DitherIdentityPhi stage2;
  const std::string stage2_threshold =
      ThresholdAccessValue(original_llvm_ir, accesses[1]);
  if (stage2_threshold.empty()) {
    result.error = "stage 2 structural verification failed: invalid threshold access";
    return result;
  }
  if (!FindUniqueDitherIdentityPhi(
          original_llvm_ir, accesses[1], discard, stage2, result.error,
          stage2_threshold)) {
    result.error = "stage 2 structural verification failed: " + result.error;
    return result;
  }
  result.stage2_structural_verification_succeeded = true;
  result.structural_verification_succeeded = true;

  result.llvm_ir = original_llvm_ir;
  result.llvm_ir.replace(stage2.start, stage2.end - stage2.start,
      IdentityReplacement(original_llvm_ir, stage2));
  result.stage2_ir_patch_succeeded = true;
  result.llvm_ir.replace(stage1.start, stage1.end - stage1.start,
      IdentityReplacement(original_llvm_ir, stage1));
  result.stage1_ir_patch_succeeded = true;
  result.ir_patch_succeeded = true;
  result.success = true;
  return result;
}

TargetDitherBypassResult PatchAllVerifiedFadePrimitiveInstancesToIdentity(
    const std::string& original_llvm_ir) {
  TargetDitherBypassResult result;
  const FadePrimitiveDiagnostic diagnostic =
      AnalyzeFadePrimitiveV1(original_llvm_ir);
  if (diagnostic.instances.empty()) {
    result.error = "no verified transparency-filter primitive instance";
    return result;
  }
  result.verified_instance_count = diagnostic.instances.size();

  std::vector<DitherIdentityPhi> replacements;
  replacements.reserve(diagnostic.instances.size());
  std::unordered_set<std::string> merge_values;
  for (const FadePrimitiveInstance& instance : diagnostic.instances) {
    if (instance.merge_value.empty() ||
        !merge_values.insert(instance.merge_value).second) {
      result.error = "verified primitive instances do not have unique merges";
      return result;
    }
    DitherIdentityPhi phi;
    if (!FindVerifiedPhiLine(original_llvm_ir, instance.merge_value, phi)) {
      result.error = "verified primitive merge could not be located";
      return result;
    }
    replacements.push_back(phi);
  }

  std::sort(replacements.begin(), replacements.end(),
      [](const DitherIdentityPhi& left, const DitherIdentityPhi& right) {
        return left.start > right.start;
      });
  result.structural_verification_succeeded = true;
  result.llvm_ir = original_llvm_ir;
  for (const DitherIdentityPhi& replacement : replacements) {
    result.llvm_ir.replace(replacement.start,
        replacement.end - replacement.start,
        IdentityReplacement(original_llvm_ir, replacement));
  }
  const FadePrimitiveDiagnostic post_patch =
      AnalyzeFadePrimitiveV1(result.llvm_ir);
  if (!post_patch.instances.empty()) {
    result.llvm_ir.clear();
    result.error = "post-patch verification still found a primitive instance";
    return result;
  }
  result.ir_patch_succeeded = true;
  result.patched_instance_count = replacements.size();
  result.success = true;
  return result;
}

} // namespace wuwa_tfr

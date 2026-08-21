// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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
  std::size_t non_identity_value_start = std::string::npos;
  std::size_t non_identity_value_end = std::string::npos;
  std::string result_value;
  std::string non_identity_value;
};

bool IsSsaNameCharacter(char value) noexcept {
  return (value >= 'a' && value <= 'z') ||
      (value >= 'A' && value <= 'Z') ||
      (value >= '0' && value <= '9') || value == '-' ||
      value == '_' || value == '.' || value == '$';
}

bool IsSsaValue(std::string_view value) noexcept {
  if (value.size() < 2 || value.front() != '%') return false;
  for (std::size_t index = 1; index < value.size(); ++index)
    if (!IsSsaNameCharacter(value[index])) return false;
  return true;
}

bool IsIdentityOne(std::string_view value) noexcept {
  return value == "1.000000e+00";
}

std::string_view TrimWhitespace(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size() &&
      std::isspace(static_cast<unsigned char>(text[first])) != 0)
    ++first;
  std::size_t last = text.size();
  while (last > first &&
      std::isspace(static_cast<unsigned char>(text[last - 1])) != 0)
    --last;
  return text.substr(first, last - first);
}

struct PhiArm {
  std::size_t value_start = std::string::npos;
  std::size_t value_end = std::string::npos;
  std::string_view value;
  std::string_view predecessor;
};

bool ParsePhiArm(
    std::string_view phi, std::size_t& cursor, PhiArm& arm) {
  while (cursor < phi.size() &&
      std::isspace(static_cast<unsigned char>(phi[cursor])) != 0)
    ++cursor;
  if (cursor == phi.size() || phi[cursor] != '[') return false;
  const std::size_t value_begin = ++cursor;
  const std::size_t comma = phi.find(',', cursor);
  const std::size_t close = phi.find(']', cursor);
  if (comma == std::string_view::npos || close == std::string_view::npos ||
      comma >= close)
    return false;
  const std::string_view raw_value = phi.substr(value_begin, comma - value_begin);
  const std::string_view value = TrimWhitespace(raw_value);
  const std::string_view predecessor =
      TrimWhitespace(phi.substr(comma + 1, close - comma - 1));
  if (value.empty() || !IsSsaValue(predecessor)) return false;
  arm.value_start = value_begin + (value.data() - raw_value.data());
  arm.value_end = arm.value_start + value.size();
  arm.value = value;
  arm.predecessor = predecessor;
  cursor = close + 1;
  return true;
}

bool ParseVerifiedDitherIdentityPhi(
    const std::string& text, DitherIdentityPhi& result, std::string& error) {
  if (result.start == std::string::npos || result.end == std::string::npos ||
      result.start >= result.end || result.end > text.size()) {
    error = "dither identity phi has an invalid source range";
    return false;
  }
  const std::string_view raw_candidate(
      text.data() + result.start, result.end - result.start);
  const std::string_view candidate = TrimLeadingWhitespace(raw_candidate);
  const std::size_t leading_whitespace = raw_candidate.size() - candidate.size();
  constexpr std::string_view kPhiPrefix = " = phi float ";
  const std::size_t equals = candidate.find(kPhiPrefix);
  if (equals == std::string_view::npos ||
      !IsSsaValue(candidate.substr(0, equals))) {
    error = "dither identity phi has an invalid SSA definition";
    return false;
  }

  std::size_t cursor = equals + kPhiPrefix.size();
  std::array<PhiArm, 2> arms;
  if (!ParsePhiArm(candidate, cursor, arms[0])) {
    error = "dither identity phi has an invalid first incoming arm";
    return false;
  }
  while (cursor < candidate.size() &&
      std::isspace(static_cast<unsigned char>(candidate[cursor])) != 0)
    ++cursor;
  if (cursor == candidate.size() || candidate[cursor++] != ',') {
    error = "dither identity phi must have exactly two incoming arms";
    return false;
  }
  if (!ParsePhiArm(candidate, cursor, arms[1])) {
    error = "dither identity phi has an invalid second incoming arm";
    return false;
  }
  while (cursor < candidate.size() &&
      std::isspace(static_cast<unsigned char>(candidate[cursor])) != 0)
    ++cursor;
  if (cursor != candidate.size() || arms[0].predecessor == arms[1].predecessor) {
    error = "dither identity phi must have exactly two distinct predecessors";
    return false;
  }

  const bool first_is_identity = IsIdentityOne(arms[0].value);
  const bool second_is_identity = IsIdentityOne(arms[1].value);
  if (first_is_identity == second_is_identity) {
    error = "dither identity phi must have exactly one identity arm";
    return false;
  }
  const PhiArm& non_identity = first_is_identity ? arms[1] : arms[0];
  if (!IsSsaValue(non_identity.value)) {
    error = "dither identity phi has no non-identity SSA incoming value";
    return false;
  }

  result.non_identity_value_start = result.start + leading_whitespace +
      non_identity.value_start;
  result.non_identity_value_end = result.start + leading_whitespace +
      non_identity.value_end;
  result.result_value = std::string(candidate.substr(0, equals));
  result.non_identity_value = std::string(non_identity.value);
  return true;
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
      DitherIdentityPhi candidate;
      candidate.start = line_start;
      candidate.end = bounded_end;
      std::string parse_error;
      if (!ParseVerifiedDitherIdentityPhi(text, candidate, parse_error)) {
        error = parse_error;
        return false;
      }
      if (!required_threshold_value.empty() &&
          !DependsOnSsaValue(text, candidate.non_identity_value,
              required_threshold_value)) {
        if (line_end == std::string::npos) break;
        line_start = line_end;
        continue;
      }
      if (result.start != std::string::npos) {
        error = "dither threshold region has multiple identity phis";
        return false;
      }
      result = std::move(candidate);
    }
    if (line_end == std::string::npos) break;
    line_start = line_end;
  }
  if (result.start == std::string::npos) {
    error = "dither threshold region has no identity phi";
    return false;
  }

  return true;
}

bool RewriteVerifiedDitherIdentityPhi(
    std::string& text, const DitherIdentityPhi& expected, std::string& error) {
  DitherIdentityPhi current;
  current.start = expected.start;
  current.end = expected.end;
  if (!ParseVerifiedDitherIdentityPhi(text, current, error) ||
      current.result_value != expected.result_value ||
      current.non_identity_value != expected.non_identity_value) {
    if (error.empty()) error = "dither identity phi changed before rewrite";
    return false;
  }
  text.replace(current.non_identity_value_start,
      current.non_identity_value_end - current.non_identity_value_start,
      "1.000000e+00");
  return true;
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

bool FindVerifiedPhiAtRange(
    const std::string& text,
    const FadePrimitiveInstance& instance,
    DitherIdentityPhi& result,
    std::string& error) {
  if (instance.function_identity.empty() || instance.merge_value.empty() ||
      instance.phi_start >= instance.phi_end || instance.phi_end > text.size()) {
    error = "verified primitive has an invalid function/source identity";
    return false;
  }
  // The matcher records whole-line offsets.  Refuse a shifted range instead
  // of searching another function for an identically named SSA value.
  if ((instance.phi_start != 0 && text[instance.phi_start - 1] != '\n') ||
      (instance.phi_end != text.size() && text[instance.phi_end] != '\n')) {
    error = "verified primitive phi range is not a complete source line";
    return false;
  }
  result.start = instance.phi_start;
  result.end = instance.phi_end;
  if (!ParseVerifiedDitherIdentityPhi(text, result, error)) return false;
  if (result.result_value != instance.merge_value) {
    error = "verified primitive merge has an inconsistent SSA definition";
    return false;
  }
  return true;
}

bool IsProductionConsumer(FadePrimitiveConsumer consumer) noexcept {
  return consumer == FadePrimitiveConsumer::Discard ||
      consumer == FadePrimitiveConsumer::SvTargetAlpha ||
      consumer == FadePrimitiveConsumer::DiscardAndSvTargetAlpha;
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
  std::string patched_llvm_ir = original_llvm_ir;
  if (!RewriteVerifiedDitherIdentityPhi(
          patched_llvm_ir, candidate, result.error)) {
    result.error = "selected target rewrite failed: " + result.error;
    return result;
  }
  result.llvm_ir = std::move(patched_llvm_ir);
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

  std::string patched_llvm_ir = original_llvm_ir;
  if (!RewriteVerifiedDitherIdentityPhi(
          patched_llvm_ir, stage2, result.error)) {
    result.error = "stage 2 rewrite failed: " + result.error;
    return result;
  }
  if (!RewriteVerifiedDitherIdentityPhi(
          patched_llvm_ir, stage1, result.error)) {
    result.error = "stage 1 rewrite failed: " + result.error;
    return result;
  }
  result.llvm_ir = std::move(patched_llvm_ir);
  result.stage1_ir_patch_succeeded = true;
  result.stage2_ir_patch_succeeded = true;
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
  std::unordered_set<std::size_t> phi_starts;
  for (const FadePrimitiveInstance& instance : diagnostic.instances) {
    if (!IsProductionConsumer(instance.consumer)) {
      result.error = "verified primitive has an unsupported production consumer";
      return result;
    }
    if (!phi_starts.insert(instance.phi_start).second) {
      result.error = "verified primitive instances do not have unique source identities";
      return result;
    }
    DitherIdentityPhi phi;
    if (!FindVerifiedPhiAtRange(
            original_llvm_ir, instance, phi, result.error)) {
      result.error = "verified primitive phi could not be revalidated: " +
          result.error;
      return result;
    }
    replacements.push_back(phi);
  }

  std::sort(replacements.begin(), replacements.end(),
      [](const DitherIdentityPhi& left, const DitherIdentityPhi& right) {
        return left.start > right.start;
      });
  result.structural_verification_succeeded = true;
  std::string patched_llvm_ir = original_llvm_ir;
  for (const DitherIdentityPhi& replacement : replacements) {
    if (!RewriteVerifiedDitherIdentityPhi(
            patched_llvm_ir, replacement, result.error)) {
      result.error = "verified primitive rewrite failed: " + result.error;
      return result;
    }
  }
  const FadePrimitiveDiagnostic post_patch =
      AnalyzeFadePrimitiveV1(patched_llvm_ir);
  if (!post_patch.instances.empty()) {
    result.error = "post-patch verification still found a primitive instance";
    return result;
  }
  result.llvm_ir = std::move(patched_llvm_ir);
  result.ir_patch_succeeded = true;
  result.patched_instance_count = replacements.size();
  result.success = true;
  return result;
}

} // namespace wuwa_tfr

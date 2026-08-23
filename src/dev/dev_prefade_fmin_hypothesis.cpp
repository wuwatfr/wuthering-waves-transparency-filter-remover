// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_prefade_fmin_hypothesis.hpp"

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr::dev {
namespace {

// This module intentionally never reuses the verified detector's internal
// parsed graph (fade_primitive_detector.cpp keeps it private in an anonymous
// namespace). Like target_dither_bypass.cpp, it independently re-derives and
// re-verifies, from raw text, exactly the structure it is about to touch --
// never trusting cached state from a prior pass.

constexpr std::size_t kBackwardSliceLimit = 1024;

bool IsSsaCharacter(char value) noexcept {
  return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
      value == '_' || value == '.' || value == '$' || value == '-';
}

bool IsSsaValue(std::string_view value) noexcept {
  if (value.size() < 2 || value.front() != '%') return false;
  for (std::size_t index = 1; index < value.size(); ++index)
    if (!IsSsaCharacter(value[index])) return false;
  return true;
}

std::string_view Trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

std::string_view StripComment(std::string_view line) noexcept {
  bool quoted = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (quoted && line[index] == '\\' && index + 1 < line.size()) { ++index; continue; }
    if (line[index] == '"') quoted = !quoted;
    if (!quoted && line[index] == ';') return line.substr(0, index);
  }
  return line;
}

std::vector<std::string_view> SsaValues(std::string_view text) {
  std::vector<std::string_view> values;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%' || index + 1 == text.size() ||
        !IsSsaCharacter(text[index + 1])) continue;
    const std::size_t first = index++;
    while (index < text.size() && IsSsaCharacter(text[index])) ++index;
    values.emplace_back(text.substr(first, index - first));
    if (index == text.size()) break;
    --index;
  }
  return values;
}

std::string FunctionIdentity(std::string_view header) {
  const std::size_t at = header.find('@');
  const std::size_t open = at == std::string_view::npos ? at : header.find('(', at);
  if (at == std::string_view::npos || open == std::string_view::npos || open == at + 1)
    return {};
  for (std::size_t index = at + 1; index < open; ++index)
    if (!IsSsaCharacter(header[index])) return {};
  return std::string(header.substr(at, open - at));
}

std::size_t AbsoluteOffset(const std::string& text, std::string_view view) noexcept {
  return static_cast<std::size_t>(view.data() - text.data());
}

struct FunctionLine {
  std::string_view code;  // comment-stripped, trimmed; view into original_llvm_ir
  std::string_view lhs;   // empty if this line defines nothing
  std::string_view rhs;   // view into original_llvm_ir, following "lhs = "
};

struct ParsedFunction {
  std::vector<FunctionLine> lines;
  std::unordered_map<std::string, std::size_t> definitions;
  bool complete = true;
};

// Locates the unique `define ... @identity(...) { ... }` block. Fails closed
// (returns false) if the identity is missing, duplicated, or the block is
// not well-formed -- never guesses at a shifted or partial range.
bool FindUniqueFunctionBlock(const std::string& text, std::string_view identity,
    std::size_t& block_start, std::size_t& block_end, std::string& error) {
  bool found = false;
  std::size_t depth = 0;
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end = newline == std::string::npos ? text.size() : newline;
    const std::string_view line = Trim(StripComment(
        std::string_view(text).substr(start, end - start)));
    if (depth == 0) {
      if (line.starts_with("define ") && FunctionIdentity(line) == identity) {
        if (found) {
          error = "verified primitive function identity is not unique";
          return false;
        }
        found = true;
        block_start = start;
        depth = 1;
      }
    } else if (line == "}") {
      depth = 0;
      block_end = end;
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  if (!found || depth != 0) {
    error = "verified primitive function could not be located";
    return false;
  }
  return true;
}

ParsedFunction ParseFunctionBlock(
    const std::string& text, std::size_t block_start, std::size_t block_end) {
  ParsedFunction function;
  bool header_skipped = false;
  for (std::size_t start = block_start; start < block_end;) {
    const std::size_t newline = text.find('\n', start);
    const std::size_t end = (newline == std::string::npos || newline > block_end)
        ? block_end : newline;
    const std::string_view raw(text.data() + start, end - start);
    const std::string_view code = Trim(StripComment(raw));
    if (!header_skipped) {
      header_skipped = true;  // the `define ... {` line itself
    } else if (!code.empty() && code != "}") {
      FunctionLine line;
      line.code = code;
      const std::size_t equals = code.find(" = ");
      if (equals != std::string_view::npos) {
        const std::string_view lhs = Trim(code.substr(0, equals));
        if (!lhs.empty() && lhs.front() == '%') {
          line.lhs = lhs;
          line.rhs = code.substr(equals + 3);
        }
      }
      const std::size_t index = function.lines.size();
      if (!line.lhs.empty() &&
          !function.definitions.emplace(std::string(line.lhs), index).second)
        function.complete = false;
      function.lines.push_back(line);
    }
    if (newline == std::string::npos || newline >= block_end) break;
    start = newline + 1;
  }
  return function;
}

const FunctionLine* Definition(const ParsedFunction& function, std::string_view value) {
  const auto found = function.definitions.find(std::string(value));
  return found == function.definitions.end() ? nullptr : &function.lines[found->second];
}

struct Slice {
  std::unordered_set<std::string> values;
  bool complete = true;
};

Slice BackwardSlice(const ParsedFunction& function, std::string_view root) {
  Slice slice;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty()) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!slice.values.insert(value).second) continue;
    if (const FunctionLine* definition = Definition(function, value))
      for (const std::string_view dependency : SsaValues(definition->rhs))
        pending.push_back(std::string(dependency));
    if (slice.values.size() >= kBackwardSliceLimit && !pending.empty()) {
      slice.complete = false;
      return slice;
    }
  }
  return slice;
}

// extractvalue <type> <%agg>, <index> -- located by position from the end
// (single-index form only), exactly as the type name also satisfies the
// SSA-character predicate and must not be mistaken for the aggregate.
bool ParseExtractValueAggregate(std::string_view rhs, std::string_view& aggregate) {
  if (!rhs.starts_with("extractvalue")) return false;
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view before_index = Trim(rhs.substr(0, comma));
  const std::size_t agg_start = before_index.find_last_of(" \t");
  if (agg_start == std::string_view::npos) return false;
  const std::string_view candidate = before_index.substr(agg_start + 1);
  if (!IsSsaValue(candidate)) return false;
  aggregate = candidate;
  return true;
}

bool ConsumeToken(std::string_view text, std::size_t& cursor, std::string_view token) {
  while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
    ++cursor;
  if (!text.substr(cursor).starts_with(token)) return false;
  cursor += token.size();
  return true;
}

// call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %h, i32 <row>)
bool ParseCBufferLoadLegacyHandle(std::string_view rhs, std::string_view& handle) {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") ||
      !ConsumeToken(rhs, cursor, "%dx.types.CBufRet.f32") ||
      !ConsumeToken(rhs, cursor, "@dx.op.cbufferLoadLegacy.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "59") || !ConsumeToken(rhs, cursor, ",") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle"))
    return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  const std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')') ++cursor;
  const std::string_view candidate = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(candidate)) return false;
  handle = candidate;
  return true;
}

// call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %h, i32 <byteOffset>)
bool ParseCBufferLoadByteHandle(std::string_view rhs, std::string_view& handle) {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.cbufferLoad.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "58") || !ConsumeToken(rhs, cursor, ",") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle"))
    return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  const std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')') ++cursor;
  const std::string_view candidate = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(candidate)) return false;
  handle = candidate;
  return true;
}

// Resolves an FMin operand *directly* to the constant-buffer handle it was
// loaded from: extractvalue-of-cbufferLoadLegacy.f32, or a bare
// cbufferLoad.f32 call. No bitcast/phi/select/freeze copy-chain is followed:
// an indirect path is not "direct" and must not qualify.
bool ResolveDirectCBufferHandle(
    const ParsedFunction& function, std::string_view operand, std::string_view& handle) {
  if (!IsSsaValue(operand)) return false;
  const FunctionLine* definition = Definition(function, operand);
  if (!definition) return false;
  std::string_view aggregate;
  if (ParseExtractValueAggregate(definition->rhs, aggregate)) {
    const FunctionLine* aggregate_definition = Definition(function, aggregate);
    if (!aggregate_definition) return false;
    return ParseCBufferLoadLegacyHandle(aggregate_definition->rhs, handle);
  }
  return ParseCBufferLoadByteHandle(definition->rhs, handle);
}

// call float @dx.op.binary.f32(i32 36, float <A>, float <B>)
// Captures A/B as views into `rhs` (hence into original_llvm_ir), so their
// absolute byte offsets are recoverable for a precise text rewrite.
bool ParseFMinOperands(std::string_view rhs, std::string_view& operand_a,
    std::string_view& operand_b) {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.binary.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "36") || !ConsumeToken(rhs, cursor, ","))
    return false;
  if (!ConsumeToken(rhs, cursor, "float")) return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',') ++cursor;
  operand_a = Trim(rhs.substr(start, cursor - start));
  if (cursor == rhs.size() || rhs[cursor] != ',') return false;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "float")) return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  operand_b = Trim(rhs.substr(start, cursor - start));
  return !operand_a.empty() && !operand_b.empty() && IsSsaValue(operand_a) &&
      cursor < rhs.size() && rhs[cursor] == ')';
}

struct QualifyingCandidate {
  std::string_view operand_a;  // view into original_llvm_ir; the rewrite target
  std::string_view operand_b;
};

// Enumerates every FMin whose both operands directly resolve to the same
// constant-buffer handle, within the given backward slice. Returns an empty
// vector (not an error) when none qualify; the caller decides what "zero"
// versus "more than one" means.
std::vector<QualifyingCandidate> FindQualifyingFMinCandidates(
    const ParsedFunction& function, const Slice& slice) {
  std::vector<QualifyingCandidate> candidates;
  if (!slice.complete) return candidates;
  for (const FunctionLine& line : function.lines) {
    if (line.lhs.empty() || !slice.values.contains(std::string(line.lhs))) continue;
    std::string_view operand_a, operand_b;
    if (!ParseFMinOperands(line.rhs, operand_a, operand_b)) continue;
    std::string_view handle_a, handle_b;
    if (!ResolveDirectCBufferHandle(function, operand_a, handle_a) ||
        !ResolveDirectCBufferHandle(function, operand_b, handle_b) ||
        handle_a != handle_b)
      continue;
    candidates.push_back({operand_a, operand_b});
  }
  return candidates;
}

struct PhiArm {
  std::string_view value;
  std::string_view predecessor;
};

bool ParsePhiArm(std::string_view phi, std::size_t& cursor, PhiArm& arm) {
  while (cursor < phi.size() && std::isspace(static_cast<unsigned char>(phi[cursor])) != 0)
    ++cursor;
  if (cursor == phi.size() || phi[cursor] != '[') return false;
  const std::size_t value_begin = ++cursor;
  const std::size_t comma = phi.find(',', cursor);
  const std::size_t close = phi.find(']', cursor);
  if (comma == std::string_view::npos || close == std::string_view::npos || comma >= close)
    return false;
  arm.value = Trim(phi.substr(value_begin, comma - value_begin));
  arm.predecessor = Trim(phi.substr(comma + 1, close - comma - 1));
  if (arm.value.empty() || !IsSsaValue(arm.predecessor)) return false;
  cursor = close + 1;
  return true;
}

bool IsIdentityOne(std::string_view value) noexcept {
  return value == "1.000000e+00" || value == "1.0" || value == "1.000000";
}

// Independently re-derives the enabled (non-identity) arm value from the
// verified phi's exact recorded source range, and cross-checks it against
// the recorded result SSA name -- the same defense-in-depth re-verification
// target_dither_bypass.cpp applies before it will touch a phi.
bool ReDeriveEnabledArmValue(const std::string& text,
    const wuwa_tfr::FadePrimitiveInstance& instance, std::string_view& enabled_value,
    std::string& error) {
  if (instance.function_identity.empty() || instance.merge_value.empty() ||
      instance.phi_start >= instance.phi_end || instance.phi_end > text.size()) {
    error = "verified primitive has an invalid function/source identity";
    return false;
  }
  if ((instance.phi_start != 0 && text[instance.phi_start - 1] != '\n') ||
      (instance.phi_end != text.size() && text[instance.phi_end] != '\n')) {
    error = "verified primitive phi range is not a complete source line";
    return false;
  }
  const std::string_view raw(
      text.data() + instance.phi_start, instance.phi_end - instance.phi_start);
  const std::string_view candidate = Trim(raw);
  constexpr std::string_view kPhiPrefix = " = phi float ";
  const std::size_t equals = candidate.find(kPhiPrefix);
  if (equals == std::string_view::npos || !IsSsaValue(candidate.substr(0, equals))) {
    error = "verified primitive phi has an invalid SSA definition";
    return false;
  }
  if (candidate.substr(0, equals) != instance.merge_value) {
    error = "verified primitive merge has an inconsistent SSA definition";
    return false;
  }
  std::size_t cursor = equals + kPhiPrefix.size();
  std::array<PhiArm, 2> arms;
  if (!ParsePhiArm(candidate, cursor, arms[0])) {
    error = "verified primitive phi has an invalid first incoming arm";
    return false;
  }
  while (cursor < candidate.size() && std::isspace(static_cast<unsigned char>(candidate[cursor])) != 0)
    ++cursor;
  if (cursor == candidate.size() || candidate[cursor++] != ',') {
    error = "verified primitive phi must have exactly two incoming arms";
    return false;
  }
  if (!ParsePhiArm(candidate, cursor, arms[1])) {
    error = "verified primitive phi has an invalid second incoming arm";
    return false;
  }
  while (cursor < candidate.size() && std::isspace(static_cast<unsigned char>(candidate[cursor])) != 0)
    ++cursor;
  if (cursor != candidate.size() || arms[0].predecessor == arms[1].predecessor) {
    error = "verified primitive phi must have exactly two distinct predecessors";
    return false;
  }
  const bool first_identity = IsIdentityOne(arms[0].value);
  const bool second_identity = IsIdentityOne(arms[1].value);
  if (first_identity == second_identity) {
    error = "verified primitive phi must have exactly one identity arm";
    return false;
  }
  const PhiArm& enabled = first_identity ? arms[1] : arms[0];
  if (!IsSsaValue(enabled.value)) {
    error = "verified primitive phi has no non-identity SSA incoming value";
    return false;
  }
  enabled_value = enabled.value;
  return true;
}

struct InstanceRewrite {
  std::size_t operand_one_start = 0;
  std::size_t operand_one_end = 0;
  std::string expected_operand_one_text;
};

// -------- process-lifetime diagnostics --------

std::mutex g_diagnostics_mutex;
std::uint64_t g_shaders_evaluated_total = 0;
std::uint64_t g_verified_instances_total = 0;
std::uint64_t g_qualifying_instances_total = 0;
std::uint64_t g_patched_instances_total = 0;
std::uint64_t g_shaders_failed_total = 0;
std::string g_last_failure_reason;

void RecordFailure(std::string_view reason) {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  ++g_shaders_failed_total;
  g_last_failure_reason.assign(reason);
}

}  // namespace

TargetDitherBypassResult PatchPreFadeFMinOperandOneHypothesis(
    const std::string& original_llvm_ir) {
  TargetDitherBypassResult result;
  const wuwa_tfr::FadePrimitiveDiagnostic diagnostic =
      wuwa_tfr::AnalyzeFadePrimitiveV1(original_llvm_ir);
  if (diagnostic.instances.empty()) {
    result.error = "no verified transparency-filter primitive instance";
    RecordFailure(result.error);
    return result;
  }
  result.verified_instance_count = diagnostic.instances.size();
  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    ++g_shaders_evaluated_total;
    g_verified_instances_total += diagnostic.instances.size();
  }

  std::vector<InstanceRewrite> rewrites;
  rewrites.reserve(diagnostic.instances.size());
  std::unordered_set<std::size_t> phi_starts_seen;
  std::size_t qualifying_count = 0;

  for (const wuwa_tfr::FadePrimitiveInstance& instance : diagnostic.instances) {
    if (!phi_starts_seen.insert(instance.phi_start).second) {
      result.error = "verified primitive instances do not have unique source identities";
      RecordFailure(result.error);
      return result;
    }

    std::string_view enabled_value;
    if (!ReDeriveEnabledArmValue(original_llvm_ir, instance, enabled_value, result.error)) {
      RecordFailure(result.error);
      return result;
    }

    std::size_t block_start = 0, block_end = 0;
    if (!FindUniqueFunctionBlock(
            original_llvm_ir, instance.function_identity, block_start, block_end, result.error)) {
      RecordFailure(result.error);
      return result;
    }
    const ParsedFunction function =
        ParseFunctionBlock(original_llvm_ir, block_start, block_end);
    if (!function.complete) {
      result.error = "verified primitive function has duplicate SSA definitions";
      RecordFailure(result.error);
      return result;
    }

    const Slice slice = BackwardSlice(function, enabled_value);
    if (!slice.complete) {
      result.error = "verified primitive backward slice is incomplete";
      RecordFailure(result.error);
      return result;
    }

    const std::vector<QualifyingCandidate> candidates =
        FindQualifyingFMinCandidates(function, slice);
    if (candidates.size() != 1) {
      result.error = candidates.empty()
          ? "no qualifying pre-Fade FMin found in the verified backward slice"
          : "multiple qualifying pre-Fade FMin candidates; ambiguous";
      RecordFailure(result.error);
      return result;
    }
    ++qualifying_count;

    InstanceRewrite rewrite;
    rewrite.operand_one_start = AbsoluteOffset(original_llvm_ir, candidates.front().operand_a);
    rewrite.operand_one_end =
        rewrite.operand_one_start + candidates.front().operand_a.size();
    rewrite.expected_operand_one_text = std::string(candidates.front().operand_a);
    rewrites.push_back(std::move(rewrite));
  }

  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    g_qualifying_instances_total += qualifying_count;
  }

  std::sort(rewrites.begin(), rewrites.end(),
      [](const InstanceRewrite& left, const InstanceRewrite& right) {
        return left.operand_one_start > right.operand_one_start;
      });
  result.structural_verification_succeeded = true;
  std::string patched_llvm_ir = original_llvm_ir;
  for (const InstanceRewrite& rewrite : rewrites) {
    const std::string_view current(
        patched_llvm_ir.data() + rewrite.operand_one_start,
        rewrite.operand_one_end - rewrite.operand_one_start);
    if (current != rewrite.expected_operand_one_text) {
      result.error = "verified pre-Fade FMin operand changed before rewrite";
      RecordFailure(result.error);
      return result;
    }
    patched_llvm_ir.replace(rewrite.operand_one_start,
        rewrite.operand_one_end - rewrite.operand_one_start, "1.000000e+00");
  }

  // Post-patch: the downstream Fade Primitive structure (phi, gate, coverage
  // expression, consumer classification) must be exactly as verified before
  // the rewrite -- proving the identity-phi collapse was never triggered and
  // nothing downstream moved. Raw phi_start/phi_end byte offsets are not
  // compared here: operand 1's replacement text ("1.000000e+00") is a
  // different length than the SSA name it replaces, which shifts every
  // absolute offset located textually after it, including a legitimately
  // untouched phi. SSA identity (function + merge value + consumer) is the
  // offset-invariant proof that the same phi, unmoved in the instruction
  // stream, still classifies the same way.
  const wuwa_tfr::FadePrimitiveDiagnostic post_patch_diagnostic =
      wuwa_tfr::AnalyzeFadePrimitiveV1(patched_llvm_ir);
  if (post_patch_diagnostic.instances.size() != diagnostic.instances.size()) {
    result.error = "post-patch verification: downstream Fade Primitive structure changed";
    RecordFailure(result.error);
    return result;
  }
  for (std::size_t i = 0; i < diagnostic.instances.size(); ++i) {
    const auto& before = diagnostic.instances[i];
    const auto& after = post_patch_diagnostic.instances[i];
    if (before.function_identity != after.function_identity ||
        before.merge_value != after.merge_value || before.consumer != after.consumer) {
      result.error = "post-patch verification: downstream Fade Primitive structure changed";
      RecordFailure(result.error);
      return result;
    }
  }

  // And the qualifying FMin itself must be gone for every instance: operand 1
  // is now a literal, so ResolveDirectCBufferHandle must fail on it.
  for (const wuwa_tfr::FadePrimitiveInstance& instance : post_patch_diagnostic.instances) {
    std::string_view enabled_value;
    std::string ignored_error;
    if (!ReDeriveEnabledArmValue(patched_llvm_ir, instance, enabled_value, ignored_error)) {
      result.error = "post-patch verification failed to re-derive the enabled path";
      RecordFailure(result.error);
      return result;
    }
    std::size_t block_start = 0, block_end = 0;
    if (!FindUniqueFunctionBlock(
            patched_llvm_ir, instance.function_identity, block_start, block_end, ignored_error)) {
      result.error = "post-patch verification failed to re-locate the function";
      RecordFailure(result.error);
      return result;
    }
    const ParsedFunction function =
        ParseFunctionBlock(patched_llvm_ir, block_start, block_end);
    const Slice slice = BackwardSlice(function, enabled_value);
    if (!slice.complete ||
        !FindQualifyingFMinCandidates(function, slice).empty()) {
      result.error = "post-patch verification: qualifying pre-Fade FMin still present";
      RecordFailure(result.error);
      return result;
    }
  }

  result.llvm_ir = std::move(patched_llvm_ir);
  result.ir_patch_succeeded = true;
  result.patched_instance_count = rewrites.size();
  result.success = true;
  {
    std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
    g_patched_instances_total += rewrites.size();
  }
  return result;
}

PreFadeFMinHypothesisDiagnostics PreFadeFMinHypothesisDiagnosticsSnapshot() {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  return {g_shaders_evaluated_total, g_verified_instances_total,
      g_qualifying_instances_total, g_patched_instances_total, g_shaders_failed_total};
}

std::string LastPreFadeFMinHypothesisFailureReason() {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  return g_last_failure_reason;
}

void ResetPreFadeFMinHypothesisDiagnosticsForTest() {
  std::lock_guard<std::mutex> lock(g_diagnostics_mutex);
  g_shaders_evaluated_total = 0;
  g_verified_instances_total = 0;
  g_qualifying_instances_total = 0;
  g_patched_instances_total = 0;
  g_shaders_failed_total = 0;
  g_last_failure_reason.clear();
}

}  // namespace wuwa_tfr::dev

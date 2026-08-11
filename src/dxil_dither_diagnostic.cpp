// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxil_dither_diagnostic.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr {
namespace {

using Definitions = std::unordered_map<std::string, std::string>;

bool IsSsaCharacter(char value) noexcept {
  return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
      value == '_' || value == '.' || value == '$' || value == '-';
}

std::string Trim(std::string_view text) {
  const auto first = text.find_first_not_of(" 	");
  if (first == std::string_view::npos) return {};
  const auto last = text.find_last_not_of(" 	");
  return std::string(text.substr(first, last - first + 1));
}

std::vector<std::string> SsaValues(std::string_view text) {
  std::vector<std::string> values;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] != '%' || index + 1 == text.size() ||
        !IsSsaCharacter(text[index + 1]))
      continue;
    const std::size_t first = index++;
    while (index < text.size() && IsSsaCharacter(text[index])) ++index;
    values.emplace_back(text.substr(first, index - first));
    --index;
  }
  return values;
}

Definitions ParseDefinitions(const std::string& llvm_ir) {
  Definitions definitions;
  std::istringstream stream(llvm_ir);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const std::string name = Trim(std::string_view(line).substr(0, equals));
    if (name.empty() || name.front() != '%') continue;
    definitions.emplace(name, line.substr(equals + 1));
  }
  return definitions;
}

const std::string* FindDefinition(
    const Definitions& definitions,
    const std::string& value) {
  const auto found = definitions.find(value);
  return found == definitions.end() ? nullptr : &found->second;
}

bool HasOp(const std::string& expression, std::string_view op) {
  return expression.find(op) != std::string::npos;
}

bool IsRound(
    const Definitions& definitions,
    const std::string& value) {
  const std::string* definition = FindDefinition(definitions, value);
  return definition && HasOp(*definition, "Round_ni(");
}

bool IsStrictSpatialNoise(
    const Definitions& definitions,
    const std::string& value,
    std::unordered_set<std::string>& visiting) {
  if (!visiting.insert(value).second) return false;
  const std::string* definition = FindDefinition(definitions, value);
  if (!definition || !HasOp(*definition, "Frc(")) return false;

  const auto frc_inputs = SsaValues(*definition);
  if (frc_inputs.empty()) return false;
  const std::string* multiply =
      FindDefinition(definitions, frc_inputs.back());
  if (!multiply || !HasOp(*multiply, " fmul ")) return false;

  const auto multiply_inputs = SsaValues(*multiply);
  for (const auto& input : multiply_inputs) {
    const std::string* cosine = FindDefinition(definitions, input);
    if (!cosine || !HasOp(*cosine, "Cos(")) continue;
    const auto cosine_inputs = SsaValues(*cosine);
    if (cosine_inputs.empty()) continue;
    const std::string* dot =
        FindDefinition(definitions, cosine_inputs.back());
    if (!dot || !HasOp(*dot, "Dot2(")) continue;
    const auto dot_inputs = SsaValues(*dot);
    if (dot_inputs.size() >= 2 && IsRound(definitions, dot_inputs[0]) &&
        IsRound(definitions, dot_inputs[1]))
      return true;
  }
  return false;
}

bool ComparisonsShareStrictSpatialNoise(
    const Definitions& definitions,
    const std::string& left,
    const std::string& right) {
  const std::string* left_definition = FindDefinition(definitions, left);
  const std::string* right_definition = FindDefinition(definitions, right);
  if (!left_definition || !right_definition ||
      !HasOp(*left_definition, " fcmp ") ||
      !HasOp(*right_definition, " fcmp "))
    return false;

  const auto left_values = SsaValues(*left_definition);
  const auto right_values = SsaValues(*right_definition);
  for (const auto& candidate : left_values) {
    if (std::find(right_values.begin(), right_values.end(), candidate) ==
        right_values.end())
      continue;
    std::unordered_set<std::string> visiting;
    if (IsStrictSpatialNoise(definitions, candidate, visiting)) return true;
  }
  return false;
}

bool ReachesStrictSpatialDitherSelect(
    const Definitions& definitions,
    const std::string& value,
    std::unordered_set<std::string>& visiting,
    std::uint32_t depth) {
  if (depth == 0 || !visiting.insert(value).second) return false;
  const std::string* definition = FindDefinition(definitions, value);
  if (!definition) return false;

  if (HasOp(*definition, " select i1 ")) {
    const auto values = SsaValues(*definition);
    return values.size() >= 3 &&
        ComparisonsShareStrictSpatialNoise(
            definitions, values[values.size() - 2], values.back());
  }

  const bool is_bridge = HasOp(*definition, " fcmp ") ||
      HasOp(*definition, " fadd ") || HasOp(*definition, " fsub ") ||
      HasOp(*definition, " fmul ") || HasOp(*definition, " fdiv ") ||
      HasOp(*definition, " uitofp ") || HasOp(*definition, " sitofp ") ||
      HasOp(*definition, " fptrunc ") || HasOp(*definition, " fpext ");
  if (!is_bridge) return false;

  for (const auto& input : SsaValues(*definition)) {
    if (ReachesStrictSpatialDitherSelect(
            definitions, input, visiting, depth - 1))
      return true;
  }
  return false;
}

bool IsStrictSpatialDitherDiscard(
    const Definitions& definitions,
    const std::string& discard_line) {
  const auto values = SsaValues(discard_line);
  if (values.empty()) return false;
  std::unordered_set<std::string> visiting;
  return ReachesStrictSpatialDitherSelect(
      definitions, values.back(), visiting, 16);
}

} // namespace

SpatialDitherDiagnostic AnalyzeSpatialDitherDiagnostic(
    const std::string& llvm_ir) {
  SpatialDitherDiagnostic diagnostic;
  const Definitions definitions = ParseDefinitions(llvm_ir);

  std::istringstream stream(llvm_ir);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.find("call void @dx.op.discard(") == std::string::npos)
      continue;
    ++diagnostic.discard_calls;
    if (IsStrictSpatialDitherDiscard(definitions, line))
      ++diagnostic.strict_spatial_dither_discards;
  }

  if (diagnostic.discard_calls == 0) {
    diagnostic.classification = SpatialDitherClassification::NoDiscard;
  } else if (diagnostic.strict_spatial_dither_discards == 0) {
    diagnostic.classification = SpatialDitherClassification::NonSpatialDiscard;
  } else if (diagnostic.strict_spatial_dither_discards == 1) {
    diagnostic.classification = SpatialDitherClassification::StrictSpatialDither;
  } else {
    diagnostic.classification =
        SpatialDitherClassification::AmbiguousStrictSpatialDither;
  }
  return diagnostic;
}

const char* SpatialDitherClassificationName(
    SpatialDitherClassification classification) noexcept {
  switch (classification) {
    case SpatialDitherClassification::NoDiscard:
      return "no_discard";
    case SpatialDitherClassification::NonSpatialDiscard:
      return "non_spatial_discard";
    case SpatialDitherClassification::StrictSpatialDither:
      return "strict_spatial_dither";
    case SpatialDitherClassification::AmbiguousStrictSpatialDither:
      return "ambiguous_strict_spatial_dither";
  }
  return "unknown";
}

} // namespace wuwa_tfr

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wuwa_tfr {
namespace {

struct Instruction {
  std::string raw;
  std::string lhs;
  std::string rhs;
};

struct Module {
  std::string_view text;
  std::vector<Instruction> instructions;
  std::unordered_map<std::string, std::size_t> definitions;
  std::unordered_map<std::string, std::vector<std::size_t>> users;
};

bool IsSsaCharacter(char value) noexcept {
  return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
      value == '_' || value == '.' || value == '$' || value == '-';
}

std::size_t FindKeyword(std::string_view text, std::string_view keyword) {
  for (std::size_t offset = text.find(keyword); offset != std::string_view::npos;
       offset = text.find(keyword, offset + keyword.size())) {
    const bool has_left_boundary =
        offset == 0 || !IsSsaCharacter(text[offset - 1]);
    const std::size_t end = offset + keyword.size();
    const bool has_right_boundary =
        end == text.size() || !IsSsaCharacter(text[end]);
    if (has_left_boundary && has_right_boundary) return offset;
  }
  return std::string_view::npos;
}

std::string_view Trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
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
    if (index == text.size()) break;
    --index;
  }
  return values;
}

Module ParseModule(const std::string& llvm_ir) {
  Module module;
  module.text = llvm_ir;
  std::istringstream stream(llvm_ir);
  std::string line;
  while (std::getline(stream, line)) {
    Instruction instruction;
    instruction.raw = line;
    const std::string_view trimmed = Trim(line);
    const std::size_t equals = trimmed.find(" = ");
    if (equals != std::string_view::npos) {
      const std::string_view candidate_lhs = Trim(trimmed.substr(0, equals));
      if (!candidate_lhs.empty() && candidate_lhs.front() == '%') {
        instruction.lhs = std::string(candidate_lhs);
        instruction.rhs = std::string(trimmed.substr(equals + 3));
      }
    }
    const std::size_t index = module.instructions.size();
    if (!instruction.lhs.empty()) module.definitions.emplace(instruction.lhs, index);
    for (const std::string& value : SsaValues(instruction.raw))
      module.users[value].push_back(index);
    module.instructions.push_back(std::move(instruction));
  }
  return module;
}

const Instruction* Definition(const Module& module, std::string_view value) {
  const auto found = module.definitions.find(std::string(value));
  return found == module.definitions.end() ? nullptr :
      &module.instructions[found->second];
}

std::unordered_set<std::string> BackwardSlice(
    const Module& module,
    std::string_view root,
    std::size_t limit = 2048) {
  std::unordered_set<std::string> slice;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty() && slice.size() < limit) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!slice.insert(value).second) continue;
    const Instruction* definition = Definition(module, value);
    if (!definition) continue;
    for (std::string& dependency : SsaValues(definition->rhs))
      pending.push_back(std::move(dependency));
  }
  return slice;
}

bool SliceContains(
    const Module& module,
    const std::unordered_set<std::string>& slice,
    std::string_view needle) {
  for (const std::string& value : slice) {
    const Instruction* definition = Definition(module, value);
    if (definition && definition->rhs.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

std::size_t SliceCount(
    const Module& module,
    const std::unordered_set<std::string>& slice,
    std::string_view needle) {
  std::size_t count = 0;
  for (const std::string& value : slice) {
    const Instruction* definition = Definition(module, value);
    if (definition && definition->rhs.find(needle) != std::string::npos)
      ++count;
  }
  return count;
}

struct PhiArm {
  std::string value;
  std::string predecessor;
};

bool ParsePhiArms(std::string_view rhs, PhiArm& first, PhiArm& second) {
  if (!rhs.starts_with("phi float ")) return false;
  std::array<PhiArm*, 2> arms = {&first, &second};
  std::size_t cursor = 0;
  for (PhiArm* arm : arms) {
    const std::size_t begin = rhs.find('[', cursor);
    const std::size_t comma = rhs.find(',', begin);
    const std::size_t end = rhs.find(']', comma);
    if (begin == std::string_view::npos || comma == std::string_view::npos ||
        end == std::string_view::npos)
      return false;
    arm->value = std::string(Trim(rhs.substr(begin + 1, comma - begin - 1)));
    arm->predecessor = std::string(Trim(rhs.substr(comma + 1, end - comma - 1)));
    if (arm->value.empty() || arm->predecessor.empty() ||
        arm->predecessor.front() != '%')
      return false;
    cursor = end + 1;
  }
  return rhs.find('[', cursor) == std::string_view::npos;
}

bool IsIdentityOne(std::string_view value) {
  return value == "1.000000e+00" || value == "1.0" || value == "1.000000";
}

bool HasCbufferControlledGate(
    const Module& module,
    std::string_view enabled_predecessor) {
  for (const Instruction& instruction : module.instructions) {
    const std::string_view line = Trim(instruction.raw);
    if (!line.starts_with("br i1 ") ||
        line.find("label " + std::string(enabled_predecessor)) ==
            std::string_view::npos)
      continue;
    const auto values = SsaValues(line);
    if (values.empty()) continue;
    const auto gate_slice = BackwardSlice(module, values.front());
    if (SliceContains(module, gate_slice, "@dx.op.cbufferLoad") ||
        SliceContains(module, gate_slice, "@dx.op.cbufferLoadLegacy"))
      return true;
  }
  return false;
}

bool IsNineElementThresholdAccess(const Instruction& instruction) {
  return instruction.rhs.find("getelementptr") != std::string::npos &&
      instruction.rhs.find("[9 x float]") != std::string::npos;
}

bool ReferencedGlobalSymbol(
    const Instruction& instruction, std::string& symbol) {
  for (std::size_t offset = 0; offset < instruction.rhs.size(); ++offset) {
    if (instruction.rhs[offset] != '@') continue;
    const std::size_t begin = offset;
    ++offset;
    while (offset < instruction.rhs.size() &&
        IsSsaCharacter(instruction.rhs[offset]))
      ++offset;
    if (offset == begin + 1 || !symbol.empty()) return false;
    symbol.assign(instruction.rhs.substr(begin, offset - begin));
    if (offset == instruction.rhs.size()) break;
    --offset;
  }
  return !symbol.empty();
}

bool IsNineElementFloatType(std::string_view value_type) {
  constexpr std::string_view kNineElementFloat = "[9 x float]";
  if (!value_type.starts_with(kNineElementFloat)) return false;
  if (value_type.size() == kNineElementFloat.size()) return true;
  const char next = value_type[kNineElementFloat.size()];
  return std::isspace(static_cast<unsigned char>(next)) != 0 || next == ',';
}

bool ReferencesNineElementThresholdGlobal(
    const Module& module, const Instruction& threshold_access) {
  std::string symbol;
  if (!ReferencedGlobalSymbol(threshold_access, symbol)) return false;

  bool found_declaration = false;
  bool has_expected_type = false;
  for (const Instruction& instruction : module.instructions) {
    const std::string_view line = Trim(instruction.raw);
    if (!line.starts_with(symbol)) continue;
    const std::string_view after_symbol = Trim(line.substr(symbol.size()));
    if (!after_symbol.starts_with("=")) continue;
    if (found_declaration) return false;
    found_declaration = true;

    const std::string_view declaration = Trim(after_symbol.substr(1));
    const std::size_t constant = FindKeyword(declaration, "constant");
    const std::size_t global = FindKeyword(declaration, "global");
    const std::size_t storage = std::min(constant, global);
    if (storage == std::string_view::npos) return false;
    has_expected_type = IsNineElementFloatType(
        Trim(declaration.substr(storage +
            (storage == constant ? std::string_view("constant").size() :
                                  std::string_view("global").size()))));
  }
  return found_declaration && has_expected_type;
}

bool IsScreenSpaceThreeByThreeIndex(
    const Module& module,
    const Instruction& threshold_access) {
  const auto values = SsaValues(threshold_access.rhs);
  if (values.empty()) return false;
  const auto index_slice = BackwardSlice(module, values.back());
  const bool has_modulo = SliceCount(module, index_slice, "srem i32") >= 2 &&
      SliceCount(module, index_slice, ", 3") >= 3;
  const bool has_grid_compose = SliceContains(module, index_slice, "mul ") &&
      SliceContains(module, index_slice, "add ");
  const bool has_two_coordinate_conversions =
      SliceCount(module, index_slice, "fptosi") +
          SliceCount(module, index_slice, "fptoui") >= 2;
  const bool has_two_input_coordinates =
      SliceCount(module, index_slice, "@dx.op.loadInput.f32") >= 2;
  return has_modulo && has_grid_compose && has_two_coordinate_conversions &&
      has_two_input_coordinates && module.text.find("SV_Position") !=
          std::string_view::npos;
}

bool IsCoverageExpression(
    const Module& module,
    std::string_view enabled_value,
    std::string_view threshold_pointer) {
  const auto coverage_slice = BackwardSlice(module, enabled_value);
  bool has_threshold_load = false;
  for (const std::string& value : coverage_slice) {
    const Instruction* instruction = Definition(module, value);
    if (instruction && instruction->rhs.find("load float") != std::string::npos &&
        instruction->rhs.find(threshold_pointer) != std::string::npos) {
      has_threshold_load = true;
      break;
    }
  }
  return has_threshold_load && SliceContains(module, coverage_slice, "fsub") &&
      SliceContains(module, coverage_slice, "fmul") &&
      SliceContains(module, coverage_slice, "2.000000") &&
      SliceContains(module, coverage_slice, "FMax(") &&
      SliceContains(module, coverage_slice, "FMin(") &&
      SliceContains(module, coverage_slice, "fadd");
}

FadePrimitiveConsumer ClassifyConsumers(
    const Module& module,
    std::string_view root) {
  bool reaches_discard = false;
  bool reaches_target_alpha = false;
  bool reaches_other_output = false;
  std::unordered_set<std::string> visited;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty() && visited.size() < 4096) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(value).second) continue;
    const auto found = module.users.find(value);
    if (found == module.users.end()) continue;
    for (const std::size_t user_index : found->second) {
      const Instruction& user = module.instructions[user_index];
      if (user.raw.find("@dx.op.discard(") != std::string::npos)
        reaches_discard = true;
      if (user.raw.find("@dx.op.storeOutput.f32") != std::string::npos) {
        if (user.raw.find("i8 3") != std::string::npos &&
            module.text.find("SV_Target") != std::string_view::npos) {
          reaches_target_alpha = true;
        } else {
          reaches_other_output = true;
        }
      }
      if (!user.lhs.empty()) pending.push_back(user.lhs);
    }
  }
  if (reaches_discard && reaches_target_alpha)
    return FadePrimitiveConsumer::DiscardAndSvTargetAlpha;
  if (reaches_discard) return FadePrimitiveConsumer::Discard;
  if (reaches_target_alpha) return FadePrimitiveConsumer::SvTargetAlpha;
  if (reaches_other_output) return FadePrimitiveConsumer::OtherVisibilityOrOutput;
  return FadePrimitiveConsumer::Unknown;
}

} // namespace

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir) {
  FadePrimitiveDiagnostic diagnostic;
  if (llvm_ir.find("SV_Position") == std::string::npos) return diagnostic;
  const Module module = ParseModule(llvm_ir);

  // Start at the rare 9-element table accesses, not at every phi in a large
  // material shader. This preserves the same fail-closed structure while
  // avoiding a per-phi graph walk in the Dev create-pipeline callback.
  std::vector<const Instruction*> threshold_accesses;
  for (const Instruction& instruction : module.instructions) {
    if (IsNineElementThresholdAccess(instruction) &&
        IsScreenSpaceThreeByThreeIndex(module, instruction))
      threshold_accesses.push_back(&instruction);
  }

  for (const Instruction* threshold_access : threshold_accesses) {
    for (const Instruction& phi : module.instructions) {
      if (phi.lhs.empty()) continue;
      PhiArm left;
      PhiArm right;
      if (!ParsePhiArms(phi.rhs, left, right)) continue;
      const PhiArm* disabled = IsIdentityOne(left.value) ? &left :
          IsIdentityOne(right.value) ? &right : nullptr;
      const PhiArm* enabled = disabled == &left ? &right :
          disabled == &right ? &left : nullptr;
      if (!disabled || !enabled || enabled->value.front() != '%') continue;

      const auto enabled_slice = BackwardSlice(module, enabled->value);
      if (!enabled_slice.contains(threshold_access->lhs) ||
          SliceCount(module, enabled_slice, "getelementptr") != 1 ||
          !IsCoverageExpression(
              module, enabled->value, threshold_access->lhs) ||
          !HasCbufferControlledGate(module, enabled->predecessor) ||
          !ReferencesNineElementThresholdGlobal(module, *threshold_access))
        continue;

      diagnostic.instances.push_back(
          {ClassifyConsumers(module, phi.lhs), phi.lhs});
    }
  }
  return diagnostic;
}

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept {
  switch (consumer) {
    case FadePrimitiveConsumer::Unknown: return "unknown";
    case FadePrimitiveConsumer::Discard: return "discard";
    case FadePrimitiveConsumer::SvTargetAlpha: return "sv_target_alpha";
    case FadePrimitiveConsumer::DiscardAndSvTargetAlpha:
      return "discard_and_sv_target_alpha";
    case FadePrimitiveConsumer::OtherVisibilityOrOutput:
      return "other_visibility_or_output";
  }
  return "unknown";
}

} // namespace wuwa_tfr

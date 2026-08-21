// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wuwa_tfr {
namespace {

constexpr std::size_t kBackwardSliceLimit = 2048;
constexpr std::size_t kConsumerTraversalLimit = 4096;

struct Instruction {
  std::string raw;
  std::string lhs;
  std::string rhs;
  std::size_t start = 0;
  std::size_t end = 0;
};

struct Function {
  std::string identity;
  std::size_t start = 0;
  std::size_t end = 0;
  bool complete = true;
  std::vector<Instruction> instructions;
  std::unordered_map<std::string, std::size_t> definitions;
  std::unordered_map<std::string, std::vector<std::size_t>> users;
};

struct Module {
  std::string_view text;
  bool complete = true;
  std::vector<std::string_view> globals;
  std::vector<Function> functions;
};

bool IsSsaCharacter(char value) noexcept {
  return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
      value == '_' || value == '.' || value == '$' || value == '-';
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
  const std::size_t open = at == std::string_view::npos ? at :
      header.find('(', at);
  if (at == std::string_view::npos || open == std::string_view::npos ||
      open == at + 1) return {};
  for (std::size_t index = at + 1; index < open; ++index)
    if (!IsSsaCharacter(header[index])) return {};
  return std::string(header.substr(at, open - at));
}

void AddInstruction(Function& function, std::string_view raw,
    std::size_t start, std::size_t end) {
  Instruction instruction;
  instruction.raw = std::string(raw);
  instruction.start = start;
  instruction.end = end;
  const std::string_view trimmed = Trim(raw);
  const std::size_t equals = trimmed.find(" = ");
  if (equals != std::string_view::npos) {
    const std::string_view lhs = Trim(trimmed.substr(0, equals));
    if (!lhs.empty() && lhs.front() == '%') {
      instruction.lhs = std::string(lhs);
      instruction.rhs = std::string(trimmed.substr(equals + 3));
    }
  }
  const std::size_t index = function.instructions.size();
  if (!instruction.lhs.empty() &&
      !function.definitions.emplace(instruction.lhs, index).second)
    function.complete = false;
  for (const std::string& value : SsaValues(instruction.raw))
    function.users[value].push_back(index);
  function.instructions.push_back(std::move(instruction));
}

Module ParseModule(const std::string& llvm_ir) {
  Module module;
  module.text = llvm_ir;
  Function* current = nullptr;
  std::unordered_set<std::string> identities;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end = newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line(llvm_ir.data() + start, end - start);
    const std::string_view trimmed = Trim(line);
    if (!current) {
      if (trimmed.starts_with("define ")) {
        const std::size_t brace = trimmed.find('{');
        const std::string identity = FunctionIdentity(trimmed);
        if (brace == std::string_view::npos || identity.empty() ||
            !Trim(trimmed.substr(brace + 1)).empty() ||
            !identities.insert(identity).second) {
          module.complete = false;
        } else {
          module.functions.push_back({});
          current = &module.functions.back();
          current->identity = identity;
          current->start = start;
        }
      } else if (trimmed == "}") {
        module.complete = false;
      } else {
        module.globals.push_back(line);
      }
    } else if (trimmed.starts_with("define ") || trimmed == "{") {
      current->complete = false;
    } else if (trimmed == "}") {
      current->end = end;
      current = nullptr;
    } else {
      AddInstruction(*current, line, start, end);
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  if (current) {
    current->complete = false;
    module.complete = false;
  }
  return module;
}

const Instruction* Definition(const Function& function, std::string_view value) {
  const auto found = function.definitions.find(std::string(value));
  return found == function.definitions.end() ? nullptr :
      &function.instructions[found->second];
}

struct Slice {
  std::unordered_set<std::string> values;
  bool complete = true;
};

Slice BackwardSlice(const Function& function, std::string_view root,
    std::size_t limit = kBackwardSliceLimit) {
  Slice slice;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty()) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!slice.values.insert(value).second) continue;
    const Instruction* definition = Definition(function, value);
    if (definition) {
      for (std::string dependency : SsaValues(definition->rhs))
        pending.push_back(std::move(dependency));
    }
    if (slice.values.size() >= limit && !pending.empty()) {
      slice.complete = false;
      return slice;
    }
  }
  return slice;
}

bool SliceContains(const Function& function, const Slice& slice,
    std::string_view needle) {
  if (!slice.complete) return false;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && definition->rhs.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::size_t SliceCount(const Function& function, const Slice& slice,
    std::string_view needle) {
  if (!slice.complete) return 0;
  std::size_t count = 0;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && definition->rhs.find(needle) != std::string::npos) ++count;
  }
  return count;
}

struct PhiArm { std::string value; std::string predecessor; };

bool ParsePhiArms(std::string_view rhs, PhiArm& first, PhiArm& second) {
  if (!rhs.starts_with("phi float ")) return false;
  std::array<PhiArm*, 2> arms = {&first, &second};
  std::size_t cursor = 0;
  for (PhiArm* arm : arms) {
    const std::size_t begin = rhs.find('[', cursor);
    const std::size_t comma = rhs.find(',', begin);
    const std::size_t end = rhs.find(']', comma);
    if (begin == std::string_view::npos || comma == std::string_view::npos ||
        end == std::string_view::npos) return false;
    arm->value = std::string(Trim(rhs.substr(begin + 1, comma - begin - 1)));
    arm->predecessor = std::string(Trim(rhs.substr(comma + 1, end - comma - 1)));
    if (arm->value.empty() || arm->predecessor.empty() ||
        arm->predecessor.front() != '%') return false;
    cursor = end + 1;
  }
  return rhs.find('[', cursor) == std::string_view::npos;
}

bool IsIdentityOne(std::string_view value) {
  return value == "1.000000e+00" || value == "1.0" || value == "1.000000";
}

bool IsSsaValue(std::string_view value) noexcept {
  if (value.size() < 2 || value.front() != '%') return false;
  for (std::size_t index = 1; index < value.size(); ++index)
    if (!IsSsaCharacter(value[index])) return false;
  return true;
}

bool HasOnlyMetadataAttachments(std::string_view trailing) noexcept {
  std::size_t cursor = 0;
  while (cursor < trailing.size()) {
    if (trailing[cursor] != ',') return false;
    ++cursor;
    while (cursor < trailing.size() &&
        std::isspace(static_cast<unsigned char>(trailing[cursor])) != 0)
      ++cursor;

    if (cursor == trailing.size() || trailing[cursor] != '!') return false;
    const std::size_t name_start = ++cursor;
    while (cursor < trailing.size() && IsSsaCharacter(trailing[cursor]))
      ++cursor;
    if (cursor == name_start || cursor == trailing.size() ||
        std::isspace(static_cast<unsigned char>(trailing[cursor])) == 0)
      return false;
    while (cursor < trailing.size() &&
        std::isspace(static_cast<unsigned char>(trailing[cursor])) != 0)
      ++cursor;

    if (cursor == trailing.size() || trailing[cursor] != '!') return false;
    const std::size_t reference_start = ++cursor;
    while (cursor < trailing.size() &&
        std::isdigit(static_cast<unsigned char>(trailing[cursor])) != 0)
      ++cursor;
    if (cursor == reference_start) return false;
    while (cursor < trailing.size() &&
        std::isspace(static_cast<unsigned char>(trailing[cursor])) != 0)
      ++cursor;
  }
  return true;
}

bool ParseConditionalBranch(
    std::string_view line,
    std::string_view& condition,
    std::array<std::string_view, 2>& successors) {
  const std::size_t comment = line.find(';');
  line = Trim(line.substr(0, comment));
  if (!line.starts_with("br i1 ")) return false;
  const std::size_t first_comma = line.find(',', std::string_view("br i1 ").size());
  if (first_comma == std::string_view::npos) return false;
  condition = Trim(line.substr(std::string_view("br i1 ").size(),
      first_comma - std::string_view("br i1 ").size()));
  if (!IsSsaValue(condition)) return false;
  std::size_t cursor = first_comma + 1;
  for (std::size_t index = 0; index < successors.size(); ++index) {
    const std::string_view rest = Trim(line.substr(cursor));
    if (!rest.starts_with("label ")) return false;
    const std::string_view after_label = rest.substr(std::string_view("label ").size());
    const std::size_t comma = after_label.find(',');
    const std::string_view token = Trim(after_label.substr(0, comma));
    if (!IsSsaValue(token)) return false;
    successors[index] = token;
    if (index + 1 == successors.size()) {
      const std::string_view trailing = Trim(after_label.substr(
          comma == std::string_view::npos ? after_label.size() : comma));
      return trailing.empty() || HasOnlyMetadataAttachments(trailing);
    }
    if (comma == std::string_view::npos) return false;
    cursor = line.size() - after_label.size() + comma + 1;
  }
  return false;
}

bool HasCbufferControlledGate(const Function& function,
    std::string_view enabled_predecessor) {
  for (const Instruction& instruction : function.instructions) {
    const std::string_view line = Trim(instruction.raw);
    std::string_view condition;
    std::array<std::string_view, 2> successors;
    if (!ParseConditionalBranch(line, condition, successors) ||
        successors[0] != enabled_predecessor && successors[1] != enabled_predecessor)
      continue;
    const Slice gate_slice = BackwardSlice(function, condition);
    if (!gate_slice.complete) return false;
    if (SliceContains(function, gate_slice, "@dx.op.cbufferLoad") ||
        SliceContains(function, gate_slice, "@dx.op.cbufferLoadLegacy")) return true;
  }
  return false;
}

bool IsNineElementThresholdAccess(const Instruction& instruction) {
  return instruction.rhs.find("getelementptr") != std::string::npos &&
      instruction.rhs.find("[9 x float]") != std::string::npos;
}

bool ReferencedGlobalSymbol(const Instruction& instruction, std::string& symbol) {
  for (std::size_t offset = 0; offset < instruction.rhs.size(); ++offset) {
    if (instruction.rhs[offset] != '@') continue;
    const std::size_t begin = offset++;
    while (offset < instruction.rhs.size() && IsSsaCharacter(instruction.rhs[offset])) ++offset;
    if (offset == begin + 1 || !symbol.empty()) return false;
    symbol.assign(instruction.rhs.substr(begin, offset - begin));
    if (offset == instruction.rhs.size()) break;
    --offset;
  }
  return !symbol.empty();
}

bool IsNineElementFloatType(std::string_view value_type) {
  constexpr std::string_view type = "[9 x float]";
  if (!value_type.starts_with(type)) return false;
  return value_type.size() == type.size() ||
      std::isspace(static_cast<unsigned char>(value_type[type.size()])) != 0 ||
      value_type[type.size()] == ',';
}

bool ReferencesNineElementThresholdGlobal(const Module& module,
    const Instruction& threshold_access) {
  std::string symbol;
  if (!ReferencedGlobalSymbol(threshold_access, symbol)) return false;
  bool found = false;
  for (std::string_view raw : module.globals) {
    const std::string_view line = Trim(raw);
    if (!line.starts_with(symbol)) continue;
    const std::string_view after = Trim(line.substr(symbol.size()));
    if (!after.starts_with("=")) continue;
    if (found) return false;
    found = true;
    const std::string_view declaration = Trim(after.substr(1));
    const std::size_t constant = declaration.find("constant");
    const std::size_t global = declaration.find("global");
    const std::size_t storage = std::min(constant, global);
    if (storage == std::string_view::npos) return false;
    const std::string_view keyword = storage == constant ? "constant" : "global";
    if (!IsNineElementFloatType(Trim(declaration.substr(storage + keyword.size()))))
      return false;
  }
  return found;
}

bool IsScreenSpaceThreeByThreeIndex(const Function& function,
    const Instruction& threshold_access, const Module& module) {
  const auto values = SsaValues(threshold_access.rhs);
  if (values.empty()) return false;
  const Slice index_slice = BackwardSlice(function, values.back());
  if (!index_slice.complete) return false;
  return SliceCount(function, index_slice, "srem i32") >= 2 &&
      SliceCount(function, index_slice, ", 3") >= 3 &&
      SliceContains(function, index_slice, "mul ") &&
      SliceContains(function, index_slice, "add ") &&
      SliceCount(function, index_slice, "fptosi") +
          SliceCount(function, index_slice, "fptoui") >= 2 &&
      SliceCount(function, index_slice, "@dx.op.loadInput.f32") >= 2 &&
      module.text.find("SV_Position") != std::string_view::npos;
}

bool IsFloatLoadFromPointer(
    const Instruction& instruction, std::string_view expected_pointer) {
  constexpr std::string_view kLoadPrefix = "load float,";
  const std::string_view rhs = Trim(instruction.rhs);
  if (!rhs.starts_with(kLoadPrefix)) return false;
  const std::string_view operands = Trim(rhs.substr(kLoadPrefix.size()));
  const std::size_t comma = operands.find(',');
  const std::string_view pointer_operand = Trim(operands.substr(0, comma));
  constexpr std::string_view kPointerType = "float*";
  if (!pointer_operand.starts_with(kPointerType)) return false;
  const std::string_view pointer_value = Trim(
      pointer_operand.substr(kPointerType.size()));
  const auto values = SsaValues(pointer_value);
  // A load has exactly one pointer operand. Anything else is malformed or
  // ambiguous evidence and must not authorize a rewrite.
  return values.size() == 1 && values.front() == expected_pointer &&
      pointer_value == expected_pointer;
}

bool IsCoverageExpression(const Function& function, std::string_view enabled_value,
    std::string_view threshold_pointer) {
  const Slice coverage_slice = BackwardSlice(function, enabled_value);
  if (!coverage_slice.complete) return false;
  bool has_threshold_load = false;
  for (const std::string& value : coverage_slice.values) {
    const Instruction* instruction = Definition(function, value);
    if (instruction && IsFloatLoadFromPointer(*instruction, threshold_pointer)) {
      has_threshold_load = true;
      break;
    }
  }
  return has_threshold_load && SliceContains(function, coverage_slice, "fsub") &&
      SliceContains(function, coverage_slice, "fmul") &&
      SliceContains(function, coverage_slice, "2.000000") &&
      SliceContains(function, coverage_slice, "FMax(") &&
      SliceContains(function, coverage_slice, "FMin(") &&
      SliceContains(function, coverage_slice, "fadd");
}

struct ConsumerAnalysis { FadePrimitiveConsumer consumer; bool complete = true; };

struct StoreOutputCall {
  std::uint32_t signature = 0;
  std::uint32_t row = 0;
  std::uint32_t column = 0;
  std::string value;
};

void SkipWhitespace(std::string_view text, std::size_t& cursor) noexcept {
  while (cursor < text.size() &&
      std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
    ++cursor;
}

bool Consume(std::string_view text, std::size_t& cursor,
    std::string_view expected) noexcept {
  if (!text.substr(cursor).starts_with(expected)) return false;
  cursor += expected.size();
  return true;
}

bool ParseUnsigned(std::string_view text, std::size_t& cursor,
    std::uint32_t& value) noexcept {
  SkipWhitespace(text, cursor);
  const std::size_t start = cursor;
  std::uint64_t parsed = 0;
  while (cursor < text.size() &&
      std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
    parsed = parsed * 10 + static_cast<unsigned>(text[cursor] - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
    ++cursor;
  }
  if (cursor == start) return false;
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseTypedUnsigned(std::string_view text, std::size_t& cursor,
    std::string_view type, std::uint32_t& value) noexcept {
  SkipWhitespace(text, cursor);
  if (!Consume(text, cursor, type) || cursor == text.size() ||
      std::isspace(static_cast<unsigned char>(text[cursor])) == 0)
    return false;
  return ParseUnsigned(text, cursor, value);
}

bool ConsumeComma(std::string_view text, std::size_t& cursor) noexcept {
  SkipWhitespace(text, cursor);
  if (cursor == text.size() || text[cursor] != ',') return false;
  ++cursor;
  return true;
}

bool ParseStoreOutputF32(std::string_view raw, StoreOutputCall& result) {
  const std::size_t comment = raw.find(';');
  const std::string_view line = Trim(raw.substr(0, comment));
  constexpr std::string_view prefix =
      "call void @dx.op.storeOutput.f32(";
  if (!line.starts_with(prefix)) return false;

  std::size_t cursor = prefix.size();
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(line, cursor, "i32", opcode) || opcode != 5 ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i32", result.signature) ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i32", result.row) ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i8", result.column) ||
      !ConsumeComma(line, cursor))
    return false;

  SkipWhitespace(line, cursor);
  constexpr std::string_view float_type = "float";
  if (!Consume(line, cursor, float_type) || cursor == line.size() ||
      std::isspace(static_cast<unsigned char>(line[cursor])) == 0)
    return false;
  SkipWhitespace(line, cursor);
  const std::size_t value_start = cursor;
  while (cursor < line.size() && line[cursor] != ')' && line[cursor] != ',')
    ++cursor;
  const std::string_view value = Trim(
      line.substr(value_start, cursor - value_start));
  if (!IsSsaValue(value) || cursor == line.size() || line[cursor] != ')')
    return false;
  result.value = std::string(value);
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(line.substr(cursor)));
}

bool IsSvTargetSignature(const Module& module, std::uint32_t signature) {
  const std::string prefix = "!{i32 " + std::to_string(signature) +
      ", !\"SV_Target\",";
  bool found = false;
  for (const std::string_view raw : module.globals) {
    const std::string_view line = Trim(raw);
    if (line.size() < 2 || line.front() != '!' ||
        std::isdigit(static_cast<unsigned char>(line[1])) == 0)
      continue;
    std::size_t definition_end = 2;
    while (definition_end < line.size() &&
        std::isdigit(static_cast<unsigned char>(line[definition_end])) != 0)
      ++definition_end;
    const std::size_t equals = line.find(" = ");
    if (equals != definition_end ||
        !line.substr(equals + 3).starts_with(prefix))
      continue;
    if (found) return false;
    found = true;
  }
  return found;
}

ConsumerAnalysis ClassifyConsumers(const Function& function, const Module& module,
    std::string_view root) {
  bool discard = false, target_alpha = false, other_output = false;
  bool has_rgb_signature = false;
  std::uint32_t rgb_signature = 0;
  unsigned rgb_columns = 0;
  std::unordered_set<std::size_t> classified_outputs;
  std::unordered_set<std::string> visited;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty()) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(value).second) continue;
    const auto found = function.users.find(value);
    if (found != function.users.end()) for (const std::size_t index : found->second) {
      const Instruction& user = function.instructions[index];
      if (user.raw.find("@dx.op.discard(") != std::string::npos) discard = true;
      if (user.raw.find("@dx.op.storeOutput.f32") != std::string::npos &&
          classified_outputs.insert(index).second) {
        if (user.raw.find("i8 3") != std::string::npos &&
            module.text.find("SV_Target") != std::string_view::npos) target_alpha = true;
        else {
          StoreOutputCall output;
          if (!ParseStoreOutputF32(user.raw, output) || output.value != value ||
              output.row != 0 || output.column >= 3 ||
              !IsSvTargetSignature(module, output.signature) ||
              (has_rgb_signature && output.signature != rgb_signature) ||
              (rgb_columns & (1u << output.column)) != 0) {
            other_output = true;
          } else {
            has_rgb_signature = true;
            rgb_signature = output.signature;
            rgb_columns |= 1u << output.column;
          }
        }
      }
      if (!user.lhs.empty()) pending.push_back(user.lhs);
    }
    if (visited.size() >= kConsumerTraversalLimit && !pending.empty())
      return {FadePrimitiveConsumer::Unknown, false};
  }
  const bool target_rgb = has_rgb_signature && rgb_columns == 0x7u;
  if (has_rgb_signature && !target_rgb) other_output = true;
  if (other_output || (target_rgb && (discard || target_alpha)))
    return {FadePrimitiveConsumer::OtherVisibilityOrOutput};
  if (target_rgb) return {FadePrimitiveConsumer::SvTargetRgb};
  if (discard && target_alpha) return {FadePrimitiveConsumer::DiscardAndSvTargetAlpha};
  if (discard) return {FadePrimitiveConsumer::Discard};
  if (target_alpha) return {FadePrimitiveConsumer::SvTargetAlpha};
  if (other_output) return {FadePrimitiveConsumer::OtherVisibilityOrOutput};
  return {FadePrimitiveConsumer::Unknown};
}

} // namespace

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir) {
  FadePrimitiveDiagnostic diagnostic;
  if (llvm_ir.find("SV_Position") == std::string::npos) return diagnostic;
  const Module module = ParseModule(llvm_ir);
  if (!module.complete) return diagnostic;
  for (const Function& function : module.functions) {
    if (!function.complete) continue;
    for (const Instruction& threshold : function.instructions) {
      if (!IsNineElementThresholdAccess(threshold) || threshold.lhs.empty() ||
          !IsScreenSpaceThreeByThreeIndex(function, threshold, module) ||
          !ReferencesNineElementThresholdGlobal(module, threshold)) continue;
      for (const Instruction& phi : function.instructions) {
        if (phi.lhs.empty()) continue;
        PhiArm left, right;
        if (!ParsePhiArms(phi.rhs, left, right)) continue;
        const PhiArm* disabled = IsIdentityOne(left.value) ? &left :
            IsIdentityOne(right.value) ? &right : nullptr;
        const PhiArm* enabled = disabled == &left ? &right :
            disabled == &right ? &left : nullptr;
        if (!disabled || !enabled || enabled->value.empty() ||
            enabled->value.front() != '%') continue;
        const Slice enabled_slice = BackwardSlice(function, enabled->value);
        if (!enabled_slice.complete || !enabled_slice.values.contains(threshold.lhs) ||
            SliceCount(function, enabled_slice, "getelementptr") != 1 ||
            !IsCoverageExpression(function, enabled->value, threshold.lhs) ||
            !HasCbufferControlledGate(function, enabled->predecessor)) continue;
        const ConsumerAnalysis consumers = ClassifyConsumers(function, module, phi.lhs);
        if (!consumers.complete) continue;
        diagnostic.instances.push_back(
            {consumers.consumer, function.identity, phi.start, phi.end, phi.lhs});
      }
    }
  }
  return diagnostic;
}

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept {
  switch (consumer) {
    case FadePrimitiveConsumer::Unknown: return "unknown";
    case FadePrimitiveConsumer::Discard: return "discard";
    case FadePrimitiveConsumer::SvTargetAlpha: return "sv_target_alpha";
    case FadePrimitiveConsumer::SvTargetRgb: return "sv_target_rgb";
    case FadePrimitiveConsumer::DiscardAndSvTargetAlpha: return "discard_and_sv_target_alpha";
    case FadePrimitiveConsumer::OtherVisibilityOrOutput: return "other_visibility_or_output";
  }
  return "unknown";
}

} // namespace wuwa_tfr

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
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

bool ParseUnsigned(std::string_view text, std::size_t& cursor,
    std::uint32_t& value) noexcept;
bool IsSsaValue(std::string_view value) noexcept;
void SkipWhitespace(std::string_view text, std::size_t& cursor) noexcept;
bool Consume(std::string_view text, std::size_t& cursor,
    std::string_view expected) noexcept;
bool ConsumeComma(std::string_view text, std::size_t& cursor) noexcept;
bool ParseTypedUnsigned(std::string_view text, std::size_t& cursor,
    std::string_view type, std::uint32_t& value) noexcept;
bool HasOnlyMetadataAttachments(std::string_view trailing) noexcept;

struct LoadInputF32Call {
  std::uint32_t signature = 0;
  std::uint32_t row = 0;
  std::uint32_t column = 0;
};

bool ParseLoadInputF32(
    std::string_view line, LoadInputF32Call& result) noexcept;

struct Instruction {
  // Only comment-free LLVM code is used to build the SSA graph or authorize
  // a replacement. The comment remains diagnostic text only.
  std::string code;
  std::string comment;
  std::string lhs;
  std::string rhs;
  std::size_t start = 0;
  std::size_t end = 0;
};

struct GlobalLine {
  std::string code;
  std::string comment;
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
  bool complete = true;
  std::vector<GlobalLine> globals;
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

struct CodeAndComment {
  std::string_view code;
  std::string_view comment;
};

// A semicolon in an LLVM metadata string is not a comment delimiter. Outside
// a quoted string, retain the comment separately and never tokenize it.
CodeAndComment SplitCodeAndComment(std::string_view text) noexcept {
  bool quoted = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (quoted && text[index] == '\\' && index + 1 < text.size()) {
      ++index;
      continue;
    }
    if (text[index] == '"') quoted = !quoted;
    if (!quoted && text[index] == ';')
      return {text.substr(0, index), text.substr(index + 1)};
  }
  return {text, {}};
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
  const CodeAndComment split = SplitCodeAndComment(raw);
  const std::string_view code = Trim(split.code);
  if (code.empty()) return;
  Instruction instruction;
  instruction.code = std::string(code);
  instruction.comment = std::string(Trim(split.comment));
  instruction.start = start;
  instruction.end = end;
  const std::size_t equals = code.find(" = ");
  if (equals != std::string_view::npos) {
    const std::string_view lhs = Trim(code.substr(0, equals));
    if (!lhs.empty() && lhs.front() == '%') {
      instruction.lhs = std::string(lhs);
      instruction.rhs = std::string(code.substr(equals + 3));
    }
  }
  const std::size_t index = function.instructions.size();
  if (!instruction.lhs.empty() &&
      !function.definitions.emplace(instruction.lhs, index).second)
    function.complete = false;
  // For an SSA definition the LHS names the value being defined; it is not
  // one of that instruction's operands and must never become a synthetic
  // self-user. Instructions without an LHS are terminal users, so their full
  // comment-free code remains the operand source.
  const std::string_view operands = instruction.lhs.empty()
      ? std::string_view(instruction.code)
      : std::string_view(instruction.rhs);
  std::unordered_set<std::string> unique_operands;
  for (const std::string& value : SsaValues(operands))
    if (unique_operands.insert(value).second)
      function.users[value].push_back(index);
  function.instructions.push_back(std::move(instruction));
}

Module ParseModule(const std::string& llvm_ir) {
  Module module;
  Function* current = nullptr;
  std::unordered_set<std::string> identities;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end = newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line(llvm_ir.data() + start, end - start);
    const CodeAndComment split = SplitCodeAndComment(line);
    const std::string_view code = Trim(split.code);
    if (!current) {
      if (code.starts_with("define ")) {
        const std::size_t brace = code.find('{');
        const std::string identity = FunctionIdentity(code);
        if (brace == std::string_view::npos || identity.empty() ||
            !Trim(code.substr(brace + 1)).empty() ||
            !identities.insert(identity).second) {
          module.complete = false;
        } else {
          module.functions.push_back({});
          current = &module.functions.back();
          current->identity = identity;
          current->start = start;
        }
      } else if (code == "}") {
        module.complete = false;
      } else if (!code.empty()) {
        module.globals.push_back({std::string(code), std::string(Trim(split.comment))});
      }
    } else if (code.starts_with("define ") || code == "{") {
      current->complete = false;
    } else if (code == "}") {
      current->end = end;
      current = nullptr;
    } else if (!code.empty()) {
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

bool IsIrTokenDelimiter(char value) noexcept {
  return std::isspace(static_cast<unsigned char>(value)) != 0 ||
      value == ',' || value == '(' || value == ')' || value == '[' ||
      value == ']' || value == '{' || value == '}' || value == '=' ||
      value == '*';
}

std::vector<std::string_view> IrTokens(std::string_view code) {
  std::vector<std::string_view> tokens;
  for (std::size_t cursor = 0; cursor < code.size();) {
    if (std::isspace(static_cast<unsigned char>(code[cursor])) != 0) {
      ++cursor;
      continue;
    }
    if (std::string_view(",()[]{}=*").find(code[cursor]) !=
        std::string_view::npos) {
      tokens.emplace_back(code.substr(cursor++, 1));
      continue;
    }
    const std::size_t start = cursor;
    while (cursor < code.size() && !IsIrTokenDelimiter(code[cursor])) ++cursor;
    if (cursor != start) tokens.emplace_back(code.substr(start, cursor - start));
  }
  return tokens;
}

bool HasExactToken(std::string_view code, std::string_view expected) {
  for (const std::string_view token : IrTokens(code))
    if (token == expected) return true;
  return false;
}

bool HasInstructionOpcode(const Instruction& instruction,
    std::string_view opcode) {
  const auto tokens = IrTokens(instruction.rhs);
  return !tokens.empty() && tokens.front() == opcode;
}

bool IsDxOpCallWithOpcode(const Instruction& instruction,
    std::string_view callee, std::uint32_t opcode) {
  const auto tokens = IrTokens(instruction.rhs);
  if (tokens.size() < 7 || tokens[0] != "call" || tokens[2] != callee ||
      tokens[3] != "(" || tokens[4] != "i32")
    return false;
  std::uint32_t parsed = 0;
  std::size_t cursor = 0;
  return ParseUnsigned(tokens[5], cursor, parsed) &&
      cursor == tokens[5].size() && parsed == opcode && tokens[6] == ",";
}

bool SliceHasInstructionOpcode(const Function& function, const Slice& slice,
    std::string_view opcode) {
  if (!slice.complete) return false;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && HasInstructionOpcode(*definition, opcode)) return true;
  }
  return false;
}

std::size_t SliceCountInstructionOpcode(const Function& function,
    const Slice& slice, std::string_view opcode) {
  if (!slice.complete) return 0;
  std::size_t count = 0;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && HasInstructionOpcode(*definition, opcode)) ++count;
  }
  return count;
}

bool SliceHasExactToken(const Function& function, const Slice& slice,
    std::string_view token) {
  if (!slice.complete) return false;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && HasExactToken(definition->rhs, token)) return true;
  }
  return false;
}

std::size_t SliceCountExactToken(const Function& function, const Slice& slice,
    std::string_view token) {
  if (!slice.complete) return 0;
  std::size_t count = 0;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && HasExactToken(definition->rhs, token)) ++count;
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
  line = Trim(line);
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

// ---- best-effort gate-predicate evidence (diagnostic only; see
// FadePrimitiveGatePredicateEvidence's own comment) ----
//
// Structural parsers for the exact same three DXIL call shapes
// pre_fade_fmin_analysis.hpp's ResolveDirectOrigin and
// dev/capture/fade_control_analysis.cpp's ResolveControlSource already
// recognize for other purposes. Deliberately not shared with either: this
// file must never depend on Dev-only code, and depending on the canonical
// shared analyzer here would pull FMin-operand-specific assumptions into
// the matcher's own gate-verification path. Every literal-value coordinate
// below is diagnostic-only -- see the per-field comments -- and a false
// return only ever leaves a coordinate unavailable, never disqualifies an
// otherwise structurally valid call.

bool IsWellFormedIndexOperand(std::string_view text) noexcept {
  if (text.empty()) return false;
  std::size_t cursor = 0;
  std::uint32_t ignored = 0;
  if (ParseUnsigned(text, cursor, ignored) && cursor == text.size()) return true;
  return IsSsaValue(text);
}

struct CbufferLoadLegacyOperands {
  std::string_view handle;
  bool row_resolved = false;
  std::uint32_t row = 0;
};

// call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59,
//     %dx.types.Handle %h, i32 <row>)
bool ParseCbufferLoadLegacyOperands(
    std::string_view rhs, CbufferLoadLegacyOperands& result) noexcept {
  std::size_t cursor = 0;
  if (!Consume(rhs, cursor,
          "call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32("))
    return false;
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(rhs, cursor, "i32", opcode) || opcode != 59 ||
      !ConsumeComma(rhs, cursor))
    return false;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "%dx.types.Handle")) return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t handle_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  const std::string_view handle =
      Trim(rhs.substr(handle_start, cursor - handle_start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  result.handle = handle;
  ++cursor;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "i32") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t row_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  if (cursor == rhs.size()) return false;
  const std::string_view row_text =
      Trim(rhs.substr(row_start, cursor - row_start));
  if (!IsWellFormedIndexOperand(row_text)) return false;
  std::size_t row_cursor = 0;
  result.row_resolved = ParseUnsigned(row_text, row_cursor, result.row) &&
      row_cursor == row_text.size();
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(rhs.substr(cursor)));
}

struct CbufferLoadByteOperands {
  std::string_view handle;
  bool byte_offset_resolved = false;
  std::uint32_t byte_offset = 0;
};

// call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %h,
//     i32 <byteOffset>, i32 <alignment>)
bool ParseCbufferLoadByteOperands(
    std::string_view rhs, CbufferLoadByteOperands& result) noexcept {
  std::size_t cursor = 0;
  if (!Consume(rhs, cursor, "call float @dx.op.cbufferLoad.f32(")) return false;
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(rhs, cursor, "i32", opcode) || opcode != 58 ||
      !ConsumeComma(rhs, cursor))
    return false;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "%dx.types.Handle")) return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t handle_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  const std::string_view handle =
      Trim(rhs.substr(handle_start, cursor - handle_start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  result.handle = handle;
  ++cursor;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "i32") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t offset_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  // dx.op.cbufferLoad is exactly (opcode, handle, byteOffset, alignment) --
  // the alignment operand must follow, so a bare ')' here is not this call.
  if (cursor == rhs.size() || rhs[cursor] != ',') return false;
  const std::string_view offset_text =
      Trim(rhs.substr(offset_start, cursor - offset_start));
  if (!IsWellFormedIndexOperand(offset_text)) return false;
  std::size_t offset_cursor = 0;
  result.byte_offset_resolved =
      ParseUnsigned(offset_text, offset_cursor, result.byte_offset) &&
      offset_cursor == offset_text.size();
  ++cursor;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "i32") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  if (cursor == rhs.size()) return false;
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(rhs.substr(cursor)));
}

// extractvalue %dx.types.CBufRet.f32 %agg, <component> -- the aggregate
// operand only; matches even when the trailing component index itself
// does not parse as the diagnostic-only 0-3 literal ParseExtractValue
// Component below requires.
bool ParseExtractValueAggregate(
    std::string_view rhs, std::string_view& aggregate) noexcept {
  std::size_t cursor = 0;
  if (!Consume(rhs, cursor, "extractvalue %dx.types.CBufRet.f32")) return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t start = cursor;
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos || comma < start) return false;
  const std::string_view candidate = Trim(rhs.substr(start, comma - start));
  if (!IsSsaValue(candidate)) return false;
  aggregate = candidate;
  return true;
}

// Diagnostic-only: the extractvalue's literal component index. A false
// return leaves the component unavailable; it never disqualifies the
// aggregate match above.
bool ParseExtractValueComponent(
    std::string_view rhs, std::uint32_t& component) noexcept {
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view text = Trim(rhs.substr(comma + 1));
  std::size_t cursor = 0;
  std::uint32_t value = 0;
  if (!ParseUnsigned(text, cursor, value) || cursor != text.size() || value > 3)
    return false;
  component = value;
  return true;
}

struct CreateHandleOperands {
  bool resource_class_resolved = false;
  std::uint32_t resource_class = 0;
  bool range_id_resolved = false;
  std::uint32_t range_id = 0;
};

// call %dx.types.Handle @dx.op.createHandle(i32 57, i8 <resourceClass>,
//     i32 <rangeId>, ...) -- only the leading operands this evidence needs
// are validated; the remainder (index, non-uniform flag) is not, since
// nothing here depends on it.
bool ParseCreateHandleOperands(
    std::string_view rhs, CreateHandleOperands& result) noexcept {
  std::size_t cursor = 0;
  if (!Consume(rhs, cursor, "call %dx.types.Handle @dx.op.createHandle("))
    return false;
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(rhs, cursor, "i32", opcode) || opcode != 57 ||
      !ConsumeComma(rhs, cursor))
    return false;
  std::uint32_t resource_class = 0;
  if (!ParseTypedUnsigned(rhs, cursor, "i8", resource_class) ||
      !ConsumeComma(rhs, cursor))
    return false;
  result.resource_class_resolved = true;
  result.resource_class = resource_class;
  std::uint32_t range_id = 0;
  if (ParseTypedUnsigned(rhs, cursor, "i32", range_id)) {
    result.range_id_resolved = true;
    result.range_id = range_id;
  }
  return true;
}

// Extracted from the SAME gate_slice the boolean gate check already
// computed -- never a second backward slice. Requires exactly one direct
// scalar CBV load reachable in the slice, behind a literally-typed CBV
// createHandle; zero or more than one candidate (including an ambiguous
// legacy load with more than one consuming extractvalue) leaves `resolved`
// false rather than guessing.
FadePrimitiveGatePredicateEvidence ResolveGatePredicateEvidence(
    const Function& function, std::string_view condition,
    const Slice& gate_slice) {
  FadePrimitiveGatePredicateEvidence evidence;
  if (IsSsaValue(condition)) {
    evidence.condition_identified = true;
    evidence.condition_value = std::string(condition);
  }

  struct Candidate {
    std::string_view handle;
    bool legacy_form = false;
    bool row_resolved = false;
    std::uint32_t row = 0;
    bool component_resolved = false;
    std::uint32_t component = 0;
    bool byte_offset_resolved = false;
    std::uint32_t byte_offset = 0;
  };

  // Pass 1: every direct-CBV-load-shaped call reachable in the slice.
  // Legacy-form loads are aggregates and only become genuine candidates
  // once pass 2 finds exactly one extractvalue consuming them; byte-form
  // loads are already scalar and are candidates immediately.
  std::unordered_map<std::string_view, CbufferLoadLegacyOperands> legacy_loads;
  std::vector<Candidate> candidates;
  for (const std::string& value : gate_slice.values) {
    const Instruction* definition = Definition(function, value);
    if (!definition) continue;
    CbufferLoadLegacyOperands legacy;
    if (ParseCbufferLoadLegacyOperands(definition->rhs, legacy)) {
      legacy_loads.emplace(std::string_view(definition->lhs), legacy);
      continue;
    }
    CbufferLoadByteOperands byte_form;
    if (ParseCbufferLoadByteOperands(definition->rhs, byte_form)) {
      Candidate candidate;
      candidate.handle = byte_form.handle;
      candidate.byte_offset_resolved = byte_form.byte_offset_resolved;
      candidate.byte_offset = byte_form.byte_offset;
      candidates.push_back(candidate);
    }
  }

  // Pass 2: for each legacy load, exactly one extractvalue reachable in
  // the same slice consuming its result -- more than one is ambiguous and
  // that load is dropped as a candidate entirely.
  std::unordered_map<std::string_view, std::size_t> extract_counts;
  std::unordered_map<std::string_view, std::uint32_t> extract_components;
  std::unordered_map<std::string_view, bool> extract_component_resolved;
  for (const std::string& value : gate_slice.values) {
    const Instruction* definition = Definition(function, value);
    if (!definition) continue;
    std::string_view aggregate;
    if (!ParseExtractValueAggregate(definition->rhs, aggregate)) continue;
    if (!legacy_loads.contains(aggregate)) continue;
    ++extract_counts[aggregate];
    std::uint32_t component = 0;
    extract_component_resolved[aggregate] =
        ParseExtractValueComponent(definition->rhs, component);
    extract_components[aggregate] = component;
  }
  for (const auto& [load_lhs, load] : legacy_loads) {
    const auto count_it = extract_counts.find(load_lhs);
    if (count_it == extract_counts.end() || count_it->second != 1) continue;
    Candidate candidate;
    candidate.handle = load.handle;
    candidate.legacy_form = true;
    candidate.row_resolved = load.row_resolved;
    candidate.row = load.row;
    candidate.component_resolved = extract_component_resolved.at(load_lhs);
    candidate.component = extract_components.at(load_lhs);
    candidates.push_back(candidate);
  }

  if (candidates.size() != 1) return evidence;
  const Candidate& winner = candidates.front();

  // Handle validation: the winning candidate's handle must resolve to a
  // literally-typed createHandle(resourceClass=CBV) call. Structurally
  // unreachable for valid DXIL -- the load opcode itself already implies a
  // CBV handle -- but never assumed; a handle that fails this leaves
  // evidence unresolved entirely rather than reporting a coordinate for a
  // source that was never proven to be a CBV.
  const Instruction* handle_definition = Definition(function, winner.handle);
  if (!handle_definition) return evidence;
  CreateHandleOperands handle_call;
  if (!ParseCreateHandleOperands(handle_definition->rhs, handle_call) ||
      !handle_call.resource_class_resolved || handle_call.resource_class != 2)
    return evidence;

  evidence.resolved = true;
  evidence.handle_value = std::string(winner.handle);
  evidence.legacy_form = winner.legacy_form;
  evidence.range_id_resolved = handle_call.range_id_resolved;
  evidence.range_id = handle_call.range_id;
  evidence.row_resolved = winner.row_resolved;
  evidence.row = winner.row;
  evidence.component_resolved = winner.component_resolved;
  evidence.component = winner.component;
  evidence.byte_offset_resolved = winner.byte_offset_resolved;
  evidence.byte_offset = winner.byte_offset;
  return evidence;
}

struct GateVerification {
  bool gate_proven = false;
  FadePrimitiveGatePredicateEvidence evidence;
};

// Preserves the exact original boolean semantics of what was
// HasCbufferControlledGate() while additionally deriving best-effort
// predicate evidence from the SAME gate branch and backward slice, without
// a second analysis pass. `gate_proven` is byte-for-byte the same decision
// the old bare-bool function made: exactly the first (in instruction
// order) candidate branch into `enabled_predecessor` whose backward slice
// is complete and reachably contains a cbufferLoadLegacy.f32 or
// cbufferLoad.f32 call. An incomplete slice on that first candidate still
// aborts immediately without considering any later candidate, exactly as
// before -- this refactor changes nothing about when an instance is
// accepted or rejected, only what is additionally recorded when it is.
GateVerification VerifyCbufferControlledGate(
    const Function& function, std::string_view enabled_predecessor) {
  GateVerification result;
  for (const Instruction& instruction : function.instructions) {
    const std::string_view line = instruction.code;
    std::string_view condition;
    std::array<std::string_view, 2> successors;
    if (!ParseConditionalBranch(line, condition, successors) ||
        successors[0] != enabled_predecessor && successors[1] != enabled_predecessor)
      continue;
    const Slice gate_slice = BackwardSlice(function, condition);
    if (!gate_slice.complete) return result;
    bool found_load = false;
    for (const std::string& value : gate_slice.values) {
      const Instruction* definition = Definition(function, value);
      if (definition &&
          (IsDxOpCallWithOpcode(*definition,
               "@dx.op.cbufferLoadLegacy.f32", 59) ||
           IsDxOpCallWithOpcode(*definition,
               "@dx.op.cbufferLoad.f32", 58))) {
        found_load = true;
        break;
      }
    }
    if (!found_load) continue;
    result.gate_proven = true;
    result.evidence = ResolveGatePredicateEvidence(function, condition, gate_slice);
    return result;
  }
  return result;
}

struct ThresholdAccess {
  std::string global;
  std::string index;
};

bool IsGlobalSymbol(std::string_view value) noexcept {
  if (value.size() < 2 || value.front() != '@') return false;
  for (std::size_t index = 1; index < value.size(); ++index)
    if (!IsSsaCharacter(value[index])) return false;
  return true;
}

bool IsNineElementFloatType(const std::vector<std::string_view>& tokens,
    std::size_t& cursor) {
  if (cursor + 5 > tokens.size() || tokens[cursor] != "[" ||
      tokens[cursor + 1] != "9" || tokens[cursor + 2] != "x" ||
      tokens[cursor + 3] != "float" || tokens[cursor + 4] != "]")
    return false;
  cursor += 5;
  return true;
}

bool ParseNineElementThresholdAccess(const Instruction& instruction,
    ThresholdAccess& result) {
  const auto tokens = IrTokens(instruction.rhs);
  std::size_t cursor = 0;
  if (cursor == tokens.size() || tokens[cursor++] != "getelementptr") return false;
  if (cursor < tokens.size() && tokens[cursor] == "inbounds") ++cursor;
  if (!IsNineElementFloatType(tokens, cursor) || cursor == tokens.size() ||
      tokens[cursor++] != "," || !IsNineElementFloatType(tokens, cursor) ||
      cursor == tokens.size() || tokens[cursor++] != "*" ||
      cursor == tokens.size() || !IsGlobalSymbol(tokens[cursor]))
    return false;
  result.global = std::string(tokens[cursor++]);
  if (cursor + 6 != tokens.size() || tokens[cursor++] != "," ||
      tokens[cursor++] != "i32" || tokens[cursor++] != "0" ||
      tokens[cursor++] != "," || tokens[cursor++] != "i32" ||
      !IsSsaValue(tokens[cursor]))
    return false;
  result.index = std::string(tokens[cursor++]);
  return cursor == tokens.size();
}

bool ReferencesNineElementThresholdGlobal(const Module& module,
    std::string_view symbol) {
  bool found = false;
  for (const GlobalLine& global : module.globals) {
    const auto tokens = IrTokens(global.code);
    if (tokens.size() < 3 || tokens[0] != symbol || tokens[1] != "=") continue;
    if (found) return false;
    found = true;
    for (std::size_t cursor = 2; cursor < tokens.size(); ++cursor) {
      if (tokens[cursor] != "constant" && tokens[cursor] != "global") continue;
      ++cursor;
      if (!IsNineElementFloatType(tokens, cursor)) return false;
      break;
    }
    if (std::none_of(tokens.begin() + 2, tokens.end(),
            [](std::string_view token) { return token == "constant" || token == "global"; }))
      return false;
  }
  return found;
}

std::size_t SliceCountDxOpCall(const Function& function, const Slice& slice,
    std::string_view callee, std::uint32_t opcode) {
  if (!slice.complete) return 0;
  std::size_t count = 0;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (definition && IsDxOpCallWithOpcode(*definition, callee, opcode)) ++count;
  }
  return count;
}

bool HasUniqueSignatureSemantic(const Module& module,
    std::uint32_t signature, std::string_view semantic);

bool IsScreenSpaceThreeByThreeIndex(const Function& function,
    const ThresholdAccess& threshold_access, const Module& module) {
  const Slice index_slice = BackwardSlice(function, threshold_access.index);
  if (!index_slice.complete) return false;
  std::array<LoadInputF32Call, 2> position_inputs;
  std::size_t position_input_count = 0;
  for (const std::string& value : index_slice.values) {
    const Instruction* definition = Definition(function, value);
    if (!definition ||
        !HasExactToken(definition->rhs, "@dx.op.loadInput.f32"))
      continue;
    if (position_input_count == position_inputs.size() ||
        !ParseLoadInputF32(
            definition->rhs, position_inputs[position_input_count]))
      return false;
    ++position_input_count;
  }
  if (position_input_count != position_inputs.size() ||
      position_inputs[0].signature != position_inputs[1].signature ||
      position_inputs[0].row != 0 || position_inputs[1].row != 0 ||
      position_inputs[0].column > 1 || position_inputs[1].column > 1 ||
      position_inputs[0].column == position_inputs[1].column ||
      !HasUniqueSignatureSemantic(module, position_inputs[0].signature,
          "SV_Position"))
    return false;

  return SliceCountInstructionOpcode(function, index_slice, "srem") >= 2 &&
      SliceCountExactToken(function, index_slice, "3") >= 3 &&
      SliceHasInstructionOpcode(function, index_slice, "mul") &&
      SliceHasInstructionOpcode(function, index_slice, "add") &&
      SliceCountInstructionOpcode(function, index_slice, "fptosi") +
          SliceCountInstructionOpcode(function, index_slice, "fptoui") >= 2;
}

bool IsFloatLoadFromPointer(
    const Instruction& instruction, std::string_view expected_pointer) {
  std::string_view rhs = Trim(instruction.rhs);
  std::size_t cursor = 0;
  if (!Consume(rhs, cursor, "load") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "float") || !ConsumeComma(rhs, cursor))
    return false;
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "float") || !Consume(rhs, cursor, "*"))
    return false;
  SkipWhitespace(rhs, cursor);
  // The exact pointer token is the sole load operand. An adjacent SSA
  // character would continue a different, longer name (e.g. %ptr2), and
  // that ambiguity must not authorize the load.
  if (!rhs.substr(cursor).starts_with(expected_pointer)) return false;
  cursor += expected_pointer.size();
  if (cursor < rhs.size() && IsSsaCharacter(rhs[cursor])) return false;

  // The standard alignment suffix, if present, must come before any DXC
  // metadata attachments (!tbaa, !noalias, and similar) and is otherwise
  // treated as absent rather than partially consumed.
  std::size_t align_cursor = cursor;
  if (ConsumeComma(rhs, align_cursor)) {
    std::size_t candidate = align_cursor;
    SkipWhitespace(rhs, candidate);
    std::uint32_t alignment = 0;
    if (Consume(rhs, candidate, "align") && candidate < rhs.size() &&
        std::isspace(static_cast<unsigned char>(rhs[candidate])) != 0 &&
        ParseUnsigned(rhs, candidate, alignment))
      cursor = candidate;
  }
  // Only well-formed metadata attachments may follow the pointer operand or
  // its optional alignment; any other trailing syntax (extra operands,
  // unmatched suffixes) is ambiguous evidence and must not authorize a
  // Production rewrite.
  return HasOnlyMetadataAttachments(Trim(rhs.substr(cursor)));
}

bool SliceHasDxOpCall(const Function& function, const Slice& slice,
    std::string_view callee, std::uint32_t opcode) {
  return SliceCountDxOpCall(function, slice, callee, opcode) != 0;
}

bool SliceHasExactToken(const Function& function, const Slice& slice,
    std::initializer_list<std::string_view> tokens) {
  for (const std::string_view token : tokens)
    if (SliceHasExactToken(function, slice, token)) return true;
  return false;
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
  return has_threshold_load &&
      SliceHasInstructionOpcode(function, coverage_slice, "fsub") &&
      SliceHasInstructionOpcode(function, coverage_slice, "fmul") &&
      SliceHasExactToken(function, coverage_slice,
          {"2.000000e+00", "2.000000", "2.0"}) &&
      SliceHasDxOpCall(function, coverage_slice, "@dx.op.binary.f32", 35) &&
      SliceHasDxOpCall(function, coverage_slice, "@dx.op.binary.f32", 36) &&
      SliceHasInstructionOpcode(function, coverage_slice, "fadd");
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

bool IsSignedIntegerLiteral(std::string_view value) noexcept {
  if (value.empty()) return false;
  std::size_t cursor = value.front() == '-' || value.front() == '+' ? 1 : 0;
  if (cursor == value.size()) return false;
  for (; cursor < value.size(); ++cursor)
    if (std::isdigit(static_cast<unsigned char>(value[cursor])) == 0)
      return false;
  return true;
}

bool ParseLoadInputF32(
    std::string_view line, LoadInputF32Call& result) noexcept {
  line = Trim(line);
  constexpr std::string_view prefix = "call float @dx.op.loadInput.f32(";
  if (!line.starts_with(prefix)) return false;

  std::size_t cursor = prefix.size();
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(line, cursor, "i32", opcode) || opcode != 4 ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i32", result.signature) ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i32", result.row) ||
      !ConsumeComma(line, cursor) ||
      !ParseTypedUnsigned(line, cursor, "i8", result.column) ||
      !ConsumeComma(line, cursor))
    return false;

  SkipWhitespace(line, cursor);
  if (!Consume(line, cursor, "i32") || cursor == line.size() ||
      std::isspace(static_cast<unsigned char>(line[cursor])) == 0)
    return false;
  SkipWhitespace(line, cursor);
  const std::size_t value_start = cursor;
  while (cursor < line.size() && line[cursor] != ')' && line[cursor] != ',' &&
      std::isspace(static_cast<unsigned char>(line[cursor])) == 0)
    ++cursor;
  const std::string_view value = line.substr(value_start, cursor - value_start);
  if (value != "undef" && value != "poison" && !IsSsaValue(value) &&
      !IsSignedIntegerLiteral(value))
    return false;
  SkipWhitespace(line, cursor);
  if (cursor == line.size() || line[cursor] != ')') return false;
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(line.substr(cursor)));
}

bool ParseStoreOutputF32(std::string_view line, StoreOutputCall& result) {
  line = Trim(line);
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

struct DiscardCall {
  std::string predicate;
};

bool ParseDiscardCall(std::string_view line, DiscardCall& result) {
  line = Trim(line);
  constexpr std::string_view prefix = "call void @dx.op.discard(";
  if (!line.starts_with(prefix)) return false;
  std::size_t cursor = prefix.size();
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsigned(line, cursor, "i32", opcode) || opcode != 82 ||
      !ConsumeComma(line, cursor))
    return false;
  SkipWhitespace(line, cursor);
  if (!Consume(line, cursor, "i1") || cursor == line.size() ||
      std::isspace(static_cast<unsigned char>(line[cursor])) == 0)
    return false;
  SkipWhitespace(line, cursor);
  const std::size_t predicate_start = cursor;
  while (cursor < line.size() && line[cursor] != ')' && line[cursor] != ',')
    ++cursor;
  const std::string_view predicate = Trim(
      line.substr(predicate_start, cursor - predicate_start));
  if (!IsSsaValue(predicate) || cursor == line.size() || line[cursor] != ')')
    return false;
  result.predicate = std::string(predicate);
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(line.substr(cursor)));
}

struct MetadataDefinition {
  std::uint32_t id = 0;
  std::vector<std::string_view> fields;
};

enum class MetadataParseResult { NotNumericDefinition, Valid, Malformed };

MetadataParseResult ParseMetadataDefinition(std::string_view line,
    MetadataDefinition& result) {
  result = {};
  line = Trim(line);
  if (line.size() < 2 || line.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(line[1])) == 0)
    return MetadataParseResult::NotNumericDefinition;
  std::size_t cursor = 1;
  if (!ParseUnsigned(line, cursor, result.id))
    return MetadataParseResult::Malformed;
  SkipWhitespace(line, cursor);
  if (cursor == line.size() || line[cursor++] != '=')
    return MetadataParseResult::Malformed;
  SkipWhitespace(line, cursor);
  constexpr std::string_view distinct = "distinct";
  if (line.substr(cursor).starts_with(distinct)) {
    cursor += distinct.size();
    if (cursor == line.size() ||
        std::isspace(static_cast<unsigned char>(line[cursor])) == 0)
      return MetadataParseResult::Malformed;
    SkipWhitespace(line, cursor);
  }
  if (cursor + 2 > line.size() || line[cursor] != '!' ||
      line[cursor + 1] != '{')
    return MetadataParseResult::Malformed;
  cursor += 2;
  std::size_t field_cursor = cursor;
  unsigned depth = 1;
  bool quoted = false;
  for (; cursor < line.size(); ++cursor) {
    const char value = line[cursor];
    if (quoted && value == '\\' && cursor + 1 < line.size()) {
      ++cursor;
      continue;
    }
    if (value == '"') {
      quoted = !quoted;
      continue;
    }
    if (quoted) continue;
    if (value == '{') {
      ++depth;
      continue;
    }
    if (value == '}') {
      if (--depth != 0) continue;
      const std::string_view field = Trim(line.substr(field_cursor,
          cursor - field_cursor));
      if (field.empty()) return MetadataParseResult::Malformed;
      result.fields.push_back(field);
      ++cursor;
      return Trim(line.substr(cursor)).empty() ? MetadataParseResult::Valid :
          MetadataParseResult::Malformed;
    }
    if (value == ',' && depth == 1) {
      const std::string_view field = Trim(line.substr(field_cursor,
          cursor - field_cursor));
      if (field.empty()) return MetadataParseResult::Malformed;
      result.fields.push_back(field);
      field_cursor = cursor + 1;
    }
  }
  return MetadataParseResult::Malformed;
}

bool ParseMetadataSignature(std::string_view field, std::uint32_t& signature) {
  std::size_t cursor = 0;
  return ParseTypedUnsigned(field, cursor, "i32", signature) &&
      Trim(field.substr(cursor)).empty();
}

bool IsValidMetadataString(std::string_view field) noexcept {
  field = Trim(field);
  if (field.size() < 3 || !field.starts_with("!\"")) return false;
  for (std::size_t cursor = 2; cursor < field.size(); ++cursor) {
    if (field[cursor] == '"') return cursor + 1 == field.size();
    if (field[cursor] != '\\') continue;
    if (cursor + 2 >= field.size() ||
        std::isxdigit(static_cast<unsigned char>(field[cursor + 1])) == 0 ||
        std::isxdigit(static_cast<unsigned char>(field[cursor + 2])) == 0)
      return false;
    cursor += 2;
  }
  return false;
}

bool IsValidTypedIntegerMetadataValue(std::string_view field) noexcept {
  field = Trim(field);
  if (field.size() < 4 || field.front() != 'i') return false;
  std::size_t cursor = 1;
  const std::size_t width_start = cursor;
  while (cursor < field.size() &&
      std::isdigit(static_cast<unsigned char>(field[cursor])) != 0)
    ++cursor;
  if (cursor == width_start || cursor == field.size() ||
      std::isspace(static_cast<unsigned char>(field[cursor])) == 0)
    return false;
  SkipWhitespace(field, cursor);
  const std::string_view value = field.substr(cursor);
  return value == "true" || value == "false" ||
      IsSignedIntegerLiteral(value);
}

bool ParseMetadataReference(
    std::string_view field, std::uint32_t& reference) noexcept {
  field = Trim(field);
  if (field.size() < 2 || field.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(field[1])) == 0)
    return false;
  std::size_t cursor = 1;
  return ParseUnsigned(field, cursor, reference) && cursor == field.size();
}

bool MetadataLineClaimsId(
    std::string_view line, std::uint32_t expected) noexcept {
  line = Trim(line);
  if (line.size() < 2 || line.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(line[1])) == 0)
    return false;
  std::size_t cursor = 1;
  std::uint32_t parsed = 0;
  return ParseUnsigned(line, cursor, parsed) && parsed == expected;
}

bool FindUniqueMetadataDefinition(const Module& module, std::uint32_t id,
    MetadataDefinition& result) {
  bool found = false;
  for (const GlobalLine& global : module.globals) {
    MetadataDefinition definition;
    const MetadataParseResult parsed =
        ParseMetadataDefinition(global.code, definition);
    if (parsed == MetadataParseResult::Malformed) {
      if (MetadataLineClaimsId(global.code, id)) return false;
      continue;
    }
    if (parsed != MetadataParseResult::Valid || definition.id != id) continue;
    if (found) return false;
    found = true;
    result = definition;
  }
  return found;
}

bool IsValidMetadataDefinition(const Module& module,
    const MetadataDefinition& definition,
    std::unordered_set<std::uint32_t>& resolving,
    std::unordered_set<std::uint32_t>& validated) {
  if (validated.contains(definition.id)) return true;
  if (!resolving.insert(definition.id).second) return false;
  for (const std::string_view raw_field : definition.fields) {
    const std::string_view field = Trim(raw_field);
    if (field == "null" || IsValidMetadataString(field) ||
        IsValidTypedIntegerMetadataValue(field))
      continue;
    std::uint32_t reference = 0;
    MetadataDefinition referenced;
    if (!ParseMetadataReference(field, reference) ||
        !FindUniqueMetadataDefinition(module, reference, referenced) ||
        !IsValidMetadataDefinition(module, referenced, resolving, validated)) {
      resolving.erase(definition.id);
      return false;
    }
  }
  resolving.erase(definition.id);
  validated.insert(definition.id);
  return true;
}

bool HasUniqueSignatureSemantic(const Module& module,
    std::uint32_t signature, std::string_view semantic) {
  const std::string expected = "!\"" + std::string(semantic) + "\"";
  std::unordered_set<std::uint32_t> definition_ids;
  std::unordered_set<std::uint32_t> resolving;
  std::unordered_set<std::uint32_t> validated;
  bool found = false;
  for (const GlobalLine& global : module.globals) {
    MetadataDefinition definition;
    const MetadataParseResult parsed =
        ParseMetadataDefinition(global.code, definition);
    if (parsed == MetadataParseResult::Malformed) {
      // An unrelated malformed metadata node is not evidence about this
      // semantic. A malformed node that claims the semantic is directly
      // relevant and therefore fails closed.
      if (global.code.find(expected) != std::string::npos) return false;
      continue;
    }
    if (parsed != MetadataParseResult::Valid) continue;
    if (!definition_ids.insert(definition.id).second) return false;
    if (definition.fields.size() < 3 ||
        Trim(definition.fields[1]) != expected)
      continue;
    if (!IsValidMetadataDefinition(
            module, definition, resolving, validated))
      return false;
    std::uint32_t parsed_signature = 0;
    if (!ParseMetadataSignature(definition.fields[0], parsed_signature))
      return false;
    if (parsed_signature != signature) continue;
    if (found) return false;
    found = true;
  }
  return found;
}

bool IsSvTargetSignature(const Module& module, std::uint32_t signature) {
  return HasUniqueSignatureSemantic(module, signature, "SV_Target");
}

bool IsStoreOutputF32Candidate(std::string_view code) noexcept {
  return Trim(code).starts_with(
      "call void @dx.op.storeOutput.f32(");
}

bool IsDiscardCandidate(std::string_view code) noexcept {
  return Trim(code).starts_with("call void @dx.op.discard(");
}

// A single well-typed float operand: an SSA value or a literal, with no
// further value hiding behind it. The exact delimiter (',' or ')') that ends
// it is left in place for the caller to consume.
bool ParseFloatOperand(std::string_view rhs, std::size_t& cursor) noexcept {
  SkipWhitespace(rhs, cursor);
  if (!Consume(rhs, cursor, "float") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t value_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  return !Trim(rhs.substr(value_start, cursor - value_start)).empty();
}

// call float @dx.op.binary.f32(i32 <opcode>, float <a>, float <b>)
// Exact callee, exact float return type, exact opcode field, exactly two
// typed float operands, and only valid metadata attachments may follow the
// closing parenthesis.
bool ParseDxOpBinaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept {
  rhs = Trim(rhs);
  constexpr std::string_view prefix = "call float @dx.op.binary.f32(";
  if (!rhs.starts_with(prefix)) return false;
  std::size_t cursor = prefix.size();
  if (!ParseTypedUnsigned(rhs, cursor, "i32", opcode) ||
      !ConsumeComma(rhs, cursor) || !ParseFloatOperand(rhs, cursor) ||
      cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  ++cursor;
  if (!ParseFloatOperand(rhs, cursor) || cursor == rhs.size() ||
      rhs[cursor] != ')')
    return false;
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(rhs.substr(cursor)));
}

// call float @dx.op.unary.f32(i32 <opcode>, float <value>)
// Same exactness requirements as the binary form, with exactly one operand.
bool ParseDxOpUnaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept {
  rhs = Trim(rhs);
  constexpr std::string_view prefix = "call float @dx.op.unary.f32(";
  if (!rhs.starts_with(prefix)) return false;
  std::size_t cursor = prefix.size();
  if (!ParseTypedUnsigned(rhs, cursor, "i32", opcode) ||
      !ConsumeComma(rhs, cursor) || !ParseFloatOperand(rhs, cursor) ||
      cursor == rhs.size() || rhs[cursor] != ')')
    return false;
  ++cursor;
  return HasOnlyMetadataAttachments(Trim(rhs.substr(cursor)));
}

// Exactly two pure, side-effect-free DXIL intrinsics are recognized as
// primitive propagation beyond plain LLVM arithmetic: FMin (dx.op.binary.f32
// opcode 36), which the verified coverage expression already requires
// elsewhere in the fade primitive, and Saturate (dx.op.unary.f32 opcode 7).
// No other dx.op.binary/unary opcode, and no other call of any kind, is
// trusted: an unrecognized or malformed call falls through to the caller's
// fail-closed catch-all instead of silently authorizing propagation.
bool IsRecognizedPureDxOpCall(std::string_view rhs) noexcept {
  std::uint32_t opcode = 0;
  if (ParseDxOpBinaryF32(rhs, opcode)) return opcode == 36;  // FMin(a, b)
  if (ParseDxOpUnaryF32(rhs, opcode)) return opcode == 7;    // Saturate(value)
  return false;
}

bool IsRecognizedPureSsaPropagation(const Instruction& instruction) {
  if (instruction.lhs.empty()) return false;
  const auto tokens = IrTokens(instruction.rhs);
  if (tokens.empty()) return false;
  constexpr std::array<std::string_view, 30> kPureOpcodes = {
      "fadd", "fsub", "fmul", "fdiv", "frem",
      "add", "sub", "mul", "udiv", "sdiv", "urem", "srem",
      "shl", "lshr", "ashr", "and", "or", "xor",
      "icmp", "fcmp", "phi", "select", "freeze",
      "trunc", "zext", "sext", "fptrunc", "fpext", "fptoui",
      "fptosi"};
  constexpr std::array<std::string_view, 9> kAdditionalPureOpcodes = {
      "uitofp", "sitofp", "ptrtoint", "inttoptr", "bitcast",
      "addrspacecast", "getelementptr", "extractelement", "insertelement"};
  constexpr std::array<std::string_view, 3> kAggregatePureOpcodes = {
      "shufflevector", "extractvalue", "insertvalue"};
  const auto contains = [opcode = tokens.front()](const auto& opcodes) {
    return std::find(opcodes.begin(), opcodes.end(), opcode) != opcodes.end();
  };
  if (contains(kPureOpcodes) || contains(kAdditionalPureOpcodes) ||
      contains(kAggregatePureOpcodes))
    return true;
  return IsRecognizedPureDxOpCall(instruction.rhs);
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
      if (IsDiscardCandidate(user.code)) {
        DiscardCall discard_call;
        if (!ParseDiscardCall(user.code, discard_call) ||
            discard_call.predicate != value) {
          // A malformed discard-looking use is not silently ignored: it is
          // ambiguous visibility evidence and therefore cannot authorize a
          // Production patch.
          other_output = true;
        } else {
          discard = true;
        }
        continue;
      }
      if (IsStoreOutputF32Candidate(user.code)) {
        if (!classified_outputs.insert(index).second) continue;
        StoreOutputCall output;
        if (!ParseStoreOutputF32(user.code, output) || output.value != value ||
            output.row != 0 || !IsSvTargetSignature(module, output.signature)) {
          other_output = true;
        } else if (output.column == 3) {
          // The complete alpha use must be unambiguous. A second matching
          // store (even to the same signature) could represent a distinct
          // output path, so it is not Production-authorized.
          if (target_alpha) other_output = true;
          target_alpha = true;
        } else if (output.column < 3) {
          if ((has_rgb_signature && output.signature != rgb_signature) ||
              (rgb_columns & (1u << output.column)) != 0) {
            other_output = true;
          } else {
            has_rgb_signature = true;
            rgb_signature = output.signature;
            rgb_columns |= 1u << output.column;
          }
        } else {
          other_output = true;
        }
        continue;
      }
      if (IsRecognizedPureSsaPropagation(user)) {
        pending.push_back(user.lhs);
        continue;
      }
      // No reachable use is implicitly harmless. Calls, stores, branches,
      // side-effecting instructions, unsupported value producers, and every
      // other terminal use make the Production authorization ambiguous.
      other_output = true;
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
  const Module module = ParseModule(llvm_ir);
  if (!module.complete) return diagnostic;
  for (const Function& function : module.functions) {
    if (!function.complete) continue;
    for (const Instruction& threshold : function.instructions) {
      ThresholdAccess threshold_access;
      if (threshold.lhs.empty() ||
          !ParseNineElementThresholdAccess(threshold, threshold_access) ||
          !IsScreenSpaceThreeByThreeIndex(function, threshold_access, module) ||
          !ReferencesNineElementThresholdGlobal(module, threshold_access.global))
        continue;
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
            SliceCountInstructionOpcode(function, enabled_slice,
                "getelementptr") != 1 ||
            !IsCoverageExpression(function, enabled->value, threshold.lhs))
          continue;
        const GateVerification gate =
            VerifyCbufferControlledGate(function, enabled->predecessor);
        if (!gate.gate_proven) continue;
        const ConsumerAnalysis consumers = ClassifyConsumers(function, module, phi.lhs);
        if (!consumers.complete) continue;
        FadePrimitiveInstance instance{
            consumers.consumer, function.identity, phi.start, phi.end, phi.lhs};
        instance.gate_predicate = gate.evidence;
        diagnostic.instances.push_back(std::move(instance));
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

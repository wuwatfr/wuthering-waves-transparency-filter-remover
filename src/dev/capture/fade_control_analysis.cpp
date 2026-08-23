// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// See fade_control_analysis.hpp for scope. This file intentionally
// duplicates a small amount of line/SSA parsing infrastructure that also
// exists (independently) in fade_primitive_detector.cpp rather than
// depending on that Production-shared file -- Dev tracing requirements must
// never contaminate the Production matcher.
//
// Currently recognized DXIL form (verified directly against
// tests/fixtures/fade_primitive_validation.ll, real dxc 1.9.2602.24
// output):
//
//   %h = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2,
//            i32 <rangeId literal>, i32 <index literal>, i1 false)
//   %r = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(
//            i32 59, %dx.types.Handle %h, i32 <row literal>)
//   %c = extractvalue %dx.types.CBufRet.f32 %r, <component literal 0-3>
//
// with <rangeId> resolved to space/register via the module's !dx.resources
// CBuffer metadata list. Any of: SM6.6 dynamic-resource binding
// (createHandleFromBinding/annotateHandle), the non-legacy
// dx.op.cbufferLoad.f32 form, a non-literal row/rangeId/index/component,
// more than one distinct cbufferLoadLegacy call reachable from the root,
// more than one distinct extractvalue component consuming the same call,
// more than one candidate gate branch, or missing/ambiguous/"distinct"
// !dx.resources metadata -- all fail closed to unresolved.

#include "dev/capture/fade_control_analysis.hpp"

#include <array>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr::dev {

namespace {

constexpr std::size_t kBackwardSliceLimit = 2048;

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

std::string_view StripComment(std::string_view line) noexcept {
  bool quoted = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    if (quoted && line[index] == '\\' && index + 1 < line.size()) {
      ++index;
      continue;
    }
    if (line[index] == '"') quoted = !quoted;
    if (!quoted && line[index] == ';') return line.substr(0, index);
  }
  return line;
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

bool ParseUnsignedLiteral(
    std::string_view text, std::uint64_t& value) noexcept {
  text = Trim(text);
  if (text.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
  }
  value = parsed;
  return true;
}

std::string ExtractFunctionIdentity(std::string_view header) {
  const std::size_t at = header.find('@');
  if (at == std::string_view::npos) return {};
  const std::size_t open = header.find('(', at);
  if (open == std::string_view::npos || open == at + 1) return {};
  for (std::size_t index = at + 1; index < open; ++index)
    if (!IsSsaCharacter(header[index])) return {};
  return std::string(header.substr(at, open - at));
}

struct Instruction {
  std::string code;  // comment-free, trimmed
  std::string lhs;
  std::string rhs;
};

struct FunctionGraph {
  bool complete = true;
  std::vector<Instruction> instructions;
  std::unordered_map<std::string, std::size_t> definitions;
};

const Instruction* Definition(
    const FunctionGraph& function, std::string_view value) {
  const auto found = function.definitions.find(std::string(value));
  return found == function.definitions.end() ? nullptr
                                              : &function.instructions[found->second];
}

// Parses exactly the function whose "define ... <identity>(...) {" line
// matches, from its opening brace to the matching top-level closing brace.
// DXIL disassembly never nests function definitions, so a simple
// "collect lines until a bare '}'" scan is sufficient; anything else (the
// function never found, or never closed) fails closed.
FunctionGraph ParseFunction(
    const std::string& llvm_ir, const std::string& function_identity) {
  FunctionGraph function;
  bool in_target = false;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end =
        newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view code =
        Trim(StripComment(std::string_view(llvm_ir.data() + start, end - start)));
    if (!in_target) {
      if (code.starts_with("define ") &&
          ExtractFunctionIdentity(code) == function_identity)
        in_target = true;
    } else if (code == "}") {
      return function;
    } else if (!code.empty()) {
      const std::size_t equals = code.find(" = ");
      Instruction instruction;
      instruction.code = std::string(code);
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
      function.instructions.push_back(std::move(instruction));
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  // Either the function was never found, or never closed.
  function.complete = false;
  return function;
}

struct Slice {
  std::unordered_set<std::string> values;
  bool complete = true;
};

Slice BackwardSlice(const FunctionGraph& function, std::string_view root) {
  Slice slice;
  std::vector<std::string> pending{std::string(root)};
  while (!pending.empty()) {
    std::string value = std::move(pending.back());
    pending.pop_back();
    if (!slice.values.insert(value).second) continue;
    if (const Instruction* definition = Definition(function, value)) {
      for (std::string dependency : SsaValues(definition->rhs))
        pending.push_back(std::move(dependency));
    }
    if (slice.values.size() >= kBackwardSliceLimit && !pending.empty()) {
      slice.complete = false;
      return slice;
    }
  }
  return slice;
}

struct PhiArm {
  std::string value;
  std::string predecessor;
};

bool ParsePhiArms(std::string_view rhs, PhiArm& first, PhiArm& second) {
  if (!rhs.starts_with("phi float ")) return false;
  const std::array<PhiArm*, 2> arms = {&first, &second};
  std::size_t cursor = 0;
  for (PhiArm* arm : arms) {
    const std::size_t begin = rhs.find('[', cursor);
    const std::size_t comma = rhs.find(',', begin);
    const std::size_t end = rhs.find(']', comma);
    if (begin == std::string_view::npos || comma == std::string_view::npos ||
        end == std::string_view::npos)
      return false;
    arm->value = std::string(Trim(rhs.substr(begin + 1, comma - begin - 1)));
    arm->predecessor =
        std::string(Trim(rhs.substr(comma + 1, end - comma - 1)));
    if (arm->value.empty() || arm->predecessor.empty() ||
        arm->predecessor.front() != '%')
      return false;
    cursor = end + 1;
  }
  return rhs.find('[', cursor) == std::string_view::npos;
}

bool IsIdentityOne(std::string_view value) noexcept {
  return value == "1.000000e+00" || value == "1.0" || value == "1.000000";
}

// Re-derives the phi's non-identity ("enabled") arm from the exact verified
// phi line, independent of fade_primitive_detector.cpp's own (private)
// phi-arm parsing.
bool ParseEnabledPhiArm(std::string_view phi_line,
    const std::string& merge_value, std::string& enabled_value,
    std::string& enabled_predecessor) {
  const std::size_t equals = phi_line.find(" = ");
  if (equals == std::string_view::npos) return false;
  if (Trim(phi_line.substr(0, equals)) != merge_value) return false;
  PhiArm first, second;
  if (!ParsePhiArms(phi_line.substr(equals + 3), first, second)) return false;
  const bool first_is_identity = IsIdentityOne(first.value);
  const bool second_is_identity = IsIdentityOne(second.value);
  const PhiArm* enabled =
      first_is_identity ? &second : (second_is_identity ? &first : nullptr);
  if (!enabled || enabled->value.empty() || enabled->value.front() != '%')
    return false;
  enabled_value = enabled->value;
  enabled_predecessor = enabled->predecessor;
  return true;
}

bool ParseConditionalBranch(std::string_view line, std::string_view& condition,
    std::string_view& successor_a, std::string_view& successor_b) {
  line = Trim(line);
  constexpr std::string_view prefix = "br i1 ";
  if (!line.starts_with(prefix)) return false;
  std::string_view rest = line.substr(prefix.size());
  const std::size_t comma1 = rest.find(',');
  if (comma1 == std::string_view::npos) return false;
  condition = Trim(rest.substr(0, comma1));
  if (condition.empty() || condition.front() != '%') return false;
  rest = Trim(rest.substr(comma1 + 1));
  constexpr std::string_view label_prefix = "label ";
  if (!rest.starts_with(label_prefix)) return false;
  rest = rest.substr(label_prefix.size());
  const std::size_t comma2 = rest.find(',');
  if (comma2 == std::string_view::npos) return false;
  successor_a = Trim(rest.substr(0, comma2));
  rest = Trim(rest.substr(comma2 + 1));
  if (!rest.starts_with(label_prefix)) return false;
  rest = rest.substr(label_prefix.size());
  const std::size_t trail = rest.find(',');
  successor_b =
      Trim(trail == std::string_view::npos ? rest : rest.substr(0, trail));
  return !successor_a.empty() && !successor_b.empty();
}

struct CbufferLoadCall {
  std::string handle_value;
  std::uint32_t row = 0;
};

bool ParseCbufferLoadLegacy(std::string_view rhs, CbufferLoadCall& result) {
  rhs = Trim(rhs);
  constexpr std::string_view prefix =
      "call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59,";
  if (!rhs.starts_with(prefix)) return false;
  std::string_view rest = Trim(rhs.substr(prefix.size()));
  constexpr std::string_view handle_type = "%dx.types.Handle ";
  if (!rest.starts_with(handle_type)) return false;
  rest = rest.substr(handle_type.size());
  const std::size_t comma = rest.find(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view handle = Trim(rest.substr(0, comma));
  if (handle.empty() || handle.front() != '%') return false;
  result.handle_value = std::string(handle);
  rest = Trim(rest.substr(comma + 1));
  constexpr std::string_view i32_type = "i32 ";
  if (!rest.starts_with(i32_type)) return false;
  rest = Trim(rest.substr(i32_type.size()));
  const std::size_t close = rest.find(')');
  if (close == std::string_view::npos) return false;
  std::uint64_t row = 0;
  if (!ParseUnsignedLiteral(rest.substr(0, close), row) || row > 0xFFFFFFFFull)
    return false;
  result.row = static_cast<std::uint32_t>(row);
  return true;
}

struct ExtractValueCall {
  std::string source_value;
  std::uint32_t component = 0;
};

bool ParseExtractValueCBufRet(std::string_view rhs, ExtractValueCall& result) {
  rhs = Trim(rhs);
  constexpr std::string_view prefix = "extractvalue %dx.types.CBufRet.f32 ";
  if (!rhs.starts_with(prefix)) return false;
  std::string_view rest = rhs.substr(prefix.size());
  const std::size_t comma = rest.find(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view source = Trim(rest.substr(0, comma));
  if (source.empty() || source.front() != '%') return false;
  result.source_value = std::string(source);
  std::uint64_t component = 0;
  if (!ParseUnsignedLiteral(Trim(rest.substr(comma + 1)), component) ||
      component > 3)
    return false;
  result.component = static_cast<std::uint32_t>(component);
  return true;
}

struct CreateHandleCall {
  std::uint8_t resource_class = 0;
  std::uint32_t range_id = 0;
};

bool ParseCreateHandle(std::string_view rhs, CreateHandleCall& result) {
  rhs = Trim(rhs);
  constexpr std::string_view prefix =
      "call %dx.types.Handle @dx.op.createHandle(i32 57,";
  if (!rhs.starts_with(prefix)) return false;
  std::string_view rest = Trim(rhs.substr(prefix.size()));
  constexpr std::string_view i8_type = "i8 ";
  if (!rest.starts_with(i8_type)) return false;
  rest = rest.substr(i8_type.size());
  std::size_t comma = rest.find(',');
  if (comma == std::string_view::npos) return false;
  std::uint64_t resource_class = 0;
  if (!ParseUnsignedLiteral(rest.substr(0, comma), resource_class) ||
      resource_class > 0xFF)
    return false;
  result.resource_class = static_cast<std::uint8_t>(resource_class);
  rest = Trim(rest.substr(comma + 1));
  constexpr std::string_view i32_type = "i32 ";
  if (!rest.starts_with(i32_type)) return false;
  rest = Trim(rest.substr(i32_type.size()));
  comma = rest.find(',');
  if (comma == std::string_view::npos) return false;
  std::uint64_t range_id = 0;
  if (!ParseUnsignedLiteral(rest.substr(0, comma), range_id) ||
      range_id > 0xFFFFFFFFull)
    return false;
  result.range_id = static_cast<std::uint32_t>(range_id);
  return true;
}

// Splits a "{a, b, c, ...}" metadata field list at depth-1 commas (ignoring
// commas inside nested "{}" and quoted strings).
bool SplitMetadataFields(
    std::string_view braced, std::vector<std::string_view>& fields) {
  braced = Trim(braced);
  if (braced.size() < 2 || braced.front() != '{' || braced.back() != '}')
    return false;
  const std::string_view inner = braced.substr(1, braced.size() - 2);
  std::size_t depth = 0;
  std::size_t field_start = 0;
  bool quoted = false;
  for (std::size_t i = 0; i <= inner.size(); ++i) {
    const bool at_end = i == inner.size();
    const char c = at_end ? ',' : inner[i];
    if (!at_end && quoted) {
      if (c == '\\' && i + 1 < inner.size()) {
        ++i;
        continue;
      }
      if (c == '"') quoted = false;
      continue;
    }
    if (!at_end && c == '"') {
      quoted = true;
      continue;
    }
    if (!at_end && c == '{') {
      ++depth;
      continue;
    }
    if (!at_end && c == '}') {
      if (depth != 0) --depth;
      continue;
    }
    if (c == ',' && depth == 0) {
      fields.push_back(Trim(inner.substr(field_start, i - field_start)));
      field_start = i + 1;
    }
  }
  return depth == 0 && !quoted;
}

bool ParseMetadataRef(std::string_view field, std::uint32_t& id) {
  field = Trim(field);
  if (field.size() < 2 || field.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(field[1])) == 0)
    return false;
  std::uint64_t parsed = 0;
  if (!ParseUnsignedLiteral(field.substr(1), parsed) ||
      parsed > 0xFFFFFFFFull)
    return false;
  id = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseTypedU32(std::string_view field, std::uint32_t& value) {
  field = Trim(field);
  constexpr std::string_view prefix = "i32 ";
  if (!field.starts_with(prefix)) return false;
  std::uint64_t parsed = 0;
  if (!ParseUnsignedLiteral(field.substr(prefix.size()), parsed) ||
      parsed > 0xFFFFFFFFull)
    return false;
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

// Deliberately does not recognize the "distinct" metadata prefix: DXIL
// resource-descriptor metadata tuples are never legitimately "distinct" in
// practice, and treating an unrecognized shape as "not found" (rather than
// guessing at its fields) is the fail-closed choice.
bool ParseMetadataDefinitionLine(std::string_view line, std::uint32_t& id,
    std::vector<std::string_view>& fields) {
  line = Trim(StripComment(line));
  if (line.size() < 2 || line.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(line[1])) == 0)
    return false;
  std::size_t cursor = 1;
  const std::size_t id_start = cursor;
  while (cursor < line.size() &&
      std::isdigit(static_cast<unsigned char>(line[cursor])) != 0)
    ++cursor;
  std::uint64_t parsed_id = 0;
  if (cursor == id_start ||
      !ParseUnsignedLiteral(line.substr(id_start, cursor - id_start), parsed_id))
    return false;
  std::string_view rest = Trim(line.substr(cursor));
  if (rest.empty() || rest.front() != '=') return false;
  rest = Trim(rest.substr(1));
  if (rest.empty() || rest.front() != '!') return false;
  rest = rest.substr(1);
  if (!SplitMetadataFields(rest, fields)) return false;
  id = static_cast<std::uint32_t>(parsed_id);
  return true;
}

// Requires exactly one definition of `id` in the whole module; a duplicate
// (or "distinct"-qualified, or otherwise malformed) definition is ambiguous
// and fails closed.
bool FindMetadataDefinition(const std::string& llvm_ir, std::uint32_t id,
    std::vector<std::string_view>& fields) {
  bool found = false;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end =
        newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line(llvm_ir.data() + start, end - start);
    std::uint32_t parsed_id = 0;
    std::vector<std::string_view> parsed_fields;
    if (ParseMetadataDefinitionLine(line, parsed_id, parsed_fields) &&
        parsed_id == id) {
      if (found) return false;
      found = true;
      fields = std::move(parsed_fields);
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  return found;
}

struct ResolvedCbvBinding {
  bool valid = false;
  std::uint32_t space = 0;
  std::uint32_t reg = 0;
};

// !dx.resources = !{ !SRVs, !UAVs, !CBVs, !Samplers }; !CBVs is a list of
// refs to individual 8-field CBuffer tuples: {ID, GV, name, space,
// register, range size, cbuffer size, tag}. Requires a unique
// !dx.resources line, a unique CBV list, and a unique entry whose ID field
// equals `range_id`; anything else fails closed (result.valid == false).
ResolvedCbvBinding ResolveCbvRangeId(
    const std::string& llvm_ir, std::uint32_t range_id) {
  ResolvedCbvBinding result;

  std::uint32_t resources_id = 0;
  bool found_resources_line = false;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end =
        newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line = Trim(StripComment(
        std::string_view(llvm_ir.data() + start, end - start)));
    constexpr std::string_view prefix = "!dx.resources = !{";
    if (line.starts_with(prefix) && !line.empty() && line.back() == '}') {
      if (found_resources_line) return result;
      const std::size_t brace = line.find('{');
      std::vector<std::string_view> fields;
      if (brace == std::string_view::npos ||
          !SplitMetadataFields(line.substr(brace), fields) ||
          fields.size() != 1 || !ParseMetadataRef(fields[0], resources_id))
        return result;
      found_resources_line = true;
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  if (!found_resources_line) return result;

  std::vector<std::string_view> resource_group_fields;
  if (!FindMetadataDefinition(llvm_ir, resources_id, resource_group_fields) ||
      resource_group_fields.size() != 4)
    return result;
  const std::string_view cbv_list_field = resource_group_fields[2];
  if (Trim(cbv_list_field) == "null") return result;
  std::uint32_t cbv_list_id = 0;
  if (!ParseMetadataRef(cbv_list_field, cbv_list_id)) return result;

  std::vector<std::string_view> cbv_refs;
  if (!FindMetadataDefinition(llvm_ir, cbv_list_id, cbv_refs)) return result;

  bool matched = false;
  for (const std::string_view ref : cbv_refs) {
    std::uint32_t entry_id = 0;
    if (!ParseMetadataRef(ref, entry_id)) return {};
    std::vector<std::string_view> entry_fields;
    if (!FindMetadataDefinition(llvm_ir, entry_id, entry_fields) ||
        entry_fields.size() < 5)
      return {};
    std::uint32_t entry_range_id = 0;
    if (!ParseTypedU32(entry_fields[0], entry_range_id)) return {};
    if (entry_range_id != range_id) continue;
    if (matched) return {};
    std::uint32_t space = 0, reg = 0;
    if (!ParseTypedU32(entry_fields[3], space) ||
        !ParseTypedU32(entry_fields[4], reg))
      return {};
    result.space = space;
    result.reg = reg;
    matched = true;
  }
  result.valid = matched;
  return result;
}

// Backward-slices from `root_value` and requires: exactly one distinct
// cbufferLoadLegacy call reachable, exactly one distinct extractvalue
// component of that call's result reachable, and a literally-indexed CBV
// createHandle for that call's handle operand, resolvable to a unique
// space/register via !dx.resources. Any ambiguity or unsupported form
// yields a default-constructed (unresolved) FadeControlSource.
FadeControlSource ResolveControlSource(const std::string& llvm_ir,
    const FunctionGraph& function, std::string_view root_value) {
  const FadeControlSource unresolved;
  const Slice slice = BackwardSlice(function, root_value);
  if (!slice.complete) return unresolved;

  std::string load_lhs;
  CbufferLoadCall load_call;
  bool found_load = false;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (!definition) continue;
    CbufferLoadCall candidate;
    if (!ParseCbufferLoadLegacy(definition->rhs, candidate)) continue;
    if (found_load) return unresolved;
    found_load = true;
    load_lhs = value;
    load_call = candidate;
  }
  if (!found_load) return unresolved;

  bool found_component = false;
  std::uint32_t component = 0;
  for (const std::string& value : slice.values) {
    const Instruction* definition = Definition(function, value);
    if (!definition) continue;
    ExtractValueCall candidate;
    if (!ParseExtractValueCBufRet(definition->rhs, candidate) ||
        candidate.source_value != load_lhs)
      continue;
    if (found_component) return unresolved;
    found_component = true;
    component = candidate.component;
  }
  if (!found_component) return unresolved;

  const Instruction* handle_definition =
      Definition(function, load_call.handle_value);
  if (!handle_definition) return unresolved;
  CreateHandleCall handle_call;
  if (!ParseCreateHandle(handle_definition->rhs, handle_call) ||
      handle_call.resource_class != 2)
    return unresolved;

  const ResolvedCbvBinding binding =
      ResolveCbvRangeId(llvm_ir, handle_call.range_id);
  if (!binding.valid) return unresolved;

  FadeControlSource resolved;
  resolved.resolved = true;
  resolved.cbuffer_space = binding.space;
  resolved.cbuffer_register = binding.reg;
  resolved.vector_index = load_call.row;
  resolved.component = component;
  return resolved;
}

// Finds the single conditional branch that gates entry into
// `enabled_predecessor` and resolves a control source from its condition.
// More than one candidate gate branch is ambiguous and fails closed.
FadeControlSource ResolveGatePredicateSource(const std::string& llvm_ir,
    const FunctionGraph& function, const std::string& enabled_predecessor) {
  bool found_branch = false;
  FadeControlSource source;
  for (const Instruction& instruction : function.instructions) {
    std::string_view condition, successor_a, successor_b;
    if (!ParseConditionalBranch(
            instruction.code, condition, successor_a, successor_b) ||
        (successor_a != enabled_predecessor &&
            successor_b != enabled_predecessor))
      continue;
    if (found_branch) return {};
    found_branch = true;
    source = ResolveControlSource(llvm_ir, function, condition);
  }
  return found_branch ? source : FadeControlSource{};
}

}  // namespace

std::vector<FadeControlInstanceSources> AnalyzeFadeControlSources(
    const std::string& llvm_ir,
    const wuwa_tfr::FadePrimitiveDiagnostic& fade_primitive) {
  std::vector<FadeControlInstanceSources> results;
  results.reserve(fade_primitive.instances.size());
  std::unordered_map<std::string, FunctionGraph> functions;
  for (const auto& instance : fade_primitive.instances) {
    FadeControlInstanceSources sources;
    auto found = functions.find(instance.function_identity);
    if (found == functions.end()) {
      found = functions
                  .emplace(instance.function_identity,
                      ParseFunction(llvm_ir, instance.function_identity))
                  .first;
    }
    const FunctionGraph& function = found->second;
    if (function.complete && instance.phi_start < instance.phi_end &&
        instance.phi_end <= llvm_ir.size()) {
      const std::string_view phi_line = Trim(StripComment(std::string_view(
          llvm_ir.data() + instance.phi_start,
          instance.phi_end - instance.phi_start)));
      std::string enabled_value;
      std::string enabled_predecessor;
      if (ParseEnabledPhiArm(
              phi_line, instance.merge_value, enabled_value,
              enabled_predecessor)) {
        sources.predicate =
            ResolveGatePredicateSource(llvm_ir, function, enabled_predecessor);
        sources.coverage =
            ResolveControlSource(llvm_ir, function, enabled_value);
      }
    }
    results.push_back(sources);
  }
  return results;
}

}  // namespace wuwa_tfr::dev

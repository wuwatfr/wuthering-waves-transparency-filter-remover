// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "pre_fade_fmin_analysis.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wuwa_tfr {
namespace {

// This analyzer independently re-derives and re-verifies, from raw text,
// exactly the structure it reports on -- it never reuses or trusts
// fade_primitive_detector.cpp's internal (anonymous-namespace, private)
// parsed graph from a prior pass.

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
  std::string_view code;
  std::string_view lhs;
  std::string_view rhs;
};

struct ParsedFunction {
  std::vector<FunctionLine> lines;
  std::unordered_map<std::string, std::size_t> definitions;
  bool complete = true;
};

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
      header_skipped = true;
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

struct CBufferLoadLegacy {
  std::string_view handle;
  std::uint32_t row = 0;
};

bool ParseCBufferLoadLegacy(std::string_view rhs, CBufferLoadLegacy& out) {
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
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')') ++cursor;
  const std::string_view handle = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',') return false;
  out.handle = handle;
  ++cursor;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  std::uint64_t row = 0;
  const std::string_view row_text = Trim(rhs.substr(start, cursor - start));
  for (char c : row_text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    row = row * 10 + static_cast<unsigned>(c - '0');
  }
  if (row_text.empty() || row > 0xFFFFFFFFull) return false;
  out.row = static_cast<std::uint32_t>(row);
  return true;
}

struct CBufferLoadByte {
  std::string_view handle;
  std::uint32_t byte_offset = 0;
};

bool ParseCBufferLoadByte(std::string_view rhs, CBufferLoadByte& out) {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.cbufferLoad.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "58") || !ConsumeToken(rhs, cursor, ",") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle"))
    return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')') ++cursor;
  const std::string_view handle = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',') return false;
  out.handle = handle;
  ++cursor;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  std::uint64_t offset = 0;
  const std::string_view offset_text = Trim(rhs.substr(start, cursor - start));
  for (char c : offset_text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    offset = offset * 10 + static_cast<unsigned>(c - '0');
  }
  if (offset_text.empty() || offset > 0xFFFFFFFFull) return false;
  out.byte_offset = static_cast<std::uint32_t>(offset);
  return true;
}

// Resolves an FMin operand *directly* to its constant-buffer origin:
// extractvalue-of-cbufferLoadLegacy.f32, or a bare cbufferLoad.f32 call. No
// bitcast/phi/select/freeze copy-chain is followed: an indirect path is not
// "direct" and must not qualify.
bool ResolveDirectOrigin(const ParsedFunction& function, std::string_view operand,
    PreFadeOperandSource& source) {
  if (!IsSsaValue(operand)) return false;
  const FunctionLine* definition = Definition(function, operand);
  if (!definition) return false;
  std::string_view aggregate;
  if (ParseExtractValueAggregate(definition->rhs, aggregate)) {
    const FunctionLine* aggregate_definition = Definition(function, aggregate);
    if (!aggregate_definition) return false;
    CBufferLoadLegacy legacy;
    if (!ParseCBufferLoadLegacy(aggregate_definition->rhs, legacy)) return false;
    source.resolved = true;
    source.handle_value = std::string(legacy.handle);
    source.legacy_form = true;
    source.row = legacy.row;
    // The extractvalue's own component index is filled in by the caller,
    // which has the operand's own (not the aggregate's) definition in hand.
    return true;
  }
  CBufferLoadByte byte_form;
  if (ParseCBufferLoadByte(definition->rhs, byte_form)) {
    source.resolved = true;
    source.handle_value = std::string(byte_form.handle);
    source.legacy_form = false;
    source.byte_offset = byte_form.byte_offset;
    return true;
  }
  return false;
}

bool ParseExtractValueComponent(std::string_view rhs, std::uint32_t& component) {
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view index_text = Trim(rhs.substr(comma + 1));
  std::uint64_t value = 0;
  for (char c : index_text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (index_text.empty() || value > 3) return false;
  component = static_cast<std::uint32_t>(value);
  return true;
}

// call float @dx.op.binary.f32(i32 36, float <A>, float <B>)
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

// ---------- best-effort, diagnostic-only CBV space/register resolution ----------

bool ParseCreateHandleRangeId(std::string_view rhs, std::uint32_t& range_id) {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "%dx.types.Handle") ||
      !ConsumeToken(rhs, cursor, "@dx.op.createHandle") || !ConsumeToken(rhs, cursor, "(") ||
      !ConsumeToken(rhs, cursor, "i32") || !ConsumeToken(rhs, cursor, "57") ||
      !ConsumeToken(rhs, cursor, ",") || !ConsumeToken(rhs, cursor, "i8"))
    return false;
  while (cursor < rhs.size() && rhs[cursor] != ',') ++cursor;
  if (cursor == rhs.size()) return false;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  while (cursor < rhs.size() && std::isspace(static_cast<unsigned char>(rhs[cursor])) != 0)
    ++cursor;
  const std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',') ++cursor;
  const std::string_view id_text = Trim(rhs.substr(start, cursor - start));
  std::uint64_t value = 0;
  for (char c : id_text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (id_text.empty() || value > 0xFFFFFFFFull) return false;
  range_id = static_cast<std::uint32_t>(value);
  return true;
}

bool SplitMetadataFields(std::string_view braced, std::vector<std::string_view>& fields) {
  braced = Trim(braced);
  if (braced.size() < 2 || braced.front() != '{' || braced.back() != '}') return false;
  const std::string_view inner = braced.substr(1, braced.size() - 2);
  std::size_t depth = 0, field_start = 0;
  bool quoted = false;
  for (std::size_t i = 0; i <= inner.size(); ++i) {
    const bool at_end = i == inner.size();
    const char c = at_end ? ',' : inner[i];
    if (!at_end && quoted) {
      if (c == '\\' && i + 1 < inner.size()) { ++i; continue; }
      if (c == '"') quoted = false;
      continue;
    }
    if (!at_end && c == '"') { quoted = true; continue; }
    if (!at_end && c == '{') { ++depth; continue; }
    if (!at_end && c == '}') { if (depth != 0) --depth; continue; }
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
  std::uint64_t value = 0;
  for (char c : field.substr(1)) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value > 0xFFFFFFFFull) return false;
  id = static_cast<std::uint32_t>(value);
  return true;
}

bool ParseTypedU32(std::string_view field, std::uint32_t& value) {
  field = Trim(field);
  constexpr std::string_view prefix = "i32 ";
  if (!field.starts_with(prefix)) return false;
  std::uint64_t parsed = 0;
  for (char c : field.substr(prefix.size())) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
  }
  if (parsed > 0xFFFFFFFFull) return false;
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool ParseMetadataDefinitionLine(std::string_view line, std::uint32_t& id,
    std::vector<std::string_view>& fields) {
  line = Trim(StripComment(line));
  if (line.size() < 2 || line.front() != '!' ||
      std::isdigit(static_cast<unsigned char>(line[1])) == 0)
    return false;
  std::size_t cursor = 1;
  const std::size_t id_start = cursor;
  while (cursor < line.size() && std::isdigit(static_cast<unsigned char>(line[cursor])) != 0)
    ++cursor;
  if (cursor == id_start) return false;
  std::uint64_t parsed_id = 0;
  for (char c : line.substr(id_start, cursor - id_start))
    parsed_id = parsed_id * 10 + static_cast<unsigned>(c - '0');
  std::string_view rest = Trim(line.substr(cursor));
  if (rest.empty() || rest.front() != '=') return false;
  rest = Trim(rest.substr(1));
  if (rest.empty() || rest.front() != '!') return false;
  rest = rest.substr(1);
  if (!SplitMetadataFields(rest, fields)) return false;
  id = static_cast<std::uint32_t>(parsed_id);
  return true;
}

bool FindMetadataDefinition(
    const std::string& llvm_ir, std::uint32_t id, std::vector<std::string_view>& fields) {
  bool found = false;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end = newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line(llvm_ir.data() + start, end - start);
    std::uint32_t parsed_id = 0;
    std::vector<std::string_view> parsed_fields;
    if (ParseMetadataDefinitionLine(line, parsed_id, parsed_fields) && parsed_id == id) {
      if (found) return false;
      found = true;
      fields = std::move(parsed_fields);
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  return found;
}

// !dx.resources = !{ !SRVs, !UAVs, !CBVs, !Samplers }; !CBVs is a list of
// refs to 8-field CBuffer tuples: {ID, GV, name, space, register, range
// size, cbuffer size, tag}. Best-effort, diagnostic-only: any ambiguity
// (duplicate/missing metadata, more than one entry matching range_id) simply
// leaves the caller's source.register_resolved false.
void ResolveCbvRangeIdBestEffort(
    const std::string& llvm_ir, std::uint32_t range_id, PreFadeOperandSource& source) {
  std::uint32_t resources_id = 0;
  bool found_resources_line = false;
  for (std::size_t start = 0; start <= llvm_ir.size();) {
    const std::size_t newline = llvm_ir.find('\n', start);
    const std::size_t end = newline == std::string::npos ? llvm_ir.size() : newline;
    const std::string_view line = Trim(StripComment(
        std::string_view(llvm_ir.data() + start, end - start)));
    constexpr std::string_view prefix = "!dx.resources = !{";
    if (line.starts_with(prefix) && !line.empty() && line.back() == '}') {
      if (found_resources_line) return;
      const std::size_t brace = line.find('{');
      std::vector<std::string_view> fields;
      if (brace == std::string_view::npos || !SplitMetadataFields(line.substr(brace), fields) ||
          fields.size() != 1 || !ParseMetadataRef(fields[0], resources_id))
        return;
      found_resources_line = true;
    }
    if (newline == std::string::npos) break;
    start = newline + 1;
  }
  if (!found_resources_line) return;

  std::vector<std::string_view> resource_group_fields;
  if (!FindMetadataDefinition(llvm_ir, resources_id, resource_group_fields) ||
      resource_group_fields.size() != 4)
    return;
  const std::string_view cbv_list_field = resource_group_fields[2];
  if (Trim(cbv_list_field) == "null") return;
  std::uint32_t cbv_list_id = 0;
  if (!ParseMetadataRef(cbv_list_field, cbv_list_id)) return;

  std::vector<std::string_view> cbv_refs;
  if (!FindMetadataDefinition(llvm_ir, cbv_list_id, cbv_refs)) return;

  bool matched = false;
  std::uint32_t space = 0, reg = 0;
  for (const std::string_view ref : cbv_refs) {
    std::uint32_t entry_id = 0;
    if (!ParseMetadataRef(ref, entry_id)) return;
    std::vector<std::string_view> entry_fields;
    if (!FindMetadataDefinition(llvm_ir, entry_id, entry_fields) || entry_fields.size() < 5)
      return;
    std::uint32_t entry_range_id = 0;
    if (!ParseTypedU32(entry_fields[0], entry_range_id)) return;
    if (entry_range_id != range_id) continue;
    if (matched) return;  // ambiguous
    if (!ParseTypedU32(entry_fields[3], space) || !ParseTypedU32(entry_fields[4], reg)) return;
    matched = true;
  }
  if (!matched) return;
  source.register_resolved = true;
  source.cbuffer_space = space;
  source.cbuffer_register = reg;
}

void ResolveRegisterBestEffort(
    const ParsedFunction& function, const std::string& llvm_ir, PreFadeOperandSource& source) {
  const FunctionLine* handle_definition = Definition(function, source.handle_value);
  if (!handle_definition) return;
  std::uint32_t range_id = 0;
  if (!ParseCreateHandleRangeId(handle_definition->rhs, range_id)) return;
  ResolveCbvRangeIdBestEffort(llvm_ir, range_id, source);
}

// ---------- phi re-derivation (independent of target_dither_bypass.cpp) ----------

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

bool ReDeriveEnabledArmValue(const std::string& text, const FadePrimitiveInstance& instance,
    std::string_view& enabled_value, std::string& error) {
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

struct FMinCandidate {
  std::string_view lhs;
  std::string_view operand_a;
  std::string_view operand_b;
  PreFadeOperandSource source_a;
  PreFadeOperandSource source_b;
};

std::vector<FMinCandidate> FindQualifyingFMinCandidates(
    const ParsedFunction& function, const Slice& slice) {
  std::vector<FMinCandidate> candidates;
  if (!slice.complete) return candidates;
  for (const FunctionLine& line : function.lines) {
    if (line.lhs.empty() || !slice.values.contains(std::string(line.lhs))) continue;
    std::string_view operand_a, operand_b;
    if (!ParseFMinOperands(line.rhs, operand_a, operand_b)) continue;
    PreFadeOperandSource source_a, source_b;
    if (!ResolveDirectOrigin(function, operand_a, source_a) ||
        !ResolveDirectOrigin(function, operand_b, source_b) ||
        source_a.handle_value != source_b.handle_value)
      continue;
    // Component for the legacy form: located precisely from the operand's
    // own extractvalue instruction (ResolveDirectOrigin only captured the
    // aggregate's row so far).
    if (source_a.legacy_form) {
      const FunctionLine* definition = Definition(function, operand_a);
      std::uint32_t component = 0;
      if (definition && ParseExtractValueComponent(definition->rhs, component))
        source_a.component = component;
    }
    if (source_b.legacy_form) {
      const FunctionLine* definition = Definition(function, operand_b);
      std::uint32_t component = 0;
      if (definition && ParseExtractValueComponent(definition->rhs, component))
        source_b.component = component;
    }
    FMinCandidate candidate;
    candidate.lhs = line.lhs;
    candidate.operand_a = operand_a;
    candidate.operand_b = operand_b;
    candidate.source_a = std::move(source_a);
    candidate.source_b = std::move(source_b);
    candidates.push_back(std::move(candidate));
  }
  return candidates;
}

}  // namespace

PreFadeFMinAnalysis AnalyzePreFadeFMinForInstance(
    const std::string& llvm_ir, const FadePrimitiveInstance& instance) {
  PreFadeFMinAnalysis result;

  std::string_view enabled_value;
  if (!ReDeriveEnabledArmValue(llvm_ir, instance, enabled_value, result.error)) return result;

  std::size_t block_start = 0, block_end = 0;
  if (!FindUniqueFunctionBlock(
          llvm_ir, instance.function_identity, block_start, block_end, result.error))
    return result;
  const ParsedFunction function = ParseFunctionBlock(llvm_ir, block_start, block_end);
  if (!function.complete) {
    result.error = "verified primitive function has duplicate SSA definitions";
    return result;
  }

  const Slice slice = BackwardSlice(function, enabled_value);
  result.backward_slice_complete = slice.complete;
  if (!slice.complete) {
    result.error = "verified primitive backward slice is incomplete";
    return result;
  }

  std::vector<FMinCandidate> candidates = FindQualifyingFMinCandidates(function, slice);
  result.qualifying_fmin_count = candidates.size();
  if (candidates.size() != 1) {
    result.error = candidates.empty()
        ? "no qualifying pre-Fade FMin found in the verified backward slice"
        : "multiple qualifying pre-Fade FMin candidates; ambiguous";
    return result;
  }

  FMinCandidate& candidate = candidates.front();
  result.fmin_result_identity = std::string(candidate.lhs);
  result.operand_one = std::move(candidate.source_a);
  result.operand_two = std::move(candidate.source_b);
  result.operand_one.source_start = AbsoluteOffset(llvm_ir, candidate.operand_a);
  result.operand_one.source_end = result.operand_one.source_start + candidate.operand_a.size();
  result.operand_one.source_text = std::string(candidate.operand_a);
  result.operand_two.source_start = AbsoluteOffset(llvm_ir, candidate.operand_b);
  result.operand_two.source_end = result.operand_two.source_start + candidate.operand_b.size();
  result.operand_two.source_text = std::string(candidate.operand_b);

  ResolveRegisterBestEffort(function, llvm_ir, result.operand_one);
  ResolveRegisterBestEffort(function, llvm_ir, result.operand_two);

  // Diagnostic-only, byte-precise adjacency classification: adjacent means
  // exactly one float (4 bytes) apart in the flattened constant-buffer
  // layout, whether that stays within one legacy row or crosses a 16-byte
  // row boundary; anything else -- including same-row but non-adjacent
  // components -- is NonAdjacent.
  if (result.operand_one.legacy_form && result.operand_two.legacy_form) {
    const std::int64_t byte_one =
        static_cast<std::int64_t>(result.operand_one.row) * 16 +
        static_cast<std::int64_t>(result.operand_one.component) * 4;
    const std::int64_t byte_two =
        static_cast<std::int64_t>(result.operand_two.row) * 16 +
        static_cast<std::int64_t>(result.operand_two.component) * 4;
    const std::int64_t diff = byte_two - byte_one;
    if (diff != 4 && diff != -4) {
      result.adjacency = PreFadeAdjacency::NonAdjacent;
    } else {
      result.adjacency = result.operand_one.row == result.operand_two.row
          ? PreFadeAdjacency::SameRow
          : PreFadeAdjacency::CrossRow;
    }
  } else if (!result.operand_one.legacy_form && !result.operand_two.legacy_form) {
    const std::int64_t diff = static_cast<std::int64_t>(result.operand_two.byte_offset) -
        static_cast<std::int64_t>(result.operand_one.byte_offset);
    result.adjacency = (diff == 4 || diff == -4)
        ? PreFadeAdjacency::CrossRow
        : PreFadeAdjacency::NonAdjacent;
  } else {
    result.adjacency = PreFadeAdjacency::Unknown;
  }

  result.success = true;
  return result;
}

}  // namespace wuwa_tfr

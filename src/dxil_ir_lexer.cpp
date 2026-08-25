// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxil_ir_lexer.hpp"

#include <cctype>
#include <limits>

namespace wuwa_tfr::dxil_ir {
namespace {

bool ParseUnsignedCursor(std::string_view text, std::size_t& cursor,
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

bool ParseTypedUnsignedCursor(std::string_view text, std::size_t& cursor,
    std::string_view type, std::uint32_t& value) noexcept {
  if (!ConsumeToken(text, cursor, type) || cursor == text.size() ||
      std::isspace(static_cast<unsigned char>(text[cursor])) == 0)
    return false;
  return ParseUnsignedCursor(text, cursor, value);
}

}  // namespace

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

std::string_view Trim(std::string_view text) noexcept {
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) return {};
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

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
  const std::size_t open = at == std::string_view::npos ? at :
      header.find('(', at);
  if (at == std::string_view::npos || open == std::string_view::npos ||
      open == at + 1) return {};
  for (std::size_t index = at + 1; index < open; ++index)
    if (!IsSsaCharacter(header[index])) return {};
  return std::string(header.substr(at, open - at));
}

void SkipWhitespace(std::string_view text, std::size_t& cursor) noexcept {
  while (cursor < text.size() &&
      std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
    ++cursor;
}

bool ConsumeToken(std::string_view text, std::size_t& cursor,
    std::string_view token) noexcept {
  SkipWhitespace(text, cursor);
  if (!text.substr(cursor).starts_with(token)) return false;
  cursor += token.size();
  return true;
}

bool ParseDecimalU32(std::string_view text, std::uint32_t& value) noexcept {
  if (text.empty()) return false;
  std::uint64_t parsed = 0;
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
    parsed = parsed * 10 + static_cast<unsigned>(c - '0');
    if (parsed > 0xFFFFFFFFull) return false;
  }
  value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool IsWellFormedIndexOperand(std::string_view text) noexcept {
  if (text.empty()) return false;
  std::uint32_t ignored = 0;
  return ParseDecimalU32(text, ignored) || IsSsaValue(text);
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

bool IsWellFormedCallSuffix(std::string_view rest) noexcept {
  rest = Trim(rest);
  if (rest.empty()) return true;
  if (rest.front() == '#') {
    std::size_t cursor = 1;
    while (cursor < rest.size() &&
        std::isdigit(static_cast<unsigned char>(rest[cursor])) != 0)
      ++cursor;
    if (cursor == 1) return false;
    rest = Trim(rest.substr(cursor));
    if (rest.empty()) return true;
  }
  return HasOnlyMetadataAttachments(rest);
}

bool ParseCbufferLoadLegacyCall(
    std::string_view rhs, CbufferLoadLegacyCall& result) noexcept {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") ||
      !ConsumeToken(rhs, cursor, "%dx.types.CBufRet.f32") ||
      !ConsumeToken(rhs, cursor, "@dx.op.cbufferLoadLegacy.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "59") || !ConsumeToken(rhs, cursor, ",") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle"))
    return false;
  SkipWhitespace(rhs, cursor);
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  const std::string_view handle = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  result.handle = handle;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  SkipWhitespace(rhs, cursor);
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  if (cursor == rhs.size()) return false;
  const std::string_view row_text = Trim(rhs.substr(start, cursor - start));
  if (!IsWellFormedIndexOperand(row_text)) return false;
  if (!IsWellFormedCallSuffix(rhs.substr(cursor + 1))) return false;
  result.row_resolved = ParseDecimalU32(row_text, result.row);
  return true;
}

bool ParseCbufferLoadByteCall(
    std::string_view rhs, CbufferLoadByteCall& result) noexcept {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.cbufferLoad.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "58") || !ConsumeToken(rhs, cursor, ",") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle"))
    return false;
  SkipWhitespace(rhs, cursor);
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  const std::string_view handle = Trim(rhs.substr(start, cursor - start));
  if (!IsSsaValue(handle) || cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  result.handle = handle;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  SkipWhitespace(rhs, cursor);
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')' && rhs[cursor] != ',')
    ++cursor;
  if (cursor == rhs.size() || rhs[cursor] != ',') return false;
  const std::string_view offset_text = Trim(rhs.substr(start, cursor - start));
  if (!IsWellFormedIndexOperand(offset_text)) return false;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "i32")) return false;
  SkipWhitespace(rhs, cursor);
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  if (cursor == rhs.size()) return false;
  std::uint32_t alignment = 0;
  if (!ParseDecimalU32(Trim(rhs.substr(start, cursor - start)), alignment))
    return false;
  if (!IsWellFormedCallSuffix(rhs.substr(cursor + 1))) return false;
  result.byte_offset_resolved = ParseDecimalU32(offset_text, result.byte_offset);
  return true;
}

bool ParseExtractValueAggregate(
    std::string_view rhs, std::string_view& aggregate) noexcept {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "extractvalue ") ||
      !ConsumeToken(rhs, cursor, "%dx.types.CBufRet.f32 "))
    return false;
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos || comma < cursor) return false;
  const std::string_view candidate = Trim(rhs.substr(cursor, comma - cursor));
  if (!IsSsaValue(candidate)) return false;
  aggregate = candidate;
  return true;
}

bool ParseExtractValueComponent(
    std::string_view rhs, std::uint32_t& component) noexcept {
  const std::size_t comma = rhs.rfind(',');
  if (comma == std::string_view::npos) return false;
  std::uint32_t value = 0;
  if (!ParseDecimalU32(Trim(rhs.substr(comma + 1)), value) || value > 3)
    return false;
  component = value;
  return true;
}

bool ParseCreateHandleCall(
    std::string_view rhs, CreateHandleCall& result) noexcept {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") ||
      !ConsumeToken(rhs, cursor, "%dx.types.Handle") ||
      !ConsumeToken(rhs, cursor, "@dx.op.createHandle") ||
      !ConsumeToken(rhs, cursor, "("))
    return false;
  std::uint32_t opcode = 0;
  if (!ParseTypedUnsignedCursor(rhs, cursor, "i32", opcode) || opcode != 57 ||
      !ConsumeToken(rhs, cursor, ","))
    return false;
  std::uint32_t resource_class = 0;
  if (!ParseTypedUnsignedCursor(rhs, cursor, "i8", resource_class) ||
      !ConsumeToken(rhs, cursor, ","))
    return false;
  result.resource_class_resolved = true;
  result.resource_class = resource_class;
  std::uint32_t range_id = 0;
  if (ParseTypedUnsignedCursor(rhs, cursor, "i32", range_id)) {
    result.range_id_resolved = true;
    result.range_id = range_id;
  }
  return true;
}

bool ParseFloatOperand(std::string_view rhs, std::size_t& cursor) noexcept {
  SkipWhitespace(rhs, cursor);
  if (!ConsumeToken(rhs, cursor, "float") || cursor == rhs.size() ||
      std::isspace(static_cast<unsigned char>(rhs[cursor])) == 0)
    return false;
  SkipWhitespace(rhs, cursor);
  const std::size_t value_start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',' && rhs[cursor] != ')')
    ++cursor;
  return !Trim(rhs.substr(value_start, cursor - value_start)).empty();
}

bool ParseFMinOperands(std::string_view rhs, std::string_view& operand_a,
    std::string_view& operand_b) noexcept {
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.binary.f32") ||
      !ConsumeToken(rhs, cursor, "(") || !ConsumeToken(rhs, cursor, "i32") ||
      !ConsumeToken(rhs, cursor, "36") || !ConsumeToken(rhs, cursor, ","))
    return false;
  if (!ConsumeToken(rhs, cursor, "float")) return false;
  SkipWhitespace(rhs, cursor);
  std::size_t start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ',') ++cursor;
  operand_a = Trim(rhs.substr(start, cursor - start));
  if (cursor == rhs.size() || rhs[cursor] != ',') return false;
  ++cursor;
  if (!ConsumeToken(rhs, cursor, "float")) return false;
  SkipWhitespace(rhs, cursor);
  start = cursor;
  while (cursor < rhs.size() && rhs[cursor] != ')') ++cursor;
  operand_b = Trim(rhs.substr(start, cursor - start));
  if (cursor == rhs.size() || rhs[cursor] != ')') return false;
  if (!IsWellFormedCallSuffix(rhs.substr(cursor + 1))) return false;
  return !operand_a.empty() && !operand_b.empty();
}

bool ParseDxOpBinaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept {
  rhs = Trim(rhs);
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.binary.f32") ||
      !ConsumeToken(rhs, cursor, "("))
    return false;
  if (!ParseTypedUnsignedCursor(rhs, cursor, "i32", opcode) ||
      !ConsumeToken(rhs, cursor, ",") || !ParseFloatOperand(rhs, cursor) ||
      cursor == rhs.size() || rhs[cursor] != ',')
    return false;
  ++cursor;
  if (!ParseFloatOperand(rhs, cursor) || cursor == rhs.size() ||
      rhs[cursor] != ')')
    return false;
  ++cursor;
  return IsWellFormedCallSuffix(rhs.substr(cursor));
}

bool ParseDxOpUnaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept {
  rhs = Trim(rhs);
  std::size_t cursor = 0;
  if (!ConsumeToken(rhs, cursor, "call") || !ConsumeToken(rhs, cursor, "float") ||
      !ConsumeToken(rhs, cursor, "@dx.op.unary.f32") ||
      !ConsumeToken(rhs, cursor, "("))
    return false;
  if (!ParseTypedUnsignedCursor(rhs, cursor, "i32", opcode) ||
      !ConsumeToken(rhs, cursor, ",") || !ParseFloatOperand(rhs, cursor) ||
      cursor == rhs.size() || rhs[cursor] != ')')
    return false;
  ++cursor;
  return IsWellFormedCallSuffix(rhs.substr(cursor));
}

}  // namespace wuwa_tfr::dxil_ir

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wuwa_tfr::dxil_ir {

bool IsSsaCharacter(char value) noexcept;
bool IsSsaValue(std::string_view value) noexcept;

std::string_view Trim(std::string_view text) noexcept;

struct CodeAndComment {
  std::string_view code;
  std::string_view comment;
};
CodeAndComment SplitCodeAndComment(std::string_view text) noexcept;

std::vector<std::string_view> SsaValues(std::string_view text);

std::string FunctionIdentity(std::string_view header);

void SkipWhitespace(std::string_view text, std::size_t& cursor) noexcept;
bool ConsumeToken(std::string_view text, std::size_t& cursor,
    std::string_view token) noexcept;

bool ParseDecimalU32(std::string_view text, std::uint32_t& value) noexcept;
bool IsWellFormedIndexOperand(std::string_view text) noexcept;

bool HasOnlyMetadataAttachments(std::string_view trailing) noexcept;
bool IsWellFormedCallSuffix(std::string_view rest) noexcept;

struct CbufferLoadLegacyCall {
  std::string_view handle;
  bool row_resolved = false;
  std::uint32_t row = 0;
};
bool ParseCbufferLoadLegacyCall(
    std::string_view rhs, CbufferLoadLegacyCall& result) noexcept;

struct CbufferLoadByteCall {
  std::string_view handle;
  bool byte_offset_resolved = false;
  std::uint32_t byte_offset = 0;
};
bool ParseCbufferLoadByteCall(
    std::string_view rhs, CbufferLoadByteCall& result) noexcept;

bool ParseExtractValueAggregate(
    std::string_view rhs, std::string_view& aggregate) noexcept;
bool ParseExtractValueComponent(
    std::string_view rhs, std::uint32_t& component) noexcept;

struct CreateHandleCall {
  bool resource_class_resolved = false;
  std::uint32_t resource_class = 0;
  bool range_id_resolved = false;
  std::uint32_t range_id = 0;
};
bool ParseCreateHandleCall(
    std::string_view rhs, CreateHandleCall& result) noexcept;

bool ParseFMinOperands(std::string_view rhs, std::string_view& operand_a,
    std::string_view& operand_b) noexcept;
bool ParseDxOpBinaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept;
bool ParseDxOpUnaryF32(std::string_view rhs, std::uint32_t& opcode) noexcept;

}  // namespace wuwa_tfr::dxil_ir

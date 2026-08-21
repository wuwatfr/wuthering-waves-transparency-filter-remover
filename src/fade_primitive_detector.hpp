// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wuwa_tfr {

enum class FadePrimitiveConsumer : std::uint8_t {
  Unknown,
  Discard,
  SvTargetAlpha,
  DiscardAndSvTargetAlpha,
  OtherVisibilityOrOutput,
};

struct FadePrimitiveInstance {
  FadePrimitiveConsumer consumer = FadePrimitiveConsumer::Unknown;
  // An SSA name is local to a defined LLVM function.  The source range is the
  // exact original phi line verified by the matcher and is the patch target.
  std::string function_identity;
  std::size_t phi_start = 0;
  std::size_t phi_end = 0;
  std::string merge_value;
};

struct FadePrimitiveDiagnostic {
  std::vector<FadePrimitiveInstance> instances;
};

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir);

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept;

} // namespace wuwa_tfr

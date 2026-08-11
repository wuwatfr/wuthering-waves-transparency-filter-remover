// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstdint>
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
  std::string merge_value;
};

struct FadePrimitiveDiagnostic {
  std::vector<FadePrimitiveInstance> instances;
};

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir);

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept;

} // namespace wuwa_tfr

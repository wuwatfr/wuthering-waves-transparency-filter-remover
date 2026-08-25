// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wuwa_tfr {

enum class FadePrimitiveConsumer : std::uint8_t {
  Unknown,
  Discard,
  SvTargetAlpha,
  SvTargetRgb,
  DiscardAndSvTargetAlpha,
  OtherVisibilityOrOutput,
};

struct FadePrimitiveGatePredicateEvidence {
  bool condition_identified = false;
  std::string condition_value;

  bool resolved = false;
  std::string handle_value;
  bool legacy_form = false;

  bool range_id_resolved = false;
  std::uint32_t range_id = 0;

  bool register_resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;

  bool row_resolved = false;
  std::uint32_t row = 0;
  bool component_resolved = false;
  std::uint32_t component = 0;
  bool byte_offset_resolved = false;
  std::uint32_t byte_offset = 0;
};

struct FadePrimitiveInstance {
  FadePrimitiveConsumer consumer = FadePrimitiveConsumer::Unknown;
  std::string function_identity;
  std::size_t phi_start = 0;
  std::size_t phi_end = 0;
  std::string merge_value;
  FadePrimitiveGatePredicateEvidence gate_predicate;
};

struct FadePrimitiveDiagnostic {
  std::vector<FadePrimitiveInstance> instances;
};

FadePrimitiveDiagnostic AnalyzeFadePrimitiveV1(const std::string& llvm_ir);

const char* FadePrimitiveConsumerName(FadePrimitiveConsumer consumer) noexcept;

}

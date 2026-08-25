// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "fade_primitive_detector.hpp"

namespace wuwa_tfr {

inline constexpr std::string_view kPreFadeRewriteLiteral = "1.000000e+00";

enum class PreFadeAdjacency { Unknown, SameRow, CrossRow, NonAdjacent };

constexpr bool PreFadeAdjacencyIsProvenAdjacent(
    PreFadeAdjacency adjacency) noexcept {
  return adjacency == PreFadeAdjacency::SameRow ||
      adjacency == PreFadeAdjacency::CrossRow;
}

inline constexpr std::int64_t kPreFadeOperandOrientationDeltaBytes = 4;

enum class PreFadeFMinStatus : std::uint8_t {
  Matched,
  NoQualifyingCandidate,
  AmbiguousCandidates,
  OperandsNotAdjacent,
  OperandsReversed,
  InvalidInstanceIdentity,
  FunctionNotUniquelyLocated,
  FunctionNotParsable,
  IncompleteBackwardSlice,
};

const char* PreFadeFMinStatusName(PreFadeFMinStatus status) noexcept;

struct PreFadeOperandSource {
  bool resolved = false;
  std::string handle_value;
  bool legacy_form = false;
  bool row_resolved = false;
  std::uint32_t row = 0;
  bool component_resolved = false;
  std::uint32_t component = 0;
  bool byte_offset_resolved = false;
  std::uint32_t byte_offset = 0;
  bool register_resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
  std::size_t source_start = 0;
  std::size_t source_end = 0;
  std::string source_text;
};

struct PreFadeFMinAnalysis {
  PreFadeFMinStatus status = PreFadeFMinStatus::InvalidInstanceIdentity;
  bool success = false;
  bool instance_identity_verified = false;
  bool function_located = false;
  bool function_parsed = false;
  bool backward_slice_complete = false;
  std::size_t qualifying_fmin_count = 0;
  std::string fmin_result_identity;
  PreFadeOperandSource operand_one;
  PreFadeOperandSource operand_two;
  PreFadeAdjacency adjacency = PreFadeAdjacency::Unknown;
  std::string error;
};

bool PreFadeFMinAnalysisIsStructurallyComplete(
    const PreFadeFMinAnalysis& analysis) noexcept;

bool PreFadeFMinProvesNoQualifyingCandidate(
    const PreFadeFMinAnalysis& analysis) noexcept;

struct CbvRegisterBinding {
  bool resolved = false;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
};

CbvRegisterBinding ResolveCbvRangeId(
    const std::string& llvm_ir, std::uint32_t range_id);

void ResolvePreFadeCbvRegisters(const std::string& llvm_ir,
    const FadePrimitiveInstance& instance, PreFadeFMinAnalysis& analysis);

void ResolveGatePredicateCbvRegister(
    const std::string& llvm_ir, FadePrimitiveGatePredicateEvidence& evidence);

bool VerifyPreFadeFMinOperandOneRewritten(const std::string& patched_llvm_ir,
    const FadePrimitiveInstance& patched_instance,
    const PreFadeFMinAnalysis& matched, std::string& error);

PreFadeFMinAnalysis AnalyzePreFadeFMinForInstance(
    const std::string& llvm_ir, const FadePrimitiveInstance& instance);

}

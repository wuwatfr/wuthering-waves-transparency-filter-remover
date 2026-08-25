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

// Whether the two resolved operands sit 4 bytes apart in one constant buffer.
// A Production match requires this: it proves the pair is two neighbouring
// scalars rather than two unrelated values that happen to share a handle.
// Both SameRow and CrossRow qualify -- which side of the boundary the pair
// straddles carries no meaning, and neither does the sign of the difference.
constexpr bool PreFadeAdjacencyIsProvenAdjacent(
    PreFadeAdjacency adjacency) noexcept {
  return adjacency == PreFadeAdjacency::SameRow ||
      adjacency == PreFadeAdjacency::CrossRow;
}

// The one operand orientation observed across every validated instance:
// operand 2 sits exactly this many bytes after operand 1. Authorization
// requires it; the adjacency classification above stays sign-agnostic and
// remains purely diagnostic. This says nothing about which operand is the
// camera side -- it only refuses shapes that were never validated.
inline constexpr std::int64_t kPreFadeOperandOrientationDeltaBytes = 4;

enum class PreFadeFMinStatus : std::uint8_t {
  Matched,
  NoQualifyingCandidate,
  AmbiguousCandidates,
  // A structurally qualifying FMin was found, but its two operands were not
  // proven to be adjacent scalars of one constant buffer -- either resolved
  // and far apart, or with coordinates that could not be resolved at all.
  // Distinct from NoQualifyingCandidate: a candidate did exist.
  OperandsNotAdjacent,
  // The two operands are adjacent, but ordered the other way round: operand 2
  // sits 4 bytes *before* operand 1. Every validated instance has operand 2
  // exactly 4 bytes after operand 1, so the reversed pair is an unvalidated
  // shape and fails closed rather than being silently reinterpreted.
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

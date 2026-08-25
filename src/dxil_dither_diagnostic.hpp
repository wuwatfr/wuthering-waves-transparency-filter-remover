// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstdint>
#include <string>

namespace wuwa_tfr {

enum class SpatialDitherClassification : std::uint8_t {
  NoDiscard,
  NonSpatialDiscard,
  StrictSpatialDither,
  AmbiguousStrictSpatialDither,
};

struct SpatialDitherDiagnostic {
  std::uint32_t discard_calls = 0;
  std::uint32_t strict_spatial_dither_discards = 0;
  SpatialDitherClassification classification =
      SpatialDitherClassification::NoDiscard;
};

SpatialDitherDiagnostic AnalyzeSpatialDitherDiagnostic(
    const std::string& llvm_ir);

const char* SpatialDitherClassificationName(
    SpatialDitherClassification classification) noexcept;

} // namespace wuwa_tfr

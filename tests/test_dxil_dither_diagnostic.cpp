// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxil_dither_diagnostic.hpp"

#include "test_check.hpp"
#include <iostream>
#include <string>

namespace {

std::string StrictDitherDiscard() {
  return R"(
%1 = call float @dx.op.loadInput.f32() ; LoadInput
%2 = call float @dx.op.loadInput.f32() ; LoadInput
%3 = call float @dx.op.unary.f32() ; Round_ni(%1)
%4 = call float @dx.op.unary.f32() ; Round_ni(%2)
%5 = call float @dx.op.dot2.f32() ; Dot2(%3, %4)
%6 = call float @dx.op.unary.f32() ; Cos(%5)
%7 = fmul fast float %6, 7.0
%8 = call float @dx.op.unary.f32() ; Frc(%7)
%9 = call float @dx.op.loadInput.f32() ; LoadInput
%10 = fcmp fast ogt float %9, %8
%11 = fcmp fast olt float %9, %8
%12 = select i1 %10, i1 %10, i1 %11
%13 = uitofp i1 %12 to float
%14 = fadd fast float %13, -0.5
%15 = fcmp fast olt float %14, 0.0
call void @dx.op.discard(i32 82, i1 %15)
)";
}

} // namespace

int main() {
  {
    const auto result =
        wuwa_tfr::AnalyzeSpatialDitherDiagnostic(StrictDitherDiscard());
    CHECK(result.discard_calls == 1);
    CHECK(result.strict_spatial_dither_discards == 1);
    CHECK(result.classification ==
        wuwa_tfr::SpatialDitherClassification::StrictSpatialDither);
  }
  {
    std::string near = StrictDitherDiscard();
    const auto position = near.find("Round_ni(%2)");
    near.replace(position, std::string("Round_ni(%2)").size(), "Floor(%2)");
    const auto result = wuwa_tfr::AnalyzeSpatialDitherDiagnostic(near);
    CHECK(result.discard_calls == 1);
    CHECK(result.strict_spatial_dither_discards == 0);
    CHECK(result.classification ==
        wuwa_tfr::SpatialDitherClassification::NonSpatialDiscard);
  }
  {
    std::string ambiguous = StrictDitherDiscard();
    ambiguous += StrictDitherDiscard();
    const auto result = wuwa_tfr::AnalyzeSpatialDitherDiagnostic(ambiguous);
    CHECK(result.discard_calls == 2);
    CHECK(result.strict_spatial_dither_discards == 2);
    CHECK(result.classification ==
        wuwa_tfr::SpatialDitherClassification::AmbiguousStrictSpatialDither);
  }
  {
    const auto result = wuwa_tfr::AnalyzeSpatialDitherDiagnostic("");
    CHECK(result.classification ==
        wuwa_tfr::SpatialDitherClassification::NoDiscard);
  }
  std::cout << "dxil dither diagnostic tests passed\n";
}

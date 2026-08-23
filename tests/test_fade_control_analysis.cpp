// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_analysis.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include "fade_primitive_detector.hpp"
#include "test_check.hpp"

namespace {

// Verbatim copy of tests/fixtures/fade_primitive_validation.ll: real dxc
// 1.9.2602.24 output for the fade_primitive_validation.hlsl fixture (see
// that file's own header comment for exact provenance). Ground truth,
// rather than a hand-built approximation, for what the analyzer is meant
// to recognize: a legacy createHandle(class=CBV) feeding two
// cbufferLoadLegacy.f32 calls on the same handle/row (predicate reads
// component 0, coverage reads component 1 of %FadeConstants = {float,
// float}), resolved via !dx.resources to space 0 / register 0.
const std::string kFixture = R"FADEFIX(target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

%dx.types.Handle = type { i8* }
%dx.types.CBufRet.f32 = type { float, float, float, float }
%FadeConstants = type { float, float }

@thresholds = internal unnamed_addr constant [9 x float] [float 0.000000e+00, float 5.000000e-01, float 1.250000e-01, float 6.250000e-01, float 2.500000e-01, float 7.500000e-01, float 8.750000e-01, float 3.750000e-01, float 1.000000e+00], align 4
@dx.nothing.a = internal constant [1 x i32] zeroinitializer

define void @main() {
  %1 = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 0, i32 0, i1 false)
  %2 = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
  %3 = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
  %4 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %5 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %1, i32 0)
  %6 = extractvalue %dx.types.CBufRet.f32 %5, 0
  %7 = fcmp fast ogt float %6, 0.000000e+00
  %8 = icmp ne i1 %7, false
  %9 = icmp ne i1 %8, false
  br i1 %9, label %10, label %30, !dx.controlflow.hints !18

; <label>:10
  %11 = fptosi float %2 to i32
  %12 = srem i32 %11, 3
  %13 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %14 = fptosi float %3 to i32
  %15 = srem i32 %14, 3
  %16 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %17 = mul nsw i32 %12, 3
  %18 = add nsw i32 %17, %15
  %19 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %18
  %20 = load float, float* %19, align 4, !tbaa !50, !noalias !54
  %21 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %22 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %1, i32 0)
  %23 = extractvalue %dx.types.CBufRet.f32 %22, 1
  %24 = fmul fast float %23, 2.000000e+00
  %25 = fsub fast float %24, %20
  %26 = call float @dx.op.binary.f32(i32 35, float %25, float 0.000000e+00)  ; FMax(a,b)
  %27 = call float @dx.op.binary.f32(i32 36, float %26, float 1.000000e+00)  ; FMin(a,b)
  %28 = fadd fast float %27, 0x3FD54FDF40000000
  %29 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  br label %30

; <label>:30
  %31 = phi float [ %28, %10 ], [ 1.000000e+00, %0 ]
  %32 = call float @dx.op.unary.f32(i32 7, float %31)  ; Saturate(value)
  %33 = call float @dx.op.binary.f32(i32 36, float %32, float 1.000000e+00)  ; FMin(a,b)
  %34 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  %35 = fsub fast float %33, 5.000000e-01
  %36 = fcmp fast olt float %35, 0.000000e+00
  call void @dx.op.discard(i32 82, i1 %36)
  %37 = load i32, i32* getelementptr inbounds ([1 x i32], [1 x i32]* @dx.nothing.a, i32 0, i32 0)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %33)
  ret void
}

declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) #0
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) #1
declare float @dx.op.binary.f32(i32, float, float) #0
declare float @dx.op.unary.f32(i32, float) #0
declare void @dx.op.discard(i32, i1) #1
declare %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32, %dx.types.Handle, i32) #2
declare %dx.types.Handle @dx.op.createHandle(i32, i8, i32, i32, i1) #2

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }
attributes #2 = { nounwind readonly }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.resources = !{!4}
!dx.viewIdState = !{!7}
!dx.entryPoints = !{!8}
!0 = !{!"dxcoob 1.9.2602.24 (d355aa836)"}
!1 = !{i32 1, i32 0}
!2 = !{i32 1, i32 9}
!3 = !{!"ps", i32 6, i32 0}
!4 = !{null, null, !5, null}
!5 = !{!6}
!6 = !{i32 0, %FadeConstants* undef, !"", i32 0, i32 0, i32 1, i32 8, null}
!7 = !{[6 x i32] [i32 4, i32 4, i32 8, i32 8, i32 0, i32 0]}
!8 = !{void ()* @main, !"main", !9, !4, !17}
!9 = !{!10, !14, null}
!10 = !{!11}
!11 = !{i32 0, !"SV_Position", i8 9, i8 3, !12, i8 4, i32 1, i8 4, i32 0, i8 0, !13}
!12 = !{i32 0}
!13 = !{i32 3, i32 3}
!14 = !{!15}
!15 = !{i32 0, !"SV_Target", i8 9, i8 16, !12, i8 0, i32 1, i8 4, i32 0, i8 0, !16}
!16 = !{i32 3, i32 15}
!17 = !{i32 0, i64 1}
!18 = distinct !{!18, !"dx.controlflow.hints", i32 1}
!50 = !{!51, !51, i64 0}
!51 = !{!"float", !52, i64 0}
!52 = !{!"omnipotent char", !53, i64 0}
!53 = !{!"Simple C++ TBAA"}
!54 = !{!55}
!55 = distinct !{!55, !56}
!56 = distinct !{!56, !"wuwatfr.noalias"}
)FADEFIX";

std::string ReplaceOnce(std::string text, std::string_view needle,
    std::string_view replacement) {
  const std::size_t pos = text.find(needle);
  CHECK(pos != std::string::npos);
  text.replace(pos, needle.size(), replacement);
  return text;
}

std::vector<wuwa_tfr::dev::FadeControlInstanceSources> AnalyzeSingleInstance(
    const std::string& ir) {
  const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
  CHECK(diagnostic.instances.size() == 1);
  return wuwa_tfr::dev::AnalyzeFadeControlSources(ir, diagnostic);
}

}  // namespace

int main() {
  // 1. Direct cbuffer root extraction from representative (real) IR: both
  // the predicate and the coverage value resolve to the same cbuffer
  // binding, same 16-byte vector slot, different components.
  {
    const auto results = AnalyzeSingleInstance(kFixture);
    CHECK(results.size() == 1);
    const auto& predicate = results[0].predicate;
    const auto& coverage = results[0].coverage;
    CHECK(predicate.resolved);
    CHECK(predicate.cbuffer_space == 0);
    CHECK(predicate.cbuffer_register == 0);
    CHECK(predicate.vector_index == 0);
    CHECK(predicate.component == 0);
    CHECK(coverage.resolved);
    CHECK(coverage.cbuffer_space == 0);
    CHECK(coverage.cbuffer_register == 0);
    CHECK(coverage.vector_index == 0);
    CHECK(coverage.component == 1);
  }

  // 2. Multiple/ambiguous roots fail closed: a second, distinct
  // cbufferLoadLegacy call also feeds the predicate's gate condition.
  {
    const std::string ambiguous = ReplaceOnce(kFixture,
        "%7 = fcmp fast ogt float %6, 0.000000e+00",
        "%99 = call %dx.types.CBufRet.f32 "
        "@dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %1, i32 0)\n"
        "  %100 = extractvalue %dx.types.CBufRet.f32 %99, 2\n"
        "  %7 = fcmp fast ogt float %6, %100");
    const auto results = AnalyzeSingleInstance(ambiguous);
    CHECK(results.size() == 1);
    CHECK(!results[0].predicate.resolved);
    // The coverage arm is untouched by this mutation and must still
    // resolve -- ambiguity in one role must not contaminate the other.
    CHECK(results[0].coverage.resolved);
  }

  // 3. Unsupported DXIL form fails closed: a non-literal (dynamically
  // indexed) cbuffer range id cannot be a structurally proven source.
  {
    const std::string dynamic_range_id = ReplaceOnce(kFixture,
        "i32 57, i8 2, i32 0, i32 0, i1 false",
        "i32 57, i8 2, i32 %2, i32 0, i1 false");
    const auto results = AnalyzeSingleInstance(dynamic_range_id);
    CHECK(results.size() == 1);
    CHECK(!results[0].predicate.resolved);
    CHECK(!results[0].coverage.resolved);
  }

  // No instances in, no sources out -- and it must not crash on an empty
  // diagnostic.
  {
    const wuwa_tfr::FadePrimitiveDiagnostic empty;
    CHECK(wuwa_tfr::dev::AnalyzeFadeControlSources(kFixture, empty).empty());
  }

  return 0;
}

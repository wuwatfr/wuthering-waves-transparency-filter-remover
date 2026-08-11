// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"

#include "test_check.hpp"
#include <string>

namespace {

std::string PrimitiveIr(std::string_view consumer) {
  return std::string(R"(
; SV_Position              0   xyzw
@thresholds = internal constant [9 x float] zeroinitializer
%posx = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%posy = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%gate_load = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 7)
%gate_value = extractvalue %dx.types.CBufRet.f32 %gate_load, 1
%gate = fcmp fast ogt float %gate_value, 0.000000e+00
br i1 %gate, label %enabled, label %merge
; <label>:enabled
%ix = fptosi float %posx to i32
%iy = fptosi float %posy to i32
%mx = srem i32 %ix, 3
%my = srem i32 %iy, 3
%row = mul nsw i32 %mx, 3
%index = add nsw i32 %row, %my
%ptr = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index
%threshold = load float, float* %ptr, align 4
%twice = fmul fast float %coverage, 2.000000e+00
%sub = fsub fast float %twice, %threshold
%lo = call float @dx.op.binary.f32(i32 35, float %sub, float 0.000000e+00)  ; FMax(a,b)
%hi = call float @dx.op.binary.f32(i32 36, float %lo, float 1.000000e+00)  ; FMin(a,b)
%computed = fadd fast float %hi, 0x3FD50F9F00000000
br label %merge
; <label>:merge
%dither = phi float [ %computed, %enabled ], [ 1.000000e+00, %entry ]
)" ) + std::string(consumer);
}

} // namespace

int main() {
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(PrimitiveIr(R"(
%score = fsub fast float %dither, 0.500000e+00
%kill = fcmp fast olt float %score, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)"));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::Discard);
  }
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(PrimitiveIr(R"(
; SV_Target
%alpha = fmul fast float %dither, %opacity
call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)
)"));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
  }
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(PrimitiveIr(R"(
; SV_Target
%alpha = fmul fast float %dither, %opacity
call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)
%score = fsub fast float %dither, 0.500000e+00
%kill = fcmp fast olt float %score, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)"));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::DiscardAndSvTargetAlpha);
  }
  {
    auto positive = PrimitiveIr(R"(
%kill = fcmp fast olt float %dither, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
    const std::string declaration =
        "@thresholds = internal constant [9 x float] zeroinitializer";
    const std::size_t table = positive.find(declaration);
    positive.replace(table, declaration.size(),
        "@thresholds = dso_local unnamed_addr constant [9 x float] zeroinitializer");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(positive).instances.size() == 1);
  }
  {
    auto near_miss = PrimitiveIr(R"(
%kill = fcmp fast olt float %dither, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
    const std::size_t table = near_miss.find("[9 x float]");
    near_miss.replace(table, std::string("[9 x float]").size(), "[8 x float]");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(near_miss).instances.empty());
  }
  {
    auto near_miss = PrimitiveIr(R"(
%kill = fcmp fast olt float %dither, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
    const std::size_t table = near_miss.find("[9 x float]");
    near_miss.replace(table, std::string("[9 x float]").size(), "[8 x float]");
    near_miss.insert(0,
        "@unrelated = internal constant [9 x float] zeroinitializer\n");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(near_miss).instances.empty());
  }
  {
    auto near_miss = PrimitiveIr(R"(
%kill = fcmp fast olt float %dither, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
    const std::size_t identity = near_miss.find("1.000000e+00, %entry");
    near_miss.replace(identity, std::string("1.000000e+00").size(), "0.000000e+00");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(near_miss).instances.empty());
  }
  {
    auto near_miss = PrimitiveIr(R"(
%kill = fcmp fast olt float %dither, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
    const std::size_t cbuffer = near_miss.find("@dx.op.cbufferLoadLegacy");
    near_miss.replace(cbuffer, std::string("@dx.op.cbufferLoadLegacy").size(),
        "@not_a_cbuffer_load");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(near_miss).instances.empty());
  }
  return 0;
}

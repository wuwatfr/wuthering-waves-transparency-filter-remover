// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_prefade_fmin_hypothesis.hpp"

#include "test_check.hpp"
#include <string>
#include <string_view>

namespace {

// A minimal, valid, single-instance Fade Primitive v1 fixture (mirrors
// tests/test_target_dither_bypass.cpp's fixture shape) whose coverage
// expression's `%coverage0` input is supplied by the caller, so each test
// can graft a different pre-Fade combination shape ahead of it while every
// other structural requirement (gate, threshold table, phi, consumer) stays
// fixed and already verified-correct.
std::string Fixture(std::string_view coverage_prefix) {
  return std::string(R"(
!899 = !{i32 0, !"SV_Position", i8 9}
@thresholds = internal constant [9 x float] zeroinitializer
define void @target() {
entry:
%x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%cb0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 7)
%g0 = extractvalue %dx.types.CBufRet.f32 %cb0, 1
%c0 = fcmp fast ogt float %g0, 0.000000e+00
br i1 %c0, label %on0, label %merge0
; <label>:on0
)") + std::string(coverage_prefix) + std::string(R"(
%xi0 = fptosi float %x to i32
%yi0 = fptosi float %y to i32
%mx0 = srem i32 %xi0, 3
%my0 = srem i32 %yi0, 3
%row0 = mul nsw i32 %mx0, 3
%index0 = add nsw i32 %row0, %my0
%ptr0 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index0
%threshold0 = load float, float* %ptr0, align 4, !tbaa !50, !noalias !54
%twice0 = fmul fast float %coverage0, 2.000000e+00
%sub0 = fsub fast float %twice0, %threshold0
%lo0 = call float @dx.op.binary.f32(i32 35, float %sub0, float 0.000000e+00)  ; FMax(a,b)
%hi0 = call float @dx.op.binary.f32(i32 36, float %lo0, float 1.000000e+00)  ; FMin(a,b)
%computed0 = fadd fast float %hi0, 0x3FD50F9F00000000
br label %merge0
; <label>:merge0
%d0 = phi float [ %computed0, %on0 ], [ 1.000000e+00, %entry0 ]
%d0_sat = call float @dx.op.unary.f32(i32 7, float %d0)  ; Saturate(value)
%alpha = fmul fast float %d0_sat, %opacity
call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)
%kill = fcmp fast olt float %d0_sat, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
}
!900 = !{i32 0, !"SV_Target", i8 9}
)");
}

std::string LongChainOperandB(int length) {
  std::string chain =
      "%chain0 = fadd fast float 0.000000e+00, 1.000000e+00\n";
  for (int i = 1; i <= length; ++i)
    chain += "%chain" + std::to_string(i) + " = fadd fast float %chain" +
        std::to_string(i - 1) + ", 1.000000e+00\n";
  return chain;
}

}  // namespace

int main() {
  wuwa_tfr::dev::ResetPreFadeFMinHypothesisDiagnosticsForTest();

  // (1) Same-row adjacent operands: qualifies, and only operand 1 changes.
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 2
%opB = extractvalue %dx.types.CBufRet.f32 %cb_a, 3
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)
)";
    const std::string original_ir = Fixture(prefix);
    const auto result = wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(original_ir);
    CHECK(result.success);
    CHECK(result.structural_verification_succeeded);
    CHECK(result.ir_patch_succeeded);
    CHECK(result.verified_instance_count == 1);
    CHECK(result.patched_instance_count == 1);

    // Downstream-unchanged-except-operand-1: the patched IR must equal the
    // original with exactly "%opA" replaced by "1.000000e+00" at the FMin
    // call site, and nowhere else.
    std::string expected = original_ir;
    const std::string needle =
        "call float @dx.op.binary.f32(i32 36, float %opA, float %opB)";
    const std::size_t at = expected.find(needle);
    CHECK(at != std::string::npos);
    expected.replace(at, needle.size(),
        "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB)");
    CHECK(result.llvm_ir == expected);
    CHECK(result.llvm_ir.find("float %opB") != std::string::npos);
    CHECK(result.llvm_ir.find(
        "%d0 = phi float [ %computed0, %on0 ], [ 1.000000e+00, %entry0 ]") !=
        std::string::npos);
  }

  // (2) Cross-row adjacent operands: same CBV, different rows -- still
  // qualifies (this patch's "qualifying" test is same-CBV only, not
  // adjacency).
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 3
%cb_b = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 41)
%opB = extractvalue %dx.types.CBufRet.f32 %cb_b, 0
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)
)";
    const auto result =
        wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(Fixture(prefix));
    CHECK(result.success);
    CHECK(result.patched_instance_count == 1);
    CHECK(result.llvm_ir.find(
        "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB)") !=
        std::string::npos);
  }

  // (3) Non-adjacent census variant: same CBV, far apart rows/components --
  // still qualifies under this patch's broader "same CBV" criterion.
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 0
%cb_b = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 55)
%opB = extractvalue %dx.types.CBufRet.f32 %cb_b, 2
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)
)";
    const auto result =
        wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(Fixture(prefix));
    CHECK(result.success);
    CHECK(result.patched_instance_count == 1);
    CHECK(result.llvm_ir.find(
        "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB)") !=
        std::string::npos);
  }

  // (4) Ambiguity: two independent same-CBV qualifying FMin candidates in
  // the same backward slice must fail closed, with zero patching.
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 2
%opB = extractvalue %dx.types.CBufRet.f32 %cb_a, 3
%fmin1 = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)
%cb_c = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 90)
%opC = extractvalue %dx.types.CBufRet.f32 %cb_c, 0
%opD = extractvalue %dx.types.CBufRet.f32 %cb_c, 1
%fmin2 = call float @dx.op.binary.f32(i32 36, float %opC, float %opD)  ; FMin(a,b)
%coverage0 = fadd fast float %fmin1, %fmin2
)";
    const std::string ir = Fixture(prefix);
    const auto result = wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(ir);
    CHECK(!result.success);
    CHECK(!result.ir_patch_succeeded);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("ambiguous") != std::string::npos);
  }

  // (5a) Unresolvable operand: one FMin input is a spatial loadInput value,
  // not a constant-buffer scalar -- zero qualifying candidates, fail closed.
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 2
%spatial = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 0, i32 undef)
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA, float %spatial)  ; FMin(a,b)
)";
    const auto result =
        wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(Fixture(prefix));
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("no qualifying pre-Fade FMin") != std::string::npos);
  }

  // (5b) Incomplete source: the backward slice from the enabled path exceeds
  // the bound before it can even reach the qualifying FMin -- fail closed on
  // incompleteness, distinct from "absent"/"ambiguous".
  {
    std::string prefix =
        "%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 2\n"
        "%opB = extractvalue %dx.types.CBufRet.f32 %cb_a, 3\n"
        "%fmin_ok = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)\n" +
        LongChainOperandB(1100) +
        "%coverage0 = fadd fast float %fmin_ok, %chain1100\n";
    const auto result =
        wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(Fixture(prefix));
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("incomplete") != std::string::npos);
  }

  // (6) Downstream-unchanged-except-operand-1, explicitly at the multi-line
  // level: the gate branch, threshold table access, phi, saturate, alpha
  // output and discard predicate are all byte-identical before and after.
  {
    const std::string prefix = R"(
%cb_a = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA = extractvalue %dx.types.CBufRet.f32 %cb_a, 2
%opB = extractvalue %dx.types.CBufRet.f32 %cb_a, 3
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA, float %opB)  ; FMin(a,b)
)";
    const std::string original_ir = Fixture(prefix);
    const auto result =
        wuwa_tfr::dev::PatchPreFadeFMinOperandOneHypothesis(original_ir);
    CHECK(result.success);
    for (std::string_view unchanged_line : {
             std::string_view("%c0 = fcmp fast ogt float %g0, 0.000000e+00"),
             std::string_view("br i1 %c0, label %on0, label %merge0"),
             std::string_view("%ptr0 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index0"),
             std::string_view("%d0 = phi float [ %computed0, %on0 ], [ 1.000000e+00, %entry0 ]"),
             std::string_view("%d0_sat = call float @dx.op.unary.f32(i32 7, float %d0)  ; Saturate(value)"),
             std::string_view("call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)"),
             std::string_view("call void @dx.op.discard(i32 82, i1 %kill)")}) {
      CHECK(original_ir.find(unchanged_line) != std::string::npos);
      CHECK(result.llvm_ir.find(unchanged_line) != std::string::npos);
    }
    // The only textual difference anywhere in the module is operand 1 of the
    // qualifying FMin call.
    CHECK(result.llvm_ir.find("float %opA") == std::string::npos);
    CHECK(original_ir.find("float 1.000000e+00, float %opB") == std::string::npos);
    CHECK(result.llvm_ir.find("float 1.000000e+00, float %opB") != std::string::npos);
  }

  // Cumulative diagnostics: 7 patch calls above (same-row, cross-row,
  // non-adjacent, ambiguous, unresolvable, incomplete, downstream-unchanged
  // -- the last reuses the same-row shape as its own independent call). The
  // ambiguous and both incomplete/unresolvable cases still count as
  // "evaluated" -- they reached structural verification -- but not
  // "qualifying" or "patched".
  const auto diagnostics = wuwa_tfr::dev::PreFadeFMinHypothesisDiagnosticsSnapshot();
  CHECK(diagnostics.shaders_evaluated_total == 7);
  CHECK(diagnostics.verified_instances_total == 7);
  CHECK(diagnostics.qualifying_instances_total == 4);
  CHECK(diagnostics.patched_instances_total == 4);
  CHECK(diagnostics.shaders_failed_total == 3);
  CHECK(!wuwa_tfr::dev::LastPreFadeFMinHypothesisFailureReason().empty());

  return 0;
}

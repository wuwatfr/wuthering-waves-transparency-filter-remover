// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"

#include "test_check.hpp"
#include <string>
#include <string_view>

int main() {
  const auto make_ir = [](std::string_view phi) {
    return std::string(R"(
@DITHER_THRESHOLDS_GLOBAL = external constant [9 x float]
%threshold = getelementptr inbounds [9 x float], [9 x float]* @DITHER_THRESHOLDS_GLOBAL, i32 0, i32 %index
)") + std::string(phi) + R"(
%kill = fcmp fast olt float %score, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)";
  };

  const auto expect_patched = [&](std::string_view phi,
                                  std::string_view expected_phi) {
    const auto patched = wuwa_tfr::PatchSelectedTargetDitherToIdentity(
        make_ir(phi));
    CHECK(patched.success);
    CHECK(patched.structural_verification_succeeded);
    CHECK(patched.ir_patch_succeeded);
    CHECK(patched.llvm_ir.find(expected_phi) != std::string::npos);
    CHECK(patched.llvm_ir.find(" = fadd fast float 1.000000e+00, 0.000000e+00") ==
        std::string::npos);
    CHECK(patched.llvm_ir.find("call void @dx.op.discard(i32 82, i1 %kill)") !=
        std::string::npos);
  };

  // Unindented, two-space indented, and mixed leading whitespace all parse
  // identically; replacement changes only the verified incoming value token.
  expect_patched(
      "%dither_factor = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]",
      "%dither_factor = phi float [ 1.000000e+00, %enabled ], [ 1.000000e+00, %disabled ]");
  expect_patched(
      "  %dither_factor = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]",
      "  %dither_factor = phi float [ 1.000000e+00, %enabled ], [ 1.000000e+00, %disabled ]");
  expect_patched(
      "\t \t%dither_factor = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]",
      "\t \t%dither_factor = phi float [ 1.000000e+00, %enabled ], [ 1.000000e+00, %disabled ]");
  expect_patched(
      "%dither_factor = phi float [ 1.000000e+00, %disabled ], [ %computed, %enabled ]",
      "%dither_factor = phi float [ 1.000000e+00, %disabled ], [ 1.000000e+00, %enabled ]");

  // The exact real target phi shape is accepted after semantic trimming.
  const auto real_shape = wuwa_tfr::PatchSelectedTargetDitherToIdentity(make_ir(
      "  %1302 = phi float [ %1300, %1285 ], [ 1.000000e+00, %1279 ]"));
  CHECK(real_shape.success);
  CHECK(real_shape.structural_verification_succeeded);
  CHECK(real_shape.ir_patch_succeeded);
  CHECK(real_shape.llvm_ir.find(
      "  %1302 = phi float [ 1.000000e+00, %1285 ], [ 1.000000e+00, %1279 ]") !=
      std::string::npos);

  const auto adjacent_phis = wuwa_tfr::PatchSelectedTargetDitherToIdentity(
      make_ir(
          "%before = phi float [ %before_a, %before_left ], [ %before_b, %before_right ]\n"
          "%target = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]\n"
          "%after = phi float [ %after_a, %after_left ], [ %after_b, %after_right ]\n"
          "%unrelated_use = fmul fast float %computed, %opacity"));
  CHECK(adjacent_phis.success);
  CHECK(adjacent_phis.llvm_ir.find(
      "%before = phi float [ %before_a, %before_left ], [ %before_b, %before_right ]\n"
      "%target = phi float [ 1.000000e+00, %enabled ], [ 1.000000e+00, %disabled ]\n"
      "%after = phi float [ %after_a, %after_left ], [ %after_b, %after_right ]") !=
      std::string::npos);
  CHECK(adjacent_phis.llvm_ir.find(
      "%unrelated_use = fmul fast float %computed, %opacity") !=
      std::string::npos);
  CHECK(adjacent_phis.llvm_ir.find("%target = fadd") == std::string::npos);

  // A non-SSA line retains the dither-like text but must fail verification.
  const auto rejected = wuwa_tfr::PatchSelectedTargetDitherToIdentity(
      make_ir("not_ssa = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]"));
  CHECK(!rejected.success);
  CHECK(!rejected.structural_verification_succeeded);
  CHECK(!rejected.ir_patch_succeeded);

  const auto malformed = wuwa_tfr::PatchSelectedTargetDitherToIdentity(make_ir(
      "%dither_factor = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled"));
  CHECK(!malformed.success);
  CHECK(!malformed.ir_patch_succeeded);
  CHECK(malformed.llvm_ir.empty());

  const auto ambiguous = wuwa_tfr::PatchSelectedTargetDitherToIdentity(make_ir(
      "%first = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]\n"
      "%second = phi float [ %other_computed, %other_enabled ], [ 1.000000e+00, %other_disabled ]"));
  CHECK(!ambiguous.success);
  CHECK(!ambiguous.ir_patch_succeeded);
  CHECK(ambiguous.llvm_ir.empty());

  const auto make_dual_ir = [] {
    return std::string(R"(
@DITHER_THRESHOLDS_GLOBAL = external constant [9 x float]
%threshold1 = getelementptr inbounds [9 x float], [9 x float]* @DITHER_THRESHOLDS_GLOBAL, i32 0, i32 %index1
%first_load = load float, float* %threshold1
%first_computed = fadd fast float %first_load, 0.000000e+00
%unrelated_identity = phi float [ %unrelated_value, %unrelated_enabled ], [ 1.000000e+00, %unrelated_disabled ]
  %first_stage = phi float [ %first_computed, %first_enabled ], [ 1.000000e+00, %first_disabled ]
%threshold2 = getelementptr inbounds [9 x float], [9 x float]* @DITHER_THRESHOLDS_GLOBAL, i32 0, i32 %index2
%second_load = load float, float* %threshold2
%second_computed = fadd fast float %second_load, 0.000000e+00
  %second_stage = phi float [ %second_computed, %second_enabled ], [ 1.000000e+00, %second_disabled ]
%kill = fcmp fast olt float %score, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
)");
  };
  const auto dual = wuwa_tfr::PatchSelectedDualDitherStagesToIdentity(
      make_dual_ir());
  CHECK(dual.success);
  CHECK(dual.structural_verification_succeeded);
  CHECK(dual.ir_patch_succeeded);
  CHECK(dual.stage1_structural_verification_succeeded);
  CHECK(dual.stage1_ir_patch_succeeded);
  CHECK(dual.stage2_structural_verification_succeeded);
  CHECK(dual.stage2_ir_patch_succeeded);
  CHECK(dual.llvm_ir.find(
      "  %first_stage = phi float [ 1.000000e+00, %first_enabled ], [ 1.000000e+00, %first_disabled ]") !=
      std::string::npos);
  CHECK(dual.llvm_ir.find(
      "  %second_stage = phi float [ 1.000000e+00, %second_enabled ], [ 1.000000e+00, %second_disabled ]") !=
      std::string::npos);
  CHECK(dual.llvm_ir.find(" = fadd fast float 1.000000e+00, 0.000000e+00") ==
      std::string::npos);

  // The special experiment is fail-closed: one or three threshold stages is
  // not accepted as its required exactly-two-stage structure.
  const auto missing_stage = wuwa_tfr::PatchSelectedDualDitherStagesToIdentity(
      make_ir("%dither_factor = phi float [ %computed, %enabled ], [ 1.000000e+00, %disabled ]"));
  CHECK(!missing_stage.success);
  CHECK(!missing_stage.stage1_structural_verification_succeeded);
  CHECK(!missing_stage.stage2_structural_verification_succeeded);

  const std::string all_instances_ir = R"(
!899 = !{i32 0, !"SV_Position", i8 9}
@thresholds = internal constant [9 x float] zeroinitializer
define void @all_instances() {
entry:
%x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%cb0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 7)
%g0 = extractvalue %dx.types.CBufRet.f32 %cb0, 1
%c0 = fcmp fast ogt float %g0, 0.000000e+00
br i1 %c0, label %on0, label %merge0
; <label>:on0
%xi0 = fptosi float %x to i32
%yi0 = fptosi float %y to i32
%mx0 = srem i32 %xi0, 3
%my0 = srem i32 %yi0, 3
%row0 = mul nsw i32 %mx0, 3
%index0 = add nsw i32 %row0, %my0
%ptr0 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index0
%threshold0 = load float, float* %ptr0, align 4
%twice0 = fmul fast float %coverage0, 2.000000e+00
%sub0 = fsub fast float %twice0, %threshold0
%lo0 = call float @dx.op.binary.f32(i32 35, float %sub0, float 0.000000e+00)  ; FMax(a,b)
%hi0 = call float @dx.op.binary.f32(i32 36, float %lo0, float 1.000000e+00)  ; FMin(a,b)
%computed0 = fadd fast float %hi0, 0x3FD50F9F00000000
br label %merge0
; <label>:merge0
%d0 = phi float [ %computed0, %on0 ], [ 1.000000e+00, %entry0 ]
%cb1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 8)
%g1 = extractvalue %dx.types.CBufRet.f32 %cb1, 1
%c1 = fcmp fast ogt float %g1, 0.000000e+00
br i1 %c1, label %on1, label %merge1
; <label>:on1
%xi1 = fptosi float %x to i32
%yi1 = fptosi float %y to i32
%mx1 = srem i32 %xi1, 3
%my1 = srem i32 %yi1, 3
%row1 = mul nsw i32 %mx1, 3
%index1 = add nsw i32 %row1, %my1
%ptr1 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index1
%threshold1 = load float, float* %ptr1, align 4
%twice1 = fmul fast float %coverage1, 2.000000e+00
%sub1 = fsub fast float %twice1, %threshold1
%lo1 = call float @dx.op.binary.f32(i32 35, float %sub1, float 0.000000e+00)  ; FMax(a,b)
%hi1 = call float @dx.op.binary.f32(i32 36, float %lo1, float 1.000000e+00)  ; FMin(a,b)
%computed1 = fadd fast float %hi1, 0x3FD50F9F00000000
br label %merge1
; <label>:merge1
%d1 = phi float [ %computed1, %on1 ], [ 1.000000e+00, %entry1 ]
%alpha = fmul fast float %d0, %opacity
call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)
%kill = fcmp fast olt float %d1, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
}
!900 = !{i32 0, !"SV_Target", i8 9}
)";
  const auto all_instances =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
          all_instances_ir);
  CHECK(all_instances.success);
  CHECK(all_instances.structural_verification_succeeded);
  CHECK(all_instances.ir_patch_succeeded);
  CHECK(all_instances.verified_instance_count == 2);
  CHECK(all_instances.patched_instance_count == 2);
  CHECK(all_instances.llvm_ir.find(
      "%d0 = phi float [ 1.000000e+00, %on0 ], [ 1.000000e+00, %entry0 ]") !=
      std::string::npos);
  CHECK(all_instances.llvm_ir.find(
      "%d1 = phi float [ 1.000000e+00, %on1 ], [ 1.000000e+00, %entry1 ]") !=
      std::string::npos);

  // Production cannot authorize screen-space indexing from an unrelated
  // TEXCOORD signature merely because SV_Position metadata exists elsewhere.
  std::string texcoord_position_inputs = all_instances_ir +
      "!898 = !{i32 1, !\"TEXCOORD\", i8 9}\n";
  const std::string position_call =
      "@dx.op.loadInput.f32(i32 4, i32 0, i32 0";
  const std::string texcoord_call =
      "@dx.op.loadInput.f32(i32 4, i32 1, i32 0";
  for (std::size_t offset = 0;
       (offset = texcoord_position_inputs.find(position_call, offset)) !=
           std::string::npos;
       offset += texcoord_call.size())
    texcoord_position_inputs.replace(
        offset, position_call.size(), texcoord_call);
  CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(
      texcoord_position_inputs).instances.empty());
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
      texcoord_position_inputs).success);

  // A balanced signature tuple with an arbitrary metadata field is not a
  // complete SV_Target definition and cannot authorize Production patching.
  std::string invalid_target_metadata = all_instances_ir;
  const std::string valid_target =
      "!900 = !{i32 0, !\"SV_Target\", i8 9}";
  const std::size_t target_metadata =
      invalid_target_metadata.find(valid_target);
  CHECK(target_metadata != std::string::npos);
  invalid_target_metadata.replace(target_metadata, valid_target.size(),
      "!900 = !{i32 0, !\"SV_Target\", garbage}");
  const auto invalid_target_diagnostic =
      wuwa_tfr::AnalyzeFadePrimitiveV1(invalid_target_metadata);
  CHECK(invalid_target_diagnostic.instances.size() == 2);
  CHECK(invalid_target_diagnostic.instances.front().consumer ==
      wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
      invalid_target_metadata).success);

  // A second real shader shape sends one verified primitive to all three RGB
  // components of an SV_Target while another primitive controls discard.
  // Rejecting the first as a generic output used to reject the whole shader
  // and leave the character's outer surface faded.
  std::string rgb_and_discard = all_instances_ir;
  const std::string alpha_output =
      "%alpha = fmul fast float %d0, %opacity\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)";
  const std::string rgb_output =
      "%red = fmul fast float %d0, %color0\n"
      "%green = fmul fast float %d0, %color1\n"
      "%blue = fmul fast float %d0, %color2\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 0, float %red)\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 1, float %green)\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 2, float %blue)";
  const std::size_t alpha_output_offset = rgb_and_discard.find(alpha_output);
  CHECK(alpha_output_offset != std::string::npos);
  rgb_and_discard.replace(alpha_output_offset, alpha_output.size(), rgb_output);
  rgb_and_discard += "!901 = !{i32 3, !\"SV_Target\", i8 9}\n";
  const auto rgb_diagnostic =
      wuwa_tfr::AnalyzeFadePrimitiveV1(rgb_and_discard);
  CHECK(rgb_diagnostic.instances.size() == 2);
  CHECK(rgb_diagnostic.instances[0].consumer ==
      wuwa_tfr::FadePrimitiveConsumer::SvTargetRgb);
  CHECK(rgb_diagnostic.instances[1].consumer ==
      wuwa_tfr::FadePrimitiveConsumer::Discard);
  const auto rgb_patched =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
          rgb_and_discard);
  CHECK(rgb_patched.success);
  CHECK(rgb_patched.verified_instance_count == 2);
  CHECK(rgb_patched.patched_instance_count == 2);
  CHECK(rgb_patched.llvm_ir.find(
      "%d0 = phi float [ 1.000000e+00, %on0 ], [ 1.000000e+00, %entry0 ]") !=
      std::string::npos);
  CHECK(rgb_patched.llvm_ir.find(
      "%d1 = phi float [ 1.000000e+00, %on1 ], [ 1.000000e+00, %entry1 ]") !=
      std::string::npos);

  // Function/source identity, rather than local SSA spelling, selects each
  // independently verified phi. Both functions intentionally reuse every SSA
  // name in this fixture.
  const std::size_t body_start = all_instances_ir.find("entry:\n");
  const std::size_t body_end = all_instances_ir.rfind("\n}");
  CHECK(body_start != std::string::npos && body_end != std::string::npos);
  const std::string globals = all_instances_ir.substr(0,
      all_instances_ir.find("define void @all_instances"));
  const std::string body = all_instances_ir.substr(
      body_start + std::string("entry:\n").size(),
      body_end - (body_start + std::string("entry:\n").size()));
  const std::string two_functions = globals +
      "define void @first() {\nentry:\n" + body + "\n}\n" +
      "define void @second() {\nentry:\n" + body + "\n}\n" +
      "!900 = !{i32 0, !\"SV_Target\", i8 9}\n";
  const auto independently_patched =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(two_functions);
  CHECK(independently_patched.success);
  CHECK(independently_patched.verified_instance_count == 4);
  CHECK(independently_patched.patched_instance_count == 4);

  // An unrelated same-named phi in function A is never located in place of
  // the verified target in function B.
  const std::string unrelated = globals + R"(define void @unrelated() {
entry:
%d0 = phi float [ %unrelated_a, %left ], [ 1.000000e+00, %right ]
}
)" + "define void @target() {\nentry:\n" + body + "\n}\n";
  const std::string unrelated_with_metadata = unrelated +
      "!900 = !{i32 0, !\"SV_Target\", i8 9}\n";
  const auto exact_target =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
          unrelated_with_metadata);
  CHECK(exact_target.success);
  CHECK(exact_target.llvm_ir.find(
      "%d0 = phi float [ %unrelated_a, %left ], [ 1.000000e+00, %right ]") !=
      std::string::npos);
  CHECK(exact_target.llvm_ir.find(
      "%d0 = phi float [ 1.000000e+00, %on0 ], [ 1.000000e+00, %entry0 ]") !=
      std::string::npos);

  // Diagnostic classifications are useful, but Production accepts only the
  // four explicit visibility consumers.
  auto unknown_consumer = all_instances_ir;
  const std::size_t output = unknown_consumer.find("@dx.op.storeOutput.f32");
  unknown_consumer.replace(output, std::string("@dx.op.storeOutput.f32").size(),
      "@dx.op.notAConsumer");
  const auto unknown_diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(unknown_consumer);
  CHECK(!unknown_diagnostic.instances.empty());
  CHECK(unknown_diagnostic.instances.front().consumer ==
      wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
      unknown_consumer).success);
  auto other_consumer = all_instances_ir;
  const std::size_t component = other_consumer.find("i8 3, float %alpha");
  other_consumer.replace(component, std::string("i8 3").size(), "i8 2");
  const auto other_diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(other_consumer);
  CHECK(!other_diagnostic.instances.empty());
  CHECK(other_diagnostic.instances.front().consumer ==
      wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
      other_consumer).success);
  return 0;
}

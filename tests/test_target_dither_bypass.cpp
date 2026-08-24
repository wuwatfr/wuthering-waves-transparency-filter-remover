// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "target_dither_bypass.hpp"

#include "fade_primitive_detector.hpp"

#include "test_check.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string LongChain(int length) {
  std::string chain = "%chain0 = fadd fast float 0.000000e+00, 1.000000e+00\n";
  for (int i = 1; i <= length; ++i)
    chain += "%chain" + std::to_string(i) + " = fadd fast float %chain" +
        std::to_string(i - 1) + ", 1.000000e+00\n";
  return chain;
}

const wuwa_tfr::FadePrimitiveInstance* FindByMergeValue(
    const std::vector<wuwa_tfr::FadePrimitiveInstance>& instances,
    std::string_view merge_value) {
  for (const auto& instance : instances)
    if (instance.merge_value == merge_value) return &instance;
  return nullptr;
}

}  // namespace

int main() {
  // A same-row adjacent, single-instance fixture: %coverage0's pre-Fade FMin
  // combines two adjacent scalars (z, w) from one cbufferLoadLegacy row --
  // the canonical qualifying shape.
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
%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)
%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2
%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3
%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)
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
%cb1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 8)
%g1 = extractvalue %dx.types.CBufRet.f32 %cb1, 1
%c1 = fcmp fast ogt float %g1, 0.000000e+00
br i1 %c1, label %on1, label %merge1
; <label>:on1
%pf1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb3, i32 55)
%opA1 = extractvalue %dx.types.CBufRet.f32 %pf1, 1
%opB1 = extractvalue %dx.types.CBufRet.f32 %pf1, 2
%coverage1 = call float @dx.op.binary.f32(i32 36, float %opA1, float %opB1)  ; FMin(a,b)
%xi1 = fptosi float %x to i32
%yi1 = fptosi float %y to i32
%mx1 = srem i32 %xi1, 3
%my1 = srem i32 %yi1, 3
%row1 = mul nsw i32 %mx1, 3
%index1 = add nsw i32 %row1, %my1
%ptr1 = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index1
%threshold1 = load float, float* %ptr1, align 4, !tbaa !50, !noalias !54
%twice1 = fmul fast float %coverage1, 2.000000e+00
%sub1 = fsub fast float %twice1, %threshold1
%lo1 = call float @dx.op.binary.f32(i32 35, float %sub1, float 0.000000e+00)  ; FMax(a,b)
%hi1 = call float @dx.op.binary.f32(i32 36, float %lo1, float 1.000000e+00)  ; FMin(a,b)
%computed1 = fadd fast float %hi1, 0x3FD50F9F00000000
br label %merge1
; <label>:merge1
%d1 = phi float [ %computed1, %on1 ], [ 1.000000e+00, %entry1 ]
%d0_sat = call float @dx.op.unary.f32(i32 7, float %d0)  ; Saturate(value)
%alpha = fmul fast float %d0_sat, %opacity
call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)
%d1_clamped = call float @dx.op.binary.f32(i32 36, float %d1, float 1.000000e+00)  ; FMin(a,b)
%kill = fcmp fast olt float %d1_clamped, 0.000000e+00
call void @dx.op.discard(i32 82, i1 %kill)
}
!900 = !{i32 0, !"SV_Target", i8 9}
)";

  // ---- successful shapes ----

  // Multiple verified instances in one shader, both same-row adjacent.
  const auto all_instances =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          all_instances_ir);
  CHECK(all_instances.success);
  CHECK(all_instances.structural_verification_succeeded);
  CHECK(all_instances.ir_patch_succeeded);
  CHECK(all_instances.verified_instance_count == 2);
  CHECK(all_instances.qualifying_instance_count == 2);
  CHECK(all_instances.patched_instance_count == 2);
  // Only operand 1 of each qualifying FMin changes.
  CHECK(all_instances.llvm_ir.find(
      "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB0)") !=
      std::string::npos);
  CHECK(all_instances.llvm_ir.find(
      "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB1)") !=
      std::string::npos);
  // Operand 2 and every downstream instruction remain byte-for-byte
  // unchanged: the phi, gate, coverage/dither expression, saturate, alpha
  // output, and discard predicate are all still present verbatim.
  for (std::string_view unchanged : {
           std::string_view("%d0 = phi float [ %computed0, %on0 ], [ 1.000000e+00, %entry0 ]"),
           std::string_view("%d1 = phi float [ %computed1, %on1 ], [ 1.000000e+00, %entry1 ]"),
           std::string_view("%c0 = fcmp fast ogt float %g0, 0.000000e+00"),
           std::string_view("br i1 %c0, label %on0, label %merge0"),
           std::string_view("%hi0 = call float @dx.op.binary.f32(i32 36, float %lo0, float 1.000000e+00)  ; FMin(a,b)"),
           std::string_view("%d0_sat = call float @dx.op.unary.f32(i32 7, float %d0)  ; Saturate(value)"),
           std::string_view("call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)"),
           std::string_view("call void @dx.op.discard(i32 82, i1 %kill)")}) {
    CHECK(all_instances_ir.find(unchanged) != std::string::npos);
    CHECK(all_instances.llvm_ir.find(unchanged) != std::string::npos);
  }
  CHECK(all_instances.llvm_ir.find("float %opA0") == std::string::npos);
  CHECK(all_instances.llvm_ir.find("float %opA1") == std::string::npos);

  // The evidence behind this same patch: exactly the analyses that
  // authorized it, associated with their instances by value (not by a
  // shared vector index), and reproducing exactly the text that got
  // rewritten -- proving this is the analysis actually used, not a
  // re-derived stand-in.
  CHECK(all_instances.instance_evidence.size() == 2);
  {
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(all_instances_ir);
    CHECK(diagnostic.instances.size() == 2);
    for (const auto& evidence : all_instances.instance_evidence) {
      CHECK(evidence.analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
      const auto* original_instance =
          FindByMergeValue(diagnostic.instances, evidence.instance.merge_value);
      CHECK(original_instance != nullptr);
      CHECK(original_instance->function_identity == evidence.instance.function_identity);
      CHECK(original_instance->consumer == evidence.instance.consumer);
      const auto& operand = evidence.analysis.operand_one;
      CHECK(operand.source_end > operand.source_start);
      CHECK(operand.source_end <= all_instances_ir.size());
      CHECK(all_instances_ir.substr(operand.source_start,
          operand.source_end - operand.source_start) == operand.source_text);
      CHECK(operand.source_text != wuwa_tfr::kPreFadeRewriteLiteral);
    }
    const bool has_d0 =
        all_instances.instance_evidence[0].instance.merge_value == "%d0" ||
        all_instances.instance_evidence[1].instance.merge_value == "%d0";
    const bool has_d1 =
        all_instances.instance_evidence[0].instance.merge_value == "%d1" ||
        all_instances.instance_evidence[1].instance.merge_value == "%d1";
    CHECK(has_d0 && has_d1);
  }

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
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
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
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
      invalid_target_metadata).success);

  // A second real shader shape sends one verified primitive to all three RGB
  // components of an SV_Target while another primitive controls discard.
  std::string rgb_and_discard = all_instances_ir;
  const std::string alpha_output =
      "%d0_sat = call float @dx.op.unary.f32(i32 7, float %d0)  ; Saturate(value)\n"
      "%alpha = fmul fast float %d0_sat, %opacity\n"
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
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          rgb_and_discard);
  CHECK(rgb_patched.success);
  CHECK(rgb_patched.verified_instance_count == 2);
  CHECK(rgb_patched.patched_instance_count == 2);
  CHECK(rgb_patched.llvm_ir.find(
      "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB0)") !=
      std::string::npos);
  CHECK(rgb_patched.llvm_ir.find(
      "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB1)") !=
      std::string::npos);

  // Function/source identity, rather than local SSA spelling, selects each
  // independently verified instance. Both functions intentionally reuse
  // every SSA name in this fixture.
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
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(two_functions);
  CHECK(independently_patched.success);
  CHECK(independently_patched.verified_instance_count == 4);
  CHECK(independently_patched.patched_instance_count == 4);

  // An unrelated same-named phi in function A is never located in place of
  // the verified target in function B -- malformed/ambiguous source identity
  // must not silently patch the wrong site.
  const std::string unrelated = globals + R"(define void @unrelated() {
entry:
%d0 = phi float [ %unrelated_a, %left ], [ 1.000000e+00, %right ]
}
)" + "define void @target() {\nentry:\n" + body + "\n}\n";
  const std::string unrelated_with_metadata = unrelated +
      "!900 = !{i32 0, !\"SV_Target\", i8 9}\n";
  const auto exact_target =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          unrelated_with_metadata);
  CHECK(exact_target.success);
  CHECK(exact_target.llvm_ir.find(
      "%d0 = phi float [ %unrelated_a, %left ], [ 1.000000e+00, %right ]") !=
      std::string::npos);
  CHECK(exact_target.llvm_ir.find(
      "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB0)") !=
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
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
      unknown_consumer).success);
  auto other_consumer = all_instances_ir;
  const std::size_t component = other_consumer.find("i8 3, float %alpha");
  other_consumer.replace(component, std::string("i8 3").size(), "i8 2");
  const auto other_diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(other_consumer);
  CHECK(!other_diagnostic.instances.empty());
  CHECK(other_diagnostic.instances.front().consumer ==
      wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
  CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
      other_consumer).success);

  // ---- pre-Fade FMin structural shapes: cross-row adjacent ----
  {
    std::string cross_row = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string cross_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%pf0b = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 41)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0b, 0\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::size_t at = cross_row.find(same_row_prefix);
    CHECK(at != std::string::npos);
    cross_row.replace(at, same_row_prefix.size(), cross_row_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(cross_row);
    CHECK(result.success);
    CHECK(result.llvm_ir.find(
        "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB0)") !=
        std::string::npos);
  }

  // ---- pre-Fade FMin structural shapes: known non-adjacent census variant ----
  {
    std::string non_adjacent = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string non_adjacent_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 0\n"
        "%pf0b = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 55)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0b, 2\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::size_t at = non_adjacent.find(same_row_prefix);
    CHECK(at != std::string::npos);
    non_adjacent.replace(at, same_row_prefix.size(), non_adjacent_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(non_adjacent);
    CHECK(result.success);
    CHECK(result.llvm_ir.find(
        "call float @dx.op.binary.f32(i32 36, float 1.000000e+00, float %opB0)") !=
        std::string::npos);
  }

  // ---- fail-closed: no qualifying FMin (operand traces to a spatial input) ----
  {
    std::string absent = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string spatial_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 0, i32 undef)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::size_t at = absent.find(same_row_prefix);
    CHECK(at != std::string::npos);
    absent.replace(at, same_row_prefix.size(), spatial_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(absent);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("no qualifying pre-Fade FMin") != std::string::npos);
    // The rejected instance is first in iteration order and never reaches a
    // Matched analysis, so no evidence is fabricated for it.
    CHECK(result.instance_evidence.empty());
  }

  // ---- fail-closed: operands from different CBV handles ----
  {
    std::string different_handles = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string different_handle_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%pf0c = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cbOther, i32 40)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0c, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::size_t at = different_handles.find(same_row_prefix);
    CHECK(at != std::string::npos);
    different_handles.replace(at, same_row_prefix.size(), different_handle_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(different_handles);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.error.find("no qualifying pre-Fade FMin") != std::string::npos);
  }

  // ---- fail-closed: multiple qualifying FMin candidates (ambiguous) ----
  {
    std::string ambiguous = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string ambiguous_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%fmin1_0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)\n"
        "%pf0c = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 90)\n"
        "%opC0 = extractvalue %dx.types.CBufRet.f32 %pf0c, 0\n"
        "%opD0 = extractvalue %dx.types.CBufRet.f32 %pf0c, 1\n"
        "%fmin2_0 = call float @dx.op.binary.f32(i32 36, float %opC0, float %opD0)  ; FMin(a,b)\n"
        "%coverage0 = fadd fast float %fmin1_0, %fmin2_0";
    const std::size_t at = ambiguous.find(same_row_prefix);
    CHECK(at != std::string::npos);
    ambiguous.replace(at, same_row_prefix.size(), ambiguous_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ambiguous);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("ambiguous") != std::string::npos);
  }

  // ---- fail-closed: only the second instance's analysis fails ----
  // The first instance (%d0) is untouched and reaches a Matched analysis;
  // the second (%d1) is broken the same way the "absent" case broke the
  // first. Evidence must contain exactly the completed prefix -- one entry,
  // for %d0 -- never a fabricated entry for the instance that failed.
  {
    std::string second_broken = all_instances_ir;
    const std::string second_instance_prefix =
        "%pf1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb3, i32 55)\n"
        "%opA1 = extractvalue %dx.types.CBufRet.f32 %pf1, 1\n"
        "%opB1 = extractvalue %dx.types.CBufRet.f32 %pf1, 2\n"
        "%coverage1 = call float @dx.op.binary.f32(i32 36, float %opA1, float %opB1)  ; FMin(a,b)";
    const std::string second_instance_broken =
        "%pf1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb3, i32 55)\n"
        "%opA1 = extractvalue %dx.types.CBufRet.f32 %pf1, 1\n"
        "%opB1 = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 0, i32 undef)\n"
        "%coverage1 = call float @dx.op.binary.f32(i32 36, float %opA1, float %opB1)  ; FMin(a,b)";
    const std::size_t at = second_broken.find(second_instance_prefix);
    CHECK(at != std::string::npos);
    second_broken.replace(at, second_instance_prefix.size(), second_instance_broken);
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(second_broken);
    CHECK(diagnostic.instances.size() == 2);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(second_broken);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("no qualifying pre-Fade FMin") != std::string::npos);
    CHECK(result.instance_evidence.size() == 1);
    CHECK(result.instance_evidence[0].instance.merge_value == "%d0");
    CHECK(result.instance_evidence[0].analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::Matched);
  }

  // ---- fail-closed: two verified instances sharing one rewrite range ----
  // Both phis take their enabled value from the same expression, so both
  // resolve to the same operand-1 byte range. Rewriting it twice would mean
  // the second rewrite operating on text the first already replaced, so this
  // is rejected up front rather than being caught as a stale-text mismatch.
  {
    std::string shared = all_instances_ir;
    const std::string second_phi =
        "%d1 = phi float [ %computed1, %on1 ], [ 1.000000e+00, %entry1 ]";
    const std::string shared_phi =
        "%d1 = phi float [ %computed0, %on1 ], [ 1.000000e+00, %entry1 ]";
    const std::size_t at = shared.find(second_phi);
    CHECK(at != std::string::npos);
    shared.replace(at, second_phi.size(), shared_phi);
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(shared);
    CHECK(diagnostic.instances.size() == 2);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(shared);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("share a pre-Fade rewrite range") != std::string::npos);
    // Both instances individually reached a Matched analysis before the
    // shared-range check ran; a failure discovered only after the per-
    // instance loop completed does not truncate already-completed evidence.
    CHECK(result.instance_evidence.size() == 2);
    for (const auto& evidence : result.instance_evidence)
      CHECK(evidence.analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
  }

  // ---- post-patch: the verified primitive itself survives byte-identically ----
  // The retired identity-phi patch removed the primitive; this one must not.
  // Every instance must still be verifiable, with the same function, merge SSA
  // name and consumer, and every phi line must be unchanged.
  {
    const auto before = wuwa_tfr::AnalyzeFadePrimitiveV1(all_instances_ir);
    const auto after = wuwa_tfr::AnalyzeFadePrimitiveV1(all_instances.llvm_ir);
    CHECK(before.instances.size() == 2);
    CHECK(after.instances.size() == before.instances.size());
    for (std::size_t i = 0; i < before.instances.size(); ++i) {
      CHECK(before.instances[i].function_identity == after.instances[i].function_identity);
      CHECK(before.instances[i].merge_value == after.instances[i].merge_value);
      CHECK(before.instances[i].consumer == after.instances[i].consumer);
      const std::string original_phi = all_instances_ir.substr(
          before.instances[i].phi_start,
          before.instances[i].phi_end - before.instances[i].phi_start);
      const std::string patched_phi = all_instances.llvm_ir.substr(
          after.instances[i].phi_start,
          after.instances[i].phi_end - after.instances[i].phi_start);
      CHECK(original_phi == patched_phi);
    }
    // Exactly the two operand-1 tokens changed and nothing else: the patched
    // IR is the original with those two substitutions and no other edit.
    CHECK(all_instances.llvm_ir.size() ==
        all_instances_ir.size() + 2 * (std::string("1.000000e+00").size() -
            std::string("%opA0").size()));
  }

  // ---- fail-closed: incomplete backward slice ----
  {
    std::string incomplete = all_instances_ir;
    const std::string same_row_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";
    const std::string long_chain_prefix =
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%fmin_ok0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)\n" +
        LongChain(1100) +
        "%coverage0 = fadd fast float %fmin_ok0, %chain1100";
    const std::size_t at = incomplete.find(same_row_prefix);
    CHECK(at != std::string::npos);
    incomplete.replace(at, same_row_prefix.size(), long_chain_prefix);
    const auto result =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(incomplete);
    CHECK(!result.success);
    CHECK(result.patched_instance_count == 0);
    CHECK(result.llvm_ir.empty());
    CHECK(result.error.find("incomplete") != std::string::npos);
  }

  return 0;
}

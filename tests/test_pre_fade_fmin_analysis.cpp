// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Focused tests for the canonical pre-Fade FMin analyzer's structural proof:
// a direct scalar CBV load per operand, one shared CBV handle, a unique
// candidate in a complete verified slice, and the two operands proven to be
// adjacent scalars of that constant buffer.
//
// Operand adjacency used to be a purely diagnostic coordinate that could
// degrade to Unknown without affecting the verdict. It is now an
// authorization condition, in two parts:
//
//   * the pair must be proven adjacent -- SameRow and CrossRow both qualify,
//     and this classification stays sign-agnostic and diagnostic;
//   * the pair must carry the one validated orientation, operand 2 exactly
//     +4 bytes after operand 1.
//
// A reversed pair therefore fails closed rather than having the lower-address
// operand selected automatically. None of this identifies a camera side: the
// rewrite still targets syntactic operand 1, and the orientation rule only
// refuses shapes that were never validated.
//
// The invariant that does still hold throughout: no analysis failure of any
// kind may ever be read as proof that a qualifying FMin is absent.
//
// Corpus at the time of the change (2026-08-25, 3269 shaders): 262/262
// qualifying instances resolved adjacent with operand2 - operand1 == +4 --
// SameRow 204, CrossRow 58, NonAdjacent 0, Unknown 0, -4 zero -- so neither
// half of the gate rejects anything observed.

#include "pre_fade_fmin_analysis.hpp"

#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"

#include "test_check.hpp"
#include <string>

namespace {

// One verified Fade Primitive instance (%d0, SV_Target alpha) whose enabled
// arm's backward slice contains a single same-CBV pre-Fade FMin. Callers
// substitute kPreFadeRegion to vary just the pre-Fade shape.
const char* const kPreFadeRegion =
    "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
    "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
    "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
    "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)  ; FMin(a,b)";

std::string BuildIr(const std::string& pre_fade_region) {
  return std::string(R"(
!899 = !{i32 0, !"SV_Position", i8 9}
@thresholds = internal constant [9 x float] zeroinitializer
define void @analysis_fixture() {
entry:
%x = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%y = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%cb0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 7)
%g0 = extractvalue %dx.types.CBufRet.f32 %cb0, 1
%c0 = fcmp fast ogt float %g0, 0.000000e+00
br i1 %c0, label %on0, label %merge0
; <label>:on0
)") + pre_fade_region + R"(
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
}
!900 = !{i32 0, !"SV_Target", i8 9}
)";
}

wuwa_tfr::PreFadeFMinAnalysis AnalyzeOnly(const std::string& ir) {
  const wuwa_tfr::FadePrimitiveDiagnostic diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
  CHECK(diagnostic.instances.size() == 1);
  return wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, diagnostic.instances.front());
}

// A structurally complete analysis that found exactly zero candidates -- the
// only shape that may be read as proof of absence.
wuwa_tfr::PreFadeFMinAnalysis ProvenAbsent() {
  wuwa_tfr::PreFadeFMinAnalysis analysis;
  analysis.status = wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate;
  analysis.instance_identity_verified = true;
  analysis.function_located = true;
  analysis.function_parsed = true;
  analysis.backward_slice_complete = true;
  analysis.qualifying_fmin_count = 0;
  return analysis;
}

}  // namespace

int main() {
  // ================= Production proof: structural source identity =========

  // Legacy row form: dx.op.cbufferLoadLegacy.f32 + extractvalue.
  {
    const auto analysis = AnalyzeOnly(BuildIr(kPreFadeRegion));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
    CHECK(analysis.success);
    CHECK(analysis.qualifying_fmin_count == 1);
    CHECK(analysis.operand_one.resolved && analysis.operand_two.resolved);
    CHECK(analysis.operand_one.legacy_form && analysis.operand_two.legacy_form);
    CHECK(analysis.operand_one.handle_value == analysis.operand_two.handle_value);
    CHECK(analysis.operand_one.source_text == "%opA0");
    // Diagnostic coordinates are available for this shape.
    CHECK(analysis.operand_one.row_resolved && analysis.operand_one.row == 40);
    CHECK(analysis.operand_one.component_resolved && analysis.operand_one.component == 2);
    CHECK(analysis.operand_two.component_resolved && analysis.operand_two.component == 3);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::SameRow);
  }

  // Byte-addressed form: bare dx.op.cbufferLoad.f32, no extractvalue.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648, i32 4)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
    CHECK(!analysis.operand_one.legacy_form && !analysis.operand_two.legacy_form);
    CHECK(analysis.operand_one.handle_value == "%cb2");
    CHECK(analysis.operand_one.byte_offset_resolved && analysis.operand_one.byte_offset == 648);
    CHECK(analysis.operand_two.byte_offset_resolved && analysis.operand_two.byte_offset == 652);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::CrossRow);
    CHECK(analysis.operand_one.source_text == "%opA0");
  }

  // ================= adjacency is an authorization condition ==============

  // Legacy cross-row, +4: operand one at row 40 component 3 (byte 652),
  // operand two at row 41 component 0 (byte 656). The pair straddles a row
  // boundary, which is exactly as adjacent as a same-row pair.
  {
    const std::string ir = BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%pf1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 41)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf1, 0\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)");
    const auto analysis = AnalyzeOnly(ir);
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
    CHECK(analysis.success);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::CrossRow);
    const auto patched =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir);
    CHECK(patched.success);
    CHECK(patched.patched_instance_count == 1);
  }

  // Legacy cross-row, -4: the same pair with the two loads swapped, so
  // operand one is now the HIGHER offset. Adjacency still classifies it as
  // CrossRow -- that classification stays sign-agnostic and diagnostic -- but
  // authorization requires the validated +4 orientation, so this fails closed
  // instead of quietly rewriting the other side of the pair.
  {
    const std::string ir = BuildIr(
        "%pf1 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 41)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf1, 0\n"
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)");
    const auto analysis = AnalyzeOnly(ir);
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::OperandsReversed);
    CHECK(!analysis.success);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::CrossRow);
    CHECK(analysis.error.find("reversed") != std::string::npos);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir)
               .success);
  }

  // Legacy same-row, -4: components 3 then 2 of one row. Same verdict, and
  // reported as a reversed pair rather than as a non-adjacent one.
  {
    const std::string ir = BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)");
    const auto analysis = AnalyzeOnly(ir);
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::OperandsReversed);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::SameRow);
    CHECK(analysis.qualifying_fmin_count == 1);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir)
               .success);
  }

  // Resolved but genuinely far apart: same row, components 0 and 3, 12 bytes
  // apart. Two scalars of one constant buffer that are not a neighbouring
  // pair, which is the shape the gate exists to reject.
  {
    const std::string ir = BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 0\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)");
    const auto analysis = AnalyzeOnly(ir);
    CHECK(analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::OperandsNotAdjacent);
    CHECK(!analysis.success);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::NonAdjacent);
    // A candidate did exist -- this is not "no qualifying FMin".
    CHECK(analysis.qualifying_fmin_count == 1);
    CHECK(analysis.error.find("not adjacent") != std::string::npos);
    const auto patched =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir);
    CHECK(!patched.success);
    CHECK(patched.patched_instance_count == 0);
  }

  // A dynamically indexed legacy row: the source identity is intact, but the
  // coordinates needed to prove adjacency are not derivable. Unproven is not
  // proven, so this fails closed -- and is reported as an adjacency rejection,
  // not as an absent candidate.
  {
    const std::string ir = BuildIr(
        "%dynrow = call i32 @dx.op.loadInput.i32(i32 4, i32 3, i32 0, i8 0, i32 undef)\n"
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 %dynrow)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)");
    const auto analysis = AnalyzeOnly(ir);
    CHECK(analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::OperandsNotAdjacent);
    CHECK(!analysis.success);
    // The structural source identity is still perfectly valid...
    CHECK(analysis.operand_one.resolved && analysis.operand_two.resolved);
    CHECK(analysis.operand_one.handle_value == analysis.operand_two.handle_value);
    CHECK(analysis.operand_one.component_resolved);
    // ...but the row it would need is not, so adjacency stays Unknown.
    CHECK(!analysis.operand_one.row_resolved);
    CHECK(!analysis.operand_two.row_resolved);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::Unknown);
    CHECK(analysis.error.find("could not be resolved") != std::string::npos);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir)
               .success);
  }

  // Same for a dynamically computed byte offset.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%dynoff = call i32 @dx.op.loadInput.i32(i32 4, i32 3, i32 0, i8 0, i32 undef)\n"
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 %dynoff, i32 4)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::OperandsNotAdjacent);
    CHECK(!analysis.operand_one.byte_offset_resolved);
    CHECK(analysis.operand_two.byte_offset_resolved);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::Unknown);
  }

  // Byte-offset form, resolved and far apart: rejected on the same footing as
  // the legacy form. Adjacency support is not legacy-only.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648, i32 4)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 700, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::OperandsNotAdjacent);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::NonAdjacent);
    CHECK(analysis.qualifying_fmin_count == 1);
  }

  // Byte-offset form, -4: operand one at the higher offset. Adjacent, but not
  // the validated orientation, so it is rejected on the same footing as the
  // legacy form -- the orientation rule is not legacy-only.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::OperandsReversed);
    CHECK(!analysis.success);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::CrossRow);
  }

  // Different CBV handles still fail closed -- the one same-handle rule the
  // hypothesis validated is not relaxed by any of the above.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%pf0b = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cbOther, i32 40)\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0b, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
    CHECK(analysis.qualifying_fmin_count == 0);
  }

  // Mixed forms from the same handle are still the same handle, but a legacy
  // row/component and a raw byte offset are not comparable coordinates, so
  // adjacency cannot be proven and the match fails closed.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status ==
        wuwa_tfr::PreFadeFMinStatus::OperandsNotAdjacent);
    CHECK(analysis.adjacency == wuwa_tfr::PreFadeAdjacency::Unknown);
  }

  // ================= exact opcode/type/operand validation =================

  // A different dx.op.binary opcode (FMax, 35) is not an FMin.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 35, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // A wrong cbuffer-load opcode number is not a CBV load, even though the
  // intrinsic name matches.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 60, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // ---- the call suffix grammar: <nothing> | #N | #N <md> | <md> ----------
  // A call may carry an attribute-group reference and/or instruction metadata
  // attachments after its closing parenthesis, and nothing else. The three
  // shapes DXC actually emits for these intrinsics are the empty suffix, `#N`
  // and `, !dx.precise !N`. Validation must be exact, not first-character.
  for (const char* suffix : {
           "",
           " #0",
           " #12",
           ", !dx.precise !1",
           " #0, !dbg !77",
           " #0 , !dx.precise !1 , !dbg !77",
           ",  !tbaa   !50 ,  !noalias  !54 ",
       }) {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)" +
        std::string(suffix) + "\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)" +
        std::string(suffix)));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::Matched);
  }

  // Everything else is malformed and must fail closed -- including forms whose
  // first character is one a laxer check would have waved through.
  for (const char* suffix : {
           " float %opB0",        // a stray extra operand
           " #",                  // attribute group with no number
           " #garbage",           // attribute group that is not a number
           " #0 garbage",         // trailing junk after a valid attribute group
           " #0garbage",
           ", garbage",           // comma-led, but not a metadata attachment
           ", !dbg",              // attachment name with no reference
           ", !dbg garbage",      // reference that is not a metadata id
           ", !dbg !",            // metadata reference with no id
           ", !dbg!77",           // missing the mandatory separating space
           ", ! !77",             // empty attachment name
           " !dbg !77",           // bare attachment: instruction metadata
                                  // always carries its leading comma, so this
                                  // is not valid call syntax
           ", !dbg !77 trailing",
           ", !dbg !77,",         // dangling separator
       }) {
    // On the FMin itself...
    const auto fmin = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)" +
        std::string(suffix)));
    CHECK(fmin.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
    // ...and on the constant-buffer load feeding it.
    const auto load = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)" +
        std::string(suffix) + "\n"
        "%opA0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(load.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // dx.op.cbufferLoad is exactly (opcode, handle, byteOffset, alignment);
  // DXC's own validator rejects any other arity, so neither may this parser
  // accept one.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // ...and its alignment is a constant, not an index: DXIL requires a
  // ConstantInt there. byteOffset stays literal-or-SSA; alignment does not.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%align = call i32 @dx.op.loadInput.i32(i32 4, i32 3, i32 0, i8 0, i32 undef)\n"
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648, i32 %align)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // A fifth operand is not this intrinsic either.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%opA0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 648, i32 4, i32 7)\n"
        "%opB0 = call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb2, i32 652, i32 4, i32 7)\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // An extraction from an unrelated aggregate type is not a CBV scalar load.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.ResRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opA0 = extractvalue %dx.types.ResRet.f32 %pf0, 2\n"
        "%opB0 = extractvalue %dx.types.ResRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // An indirect operand (a copy chain through a phi/select/bitcast) is not a
  // *direct* load and must not qualify.
  {
    const auto analysis = AnalyzeOnly(BuildIr(
        "%pf0 = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
        "%opRaw0 = extractvalue %dx.types.CBufRet.f32 %pf0, 2\n"
        "%opA0 = select i1 true, float %opRaw0, float %opRaw0\n"
        "%opB0 = extractvalue %dx.types.CBufRet.f32 %pf0, 3\n"
        "%coverage0 = call float @dx.op.binary.f32(i32 36, float %opA0, float %opB0)"));
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::NoQualifyingCandidate);
  }

  // ================= no failure may pass as proof of absence ==============

  // The exact expected post-patch state, and only it, proves absence.
  CHECK(wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(ProvenAbsent()));
  CHECK(wuwa_tfr::PreFadeFMinAnalysisIsStructurallyComplete(ProvenAbsent()));

  // A default-constructed (i.e. never-run) analysis proves nothing.
  CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(wuwa_tfr::PreFadeFMinAnalysis{}));

  // Every structural failure status proves nothing -- not even with a zero
  // candidate count and every other stage flag optimistically set.
  for (const wuwa_tfr::PreFadeFMinStatus status :
      {wuwa_tfr::PreFadeFMinStatus::InvalidInstanceIdentity,
          wuwa_tfr::PreFadeFMinStatus::FunctionNotUniquelyLocated,
          wuwa_tfr::PreFadeFMinStatus::FunctionNotParsable,
          wuwa_tfr::PreFadeFMinStatus::IncompleteBackwardSlice}) {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.status = status;
    CHECK(!wuwa_tfr::PreFadeFMinAnalysisIsStructurallyComplete(analysis));
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }

  // Nor does any single missing stage flag, even with the right status.
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.instance_identity_verified = false;
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.function_located = false;
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.function_parsed = false;
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.backward_slice_complete = false;
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }

  // A still-present candidate never proves absence, matched or ambiguous.
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.status = wuwa_tfr::PreFadeFMinStatus::AmbiguousCandidates;
    analysis.qualifying_fmin_count = 2;
    CHECK(wuwa_tfr::PreFadeFMinAnalysisIsStructurallyComplete(analysis));
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }
  {
    wuwa_tfr::PreFadeFMinAnalysis analysis = ProvenAbsent();
    analysis.status = wuwa_tfr::PreFadeFMinStatus::Matched;
    analysis.success = true;
    analysis.qualifying_fmin_count = 1;
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));
  }

  // A real analysis failure -- here, a source identity the analyzer cannot
  // re-derive -- reports a structural status, never NoQualifyingCandidate.
  {
    const std::string ir = BuildIr(kPreFadeRegion);
    wuwa_tfr::FadePrimitiveDiagnostic diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
    CHECK(diagnostic.instances.size() == 1);
    wuwa_tfr::FadePrimitiveInstance instance = diagnostic.instances.front();
    instance.function_identity = "@not_present";
    const auto analysis = wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, instance);
    CHECK(analysis.status == wuwa_tfr::PreFadeFMinStatus::FunctionNotUniquelyLocated);
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(analysis));

    wuwa_tfr::FadePrimitiveInstance broken_phi = diagnostic.instances.front();
    broken_phi.merge_value = "%not_the_merge";
    const auto phi_analysis = wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, broken_phi);
    CHECK(phi_analysis.status == wuwa_tfr::PreFadeFMinStatus::InvalidInstanceIdentity);
    CHECK(!wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(phi_analysis));
  }

  // ================= the rewrite proof is positive, not inferred ==========
  {
    const std::string ir = BuildIr(kPreFadeRegion);
    const wuwa_tfr::FadePrimitiveDiagnostic diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
    CHECK(diagnostic.instances.size() == 1);
    const wuwa_tfr::PreFadeFMinAnalysis matched =
        wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, diagnostic.instances.front());
    CHECK(matched.status == wuwa_tfr::PreFadeFMinStatus::Matched);

    // Against the unpatched IR the proof must fail: the FMin is still intact.
    std::string error;
    CHECK(!wuwa_tfr::VerifyPreFadeFMinOperandOneRewritten(
        ir, diagnostic.instances.front(), matched, error));
    CHECK(!error.empty());

    // Against the correctly patched IR it must hold.
    const auto patched = wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir);
    CHECK(patched.success);
    const wuwa_tfr::FadePrimitiveDiagnostic post =
        wuwa_tfr::AnalyzeFadePrimitiveV1(patched.llvm_ir);
    CHECK(post.instances.size() == 1);
    error.clear();
    CHECK(wuwa_tfr::VerifyPreFadeFMinOperandOneRewritten(
        patched.llvm_ir, post.instances.front(), matched, error));

    // A rewrite that emptied the candidate set by damaging the *operand's*
    // load instead of the FMin operand leaves the FMin itself untouched. The
    // candidate count does go to zero, so absence alone would be satisfied --
    // only the positive proof catches it.
    std::string wrong_target = ir;
    const std::string load = "@dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)";
    const std::size_t at = wrong_target.find(load);
    CHECK(at != std::string::npos);
    wrong_target.replace(at, load.size(),
        "@dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40) trailing");
    const wuwa_tfr::FadePrimitiveDiagnostic damaged =
        wuwa_tfr::AnalyzeFadePrimitiveV1(wrong_target);
    CHECK(damaged.instances.size() == 1);
    const auto damaged_analysis =
        wuwa_tfr::AnalyzePreFadeFMinForInstance(wrong_target, damaged.instances.front());
    CHECK(wuwa_tfr::PreFadeFMinProvesNoQualifyingCandidate(damaged_analysis));
    error.clear();
    CHECK(!wuwa_tfr::VerifyPreFadeFMinOperandOneRewritten(
        wrong_target, damaged.instances.front(), matched, error));
    CHECK(!error.empty());
  }

  return 0;
}

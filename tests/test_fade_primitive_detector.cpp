// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"

#include "test_check.hpp"
#include <string>
#include <string_view>

namespace {

// Inserts a same-row adjacent, direct-scalar pre-Fade FMin defining
// %coverage, right before its first use -- the canonical shape
// target_dither_bypass.cpp's Production patch requires. Applied only to the
// specific fixtures that call PatchAllVerifiedFadePrimitiveInstancesPreFade-
// Operand() and expect success; every detector-only test in this file (and
// every test whose fixture PatchAllVerified... is expected to reject for an
// unrelated reason) is untouched, since inserting this earlier in the
// shared PrimitiveFunction() template collides with other tests' find/rfind
// searches for "@dx.op.binary.f32" (the FMax/FMin comment_fmax/comment_fmin
// coverage-expression rejection tests).
std::string WithQualifyingPreFadeFMin(std::string ir) {
  const std::string marker = "%twice = fmul fast float %coverage, 2.000000e+00";
  const std::size_t at = ir.find(marker);
  if (at == std::string::npos) return ir;
  ir.insert(at,
      "%cv_pf = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb2, i32 40)\n"
      "%cv_a = extractvalue %dx.types.CBufRet.f32 %cv_pf, 2\n"
      "%cv_b = extractvalue %dx.types.CBufRet.f32 %cv_pf, 3\n"
      "%coverage = call float @dx.op.binary.f32(i32 36, float %cv_a, float %cv_b)  ; FMin(a,b)\n");
  return ir;
}

std::string PrimitiveFunction(std::string_view name, std::string_view consumer,
    std::string_view prefix = {}, std::string_view suffix = {}) {
  return "define void @" + std::string(name) + "() {\nentry:\n" +
      std::string(prefix) + R"(
%posx = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%posy = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%gate_load = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle %cb, i32 7)
%gate_value = extractvalue %dx.types.CBufRet.f32 %gate_load, 1
%gate = fcmp fast ogt float %gate_value, 0.000000e+00
br i1 %gate, label %enabled, label %merge
enabled:
%ix = fptosi float %posx to i32
%iy = fptosi float %posy to i32
%mx = srem i32 %ix, 3
%my = srem i32 %iy, 3
%row = mul nsw i32 %mx, 3
%index = add nsw i32 %row, %my
%ptr = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index
%threshold = load float, float* %ptr, align 4, !tbaa !50, !noalias !54
%twice = fmul fast float %coverage, 2.000000e+00
%sub = fsub fast float %twice, %threshold
%lo = call float @dx.op.binary.f32(i32 35, float %sub, float 0.000000e+00)  ; FMax(a,b)
%hi = call float @dx.op.binary.f32(i32 36, float %lo, float 1.000000e+00)  ; FMin(a,b)
%computed = fadd fast float %hi, 0x3FD50F9F00000000
br label %merge
merge:
%dither = phi float [ %computed, %enabled ], [ 1.000000e+00, %entry ]
)" + std::string(consumer) + std::string(suffix) + "\n}\n";
}

std::string Module(std::string_view functions) {
  return "!899 = !{i32 0, !\"SV_Position\", i8 9}\n"
      "@thresholds = internal constant [9 x float] zeroinitializer\n" +
      std::string(functions);
}

std::string DiscardConsumer() {
  return "%score = fsub fast float %dither, 0.500000e+00\n"
      "%kill = fcmp fast olt float %score, 0.000000e+00\n"
      "call void @dx.op.discard(i32 82, i1 %kill)";
}

std::string SvTargetRgbConsumer() {
  return "%red = fmul fast float %dither, %color0\n"
      "%green = fmul fast float %dither, %color1\n"
      "%blue = fmul fast float %dither, %color2\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 0, float %red)\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 1, float %green)\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 2, float %blue)";
}

std::string SvTargetAlphaConsumer(std::uint32_t signature = 3,
    std::uint32_t row = 0, std::uint32_t column = 3,
    std::string_view value = "%alpha", std::string_view trailing = {}) {
  return "%alpha = fmul fast float %dither, %opacity\n"
      "call void @dx.op.storeOutput.f32(i32 5, i32 " +
      std::to_string(signature) + ", i32 " + std::to_string(row) +
      ", i8 " + std::to_string(column) + ", float " +
      std::string(value) + ")" + std::string(trailing);
}

std::string SvTargetMetadata(std::uint32_t signature = 3,
    std::uint32_t metadata = 900) {
  return "!" + std::to_string(metadata) + " = !{i32 " +
      std::to_string(signature) +
      ", !\"SV_Target\", i8 9}\n";
}

} // namespace

int main() {
  const std::string positive = Module(PrimitiveFunction("positive", DiscardConsumer()));
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(positive);
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::Discard);
    CHECK(result.instances.front().function_identity == "@positive");
    CHECK(result.instances.front().phi_start < result.instances.front().phi_end);
  }
  const auto expect_unpatchable_consumer = [](const std::string& llvm_ir) {
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(llvm_ir);
    CHECK(diagnostic.instances.size() == 1);
    const auto consumer = diagnostic.instances.front().consumer;
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::Discard);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::SvTargetRgb);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::DiscardAndSvTargetAlpha);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        llvm_ir).success);
  };
  const auto expect_no_candidate = [](const std::string& llvm_ir) {
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(llvm_ir).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        llvm_ir).success);
  };
  {
    // Every consumer path is built from comment-free code. Comments cannot
    // invent discard, SSA dependencies, output identity, FMax, FMin, or an
    // input signature that would make a Production rewrite authorized.
    const std::string comment_discard = Module(PrimitiveFunction(
        "comment_discard", "; call void @dx.op.discard(i32 82, i1 %dither)"));
    const auto comment_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(comment_discard);
    CHECK(comment_diagnostic.instances.size() == 1);
    CHECK(comment_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::Unknown);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        comment_discard).success);

    std::string wrong_opcode = Module(PrimitiveFunction(
        "wrong_discard_opcode", DiscardConsumer()));
    wrong_opcode.replace(wrong_opcode.find("i32 82, i1 %kill"),
        std::string("i32 82").size(), "i32 81");
    expect_unpatchable_consumer(wrong_opcode);

    std::string predicate_prefix = Module(PrimitiveFunction(
        "discard_predicate_prefix", DiscardConsumer()));
    predicate_prefix.replace(predicate_prefix.find("i1 %kill)"),
        std::string("i1 %kill)").size(), "i1 %kill2)  ; %kill");
    expect_unpatchable_consumer(predicate_prefix);

    std::string malformed_discard = Module(PrimitiveFunction(
        "malformed_discard", DiscardConsumer()));
    malformed_discard.erase(malformed_discard.find("%kill)"),
        std::string("%kill)").size());
    expect_unpatchable_consumer(malformed_discard);

    std::string comment_dependency = positive;
    const std::string expected_load =
        "%threshold = load float, float* %ptr, align 4, !tbaa !50, !noalias !54";
    comment_dependency.replace(comment_dependency.find(expected_load),
        expected_load.size(),
        "%threshold = load float, float* %ptr2, align 4, "
        "!tbaa !50, !noalias !54  ; %ptr");
    expect_no_candidate(comment_dependency);

    std::string comment_fmax = positive;
    comment_fmax.replace(comment_fmax.find("@dx.op.binary.f32"),
        std::string("@dx.op.binary.f32").size(), "@not_a_binary");
    expect_no_candidate(comment_fmax);

    std::string comment_fmin = positive;
    comment_fmin.replace(comment_fmin.rfind("@dx.op.binary.f32"),
        std::string("@dx.op.binary.f32").size(), "@not_a_binary");
    expect_no_candidate(comment_fmin);

    const std::string comment_output = Module(PrimitiveFunction("comment_output",
        "%alpha = fmul fast float %dither, %opacity\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, "
        "float %alpha2)  ; float %dither")) + SvTargetMetadata();
    const auto output_diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(comment_output);
    CHECK(output_diagnostic.instances.size() == 1);
    CHECK(output_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::Unknown);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        comment_output).success);

    const std::string comment_position =
        "; !899 = !{i32 0, !\"SV_Position\", i8 9}\n"
        "@thresholds = internal constant [9 x float] zeroinitializer\n" +
        PrimitiveFunction("comment_position", DiscardConsumer());
    expect_no_candidate(comment_position);
  }
  {
    // The two index inputs must resolve to the exact signature metadata that
    // names SV_Position. Merely having an unrelated SV_Position definition
    // elsewhere in the module is insufficient.
    const auto replace_all = [](std::string& text, std::string_view from,
                                 std::string_view to) {
      for (std::size_t offset = 0;
           (offset = text.find(from, offset)) != std::string::npos;
           offset += to.size())
        text.replace(offset, from.size(), to);
    };

    std::string texcoord_inputs = positive +
        "!898 = !{i32 1, !\"TEXCOORD\", i8 9}\n";
    replace_all(texcoord_inputs,
        "@dx.op.loadInput.f32(i32 4, i32 0, i32 0",
        "@dx.op.loadInput.f32(i32 4, i32 1, i32 0");
    expect_no_candidate(texcoord_inputs);

    std::string mixed_signatures = positive +
        "!898 = !{i32 1, !\"TEXCOORD\", i8 9}\n";
    const std::size_t second_input = mixed_signatures.find(
        "@dx.op.loadInput.f32(i32 4, i32 0, i32 0", mixed_signatures.find(
            "@dx.op.loadInput.f32") + 1);
    CHECK(second_input != std::string::npos);
    mixed_signatures.replace(second_input,
        std::string("@dx.op.loadInput.f32(i32 4, i32 0, i32 0").size(),
        "@dx.op.loadInput.f32(i32 4, i32 1, i32 0");
    expect_no_candidate(mixed_signatures);

    std::string duplicate_column = positive;
    const std::size_t column = duplicate_column.find("i8 1, i32 undef");
    CHECK(column != std::string::npos);
    duplicate_column.replace(column, std::string("i8 1").size(), "i8 0");
    expect_no_candidate(duplicate_column);

    std::string wrong_row = positive;
    const std::string row_input =
        "@dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1";
    const std::size_t row = wrong_row.find(row_input);
    CHECK(row != std::string::npos);
    wrong_row.replace(row, row_input.size(),
        "@dx.op.loadInput.f32(i32 4, i32 0, i32 1, i8 1");
    expect_no_candidate(wrong_row);

    std::string malformed_call = positive;
    const std::size_t final_operand = malformed_call.find("i32 undef)");
    CHECK(final_operand != std::string::npos);
    malformed_call.replace(final_operand, std::string("i32 undef)").size(),
        "i32 undef, i32 0)");
    expect_no_candidate(malformed_call);

    std::string wrong_load_opcode = positive;
    const std::size_t load_opcode = wrong_load_opcode.find(
        "@dx.op.loadInput.f32(i32 4");
    CHECK(load_opcode != std::string::npos);
    wrong_load_opcode.replace(load_opcode,
        std::string("@dx.op.loadInput.f32(i32 4").size(),
        "@dx.op.loadInput.f32(i32 5");
    expect_no_candidate(wrong_load_opcode);

    std::string with_load_metadata = positive;
    replace_all(with_load_metadata, "i32 undef)",
        "i32 undef), !dbg !42");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(
        with_load_metadata).instances.size() == 1);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(with_load_metadata)).success);

    std::string malformed_load_metadata = positive;
    const std::size_t load_end = malformed_load_metadata.find("i32 undef)");
    CHECK(load_end != std::string::npos);
    malformed_load_metadata.replace(load_end,
        std::string("i32 undef)").size(), "i32 undef), !dbg");
    expect_no_candidate(malformed_load_metadata);

    expect_no_candidate(positive +
        "!898 = !{i32 0, !\"SV_Position\", i8 9}\n");

    std::string dangling_position_reference = positive;
    const std::size_t position_end = dangling_position_reference.find(
        "!\"SV_Position\", i8 9}");
    CHECK(position_end != std::string::npos);
    dangling_position_reference.replace(position_end,
        std::string("!\"SV_Position\", i8 9}").size(),
        "!\"SV_Position\", i8 9, !777}");
    expect_no_candidate(dangling_position_reference);

    std::string malformed_nested_position = positive;
    const std::size_t nested_position_end = malformed_nested_position.find(
        "!\"SV_Position\", i8 9}");
    CHECK(nested_position_end != std::string::npos);
    malformed_nested_position.replace(nested_position_end,
        std::string("!\"SV_Position\", i8 9}").size(),
        "!\"SV_Position\", i8 9, !{garbage}}");
    expect_no_candidate(malformed_nested_position);
  }
  {
    // DXC emits control-flow hints as instruction metadata after the two
    // branch labels. Metadata and comments must not change successor identity.
    std::string with_branch_metadata = positive;
    const std::string branch =
        "br i1 %gate, label %enabled, label %merge";
    const std::size_t branch_offset = with_branch_metadata.find(branch);
    CHECK(branch_offset != std::string::npos);
    with_branch_metadata.replace(branch_offset, branch.size(),
        branch + ", !dx.controlflow.hints !42, !prof !43  ; hinted gate");
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(with_branch_metadata);
    CHECK(result.instances.size() == 1);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(with_branch_metadata)).success);
  }
  {
    // Extra operands and malformed metadata remain fail-closed.
    const std::string branch =
        "br i1 %gate, label %enabled, label %merge";
    for (const std::string_view suffix : {
             ", label %unexpected",
             ", !dx.controlflow.hints",
             ", !dx.controlflow.hints !not_a_reference"}) {
      std::string malformed = positive;
      const std::size_t branch_offset = malformed.find(branch);
      CHECK(branch_offset != std::string::npos);
      malformed.replace(branch_offset, branch.size(), branch + std::string(suffix));
      CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(malformed).instances.empty());
      CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          malformed).success);
    }
  }
  const auto expect_alpha_rejected = [](const std::string& llvm_ir) {
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(llvm_ir);
    CHECK(diagnostic.instances.size() == 1);
    const auto consumer = diagnostic.instances.front().consumer;
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::Discard);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::SvTargetRgb);
    CHECK(consumer != wuwa_tfr::FadePrimitiveConsumer::DiscardAndSvTargetAlpha);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        llvm_ir).success);
  };
  {
    const std::string alpha = Module(PrimitiveFunction("alpha",
        SvTargetAlphaConsumer())) + SvTargetMetadata();
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(alpha);
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(alpha)).success);
  }
  {
    const std::string both = Module(PrimitiveFunction("both",
        SvTargetAlphaConsumer() + "\n" + DiscardConsumer())) +
        SvTargetMetadata();
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(both);
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::DiscardAndSvTargetAlpha);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(both)).success);
  }
  {
    // Authorized output/discard evidence never hides an additional reachable
    // terminal or side-effecting use of the same verified fade value.
    const auto alpha_with_extra = [](std::string_view name,
                                      std::string_view extra) {
      return Module(PrimitiveFunction(name,
          SvTargetAlphaConsumer() + "\n" + std::string(extra))) +
          SvTargetMetadata();
    };
    expect_unpatchable_consumer(alpha_with_extra("unknown_terminal_call",
        "call void @sink(float %dither)"));
    expect_unpatchable_consumer(alpha_with_extra("side_effecting_store",
        "store float %dither, float* %side_effect_ptr"));
    expect_unpatchable_consumer(alpha_with_extra("branch_predicate",
        "%branch_pred = fcmp fast olt float %dither, 0.000000e+00\n"
        "br i1 %branch_pred, label %visible, label %hidden"));
    expect_unpatchable_consumer(alpha_with_extra("side_effecting_value_call",
        "%observed = call float @side_effect(float %dither)"));
  }
  {
    // Structural alpha authorization never accepts textual component or
    // signature hints. Every negative case is rejected by both the detector
    // classification and the Production patcher.
    const auto alpha_ir = [](std::string_view name, std::string_view consumer,
                             std::string_view metadata =
                                 "!900 = !{i32 3, !\"SV_Target\", i8 9}\n") {
      return Module(PrimitiveFunction(name, consumer)) + std::string(metadata);
    };
    expect_alpha_rejected(alpha_ir("comment_component",
        SvTargetAlphaConsumer(3, 0, 2, "%alpha", "  ; i8 3 SV_Target")));
    expect_alpha_rejected(alpha_ir("prefix_component",
        SvTargetAlphaConsumer(3, 0, 30)));
    expect_alpha_rejected(alpha_ir("wrong_row", SvTargetAlphaConsumer(3, 1)));
    expect_alpha_rejected(alpha_ir("wrong_column", SvTargetAlphaConsumer(3, 0, 2)));
    expect_alpha_rejected(alpha_ir("wrong_signature", SvTargetAlphaConsumer(4)));
    expect_alpha_rejected(alpha_ir("missing_metadata", SvTargetAlphaConsumer(), ""));
    expect_alpha_rejected(alpha_ir("malformed_metadata", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\"}\n"));
    expect_alpha_rejected(alpha_ir("unterminated_metadata", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", i8 9\n"));
    expect_alpha_rejected(alpha_ir("metadata_trailing_garbage", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", i8 9} trailing\n"));
    expect_alpha_rejected(alpha_ir("metadata_invalid_field", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", garbage}\n"));
    expect_alpha_rejected(alpha_ir("metadata_invalid_reference", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", i8 9, !777}\n"));
    expect_alpha_rejected(alpha_ir("metadata_invalid_nested_value", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", i8 9, !{garbage}}\n"));
    expect_alpha_rejected(alpha_ir("duplicate_metadata", SvTargetAlphaConsumer(),
        "!900 = !{i32 3, !\"SV_Target\", i8 9}\n"
        "!901 = !{i32 3, !\"SV_Target\", i8 9}\n"));
    expect_alpha_rejected(alpha_ir("malformed_output",
        "%alpha = fmul fast float %dither, %opacity\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, float %alpha, i32 0)"));
    expect_alpha_rejected(alpha_ir("duplicate_alpha",
        SvTargetAlphaConsumer() + "\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, float %alpha)"));

    // A value whose spelling has the tracked name as a prefix is not the
    // tracked value. The comment makes the outer traversal visit the current
    // value, while ParseStoreOutputF32 still demands exact parsed equality.
    expect_alpha_rejected(alpha_ir("named_prefix",
        "%alpha = fmul fast float %dither, %opacity\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, float %dither2)  ; %dither"));
    std::string numeric_prefix = alpha_ir("numeric_prefix",
        "%alpha = fmul fast float %dither, %opacity\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, float %190)  ; %19");
    for (std::size_t offset = 0;
         (offset = numeric_prefix.find("%dither", offset)) != std::string::npos;
         offset += std::string("%19").size())
      numeric_prefix.replace(offset, std::string("%dither").size(), "%19");
    expect_alpha_rejected(numeric_prefix);

    // An output in another function cannot authorize this function's phi,
    // even with valid module-level metadata.
    const std::string cross_function = Module(
        PrimitiveFunction("current", "") +
        "define void @other() {\nentry:\n" +
        SvTargetAlphaConsumer() + "\n}\n") + SvTargetMetadata();
    const auto cross_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(cross_function);
    CHECK(cross_diagnostic.instances.size() == 1);
    CHECK(cross_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::Unknown);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        cross_function).success);
  }
  {
    const std::string rgb = Module(PrimitiveFunction(
        "target_rgb", SvTargetRgbConsumer())) + SvTargetMetadata();
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(rgb);
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::SvTargetRgb);
    const auto patched =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
            WithQualifyingPreFadeFMin(rgb));
    CHECK(patched.success);
    CHECK(patched.verified_instance_count == 1);
    CHECK(patched.patched_instance_count == 1);

    // Production authorizes only an exact row-zero RGB triplet on one
    // signature whose DXIL metadata names it SV_Target.
    const std::string blue_store =
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 2, float %blue)";
    std::string missing_blue = rgb;
    missing_blue.erase(missing_blue.find(blue_store), blue_store.size());
    const auto missing_blue_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(missing_blue);
    CHECK(missing_blue_diagnostic.instances.size() == 1);
    CHECK(missing_blue_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        missing_blue).success);

    std::string duplicate_column = rgb;
    duplicate_column.replace(duplicate_column.find("i8 2, float %blue"),
        std::string("i8 2").size(), "i8 1");
    const auto duplicate_column_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(duplicate_column);
    CHECK(duplicate_column_diagnostic.instances.size() == 1);
    CHECK(duplicate_column_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);

    std::string wrong_row = rgb;
    wrong_row.replace(wrong_row.find("i32 0, i8 2, float %blue"),
        std::string("i32 0").size(), "i32 1");
    const auto wrong_row_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(wrong_row);
    CHECK(wrong_row_diagnostic.instances.size() == 1);
    CHECK(wrong_row_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);

    const std::string without_metadata = Module(PrimitiveFunction(
        "target_rgb_without_metadata", SvTargetRgbConsumer()));
    const auto without_metadata_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(without_metadata);
    CHECK(without_metadata_diagnostic.instances.size() == 1);
    CHECK(without_metadata_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);

    std::string mixed_signature = rgb + SvTargetMetadata(4, 901);
    mixed_signature.replace(mixed_signature.find(
        "i32 3, i32 0, i8 2, float %blue"), std::string("i32 3").size(),
        "i32 4");
    const auto mixed_signature_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(mixed_signature);
    CHECK(mixed_signature_diagnostic.instances.size() == 1);
    CHECK(mixed_signature_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);

    const std::string duplicate_signature_metadata =
        rgb + SvTargetMetadata(3, 901);
    const auto duplicate_signature_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(duplicate_signature_metadata);
    CHECK(duplicate_signature_diagnostic.instances.size() == 1);
    CHECK(duplicate_signature_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);

    const std::string signature_prefix = Module(PrimitiveFunction(
        "target_rgb_signature_prefix", SvTargetRgbConsumer())) +
        SvTargetMetadata(30);
    const auto signature_prefix_diagnostic =
        wuwa_tfr::AnalyzeFadePrimitiveV1(signature_prefix);
    CHECK(signature_prefix_diagnostic.instances.size() == 1);
    CHECK(signature_prefix_diagnostic.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
  }
  {
    // Same local SSA names in another function must not complete a candidate.
    const std::string threshold_only = R"(define void @threshold_only() {
entry:
%posx = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 0, i32 undef)
%posy = call float @dx.op.loadInput.f32(i32 4, i32 0, i32 0, i8 1, i32 undef)
%ix = fptosi float %posx to i32
%iy = fptosi float %posy to i32
%mx = srem i32 %ix, 3
%my = srem i32 %iy, 3
%row = mul nsw i32 %mx, 3
%index = add nsw i32 %row, %my
%ptr = getelementptr inbounds [9 x float], [9 x float]* @thresholds, i32 0, i32 %index
})";
    const std::string phi_only = R"(define void @phi_only() {
entry:
%dither = phi float [ %computed, %enabled ], [ 1.000000e+00, %entry ]
})";
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(Module(threshold_only + phi_only)).instances.empty());
  }
  {
    // A consumer in another function is not a consumer of this function's phi.
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(Module(
        PrimitiveFunction("candidate", "") +
        "define void @other() {\nentry:\n" + DiscardConsumer() + "\n}\n"));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::Unknown);
  }
  {
    // A cbuffer gate in function A cannot authorize function B's coverage and
    // phi, even when all local SSA spellings are deliberately identical.
    std::string gate_only = PrimitiveFunction("gate_only", "");
    const std::size_t access = gate_only.find(
        "getelementptr inbounds [9 x float], [9 x float]*");
    gate_only.replace(access,
        std::string("getelementptr inbounds [9 x float], [9 x float]*").size(),
        "getelementptr inbounds [8 x float], [8 x float]*");
    std::string coverage_and_phi = PrimitiveFunction("coverage_and_phi", DiscardConsumer());
    const std::size_t gate = coverage_and_phi.find("@dx.op.cbufferLoadLegacy");
    coverage_and_phi.replace(gate, std::string("@dx.op.cbufferLoadLegacy").size(),
        "@not_a_cbuffer_load");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(
        Module(gate_only + coverage_and_phi)).instances.empty());
  }
  {
    // A predecessor name is an SSA token, not a string prefix: the cbuffer
    // branch targets %enabled2 while only an unrelated condition reaches the
    // actual %enabled predecessor of the otherwise valid phi.
    std::string prefix_successor = Module(PrimitiveFunction(
        "prefix_successor", DiscardConsumer()));
    const std::string original =
        "br i1 %gate, label %enabled, label %merge";
    const std::size_t branch = prefix_successor.find(original);
    prefix_successor.replace(branch, original.size(),
        "br i1 %gate, label %enabled2, label %other\n"
        "enabled2:\n"
        "br label %merge\n"
        "other:\n"
        "%plain = icmp eq i32 0, 0\n"
        "br i1 %plain, label %enabled, label %merge");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(prefix_successor).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        prefix_successor).success);
  }
  {
    auto near_miss = positive;
    const std::size_t type = near_miss.find("[9 x float]");
    near_miss.replace(type, std::string("[9 x float]").size(), "[8 x float]");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(near_miss).instances.empty());
  }
  {
    // The expected GEP value remains in the slice through %ptr2, but the only
    // float threshold-like load reads %ptr2 rather than the verified %ptr.
    // Prefix spelling must not make that a threshold load.
    std::string pointer_prefix = positive;
    const std::string load =
        "%threshold = load float, float* %ptr, align 4, !tbaa !50, !noalias !54";
    const std::size_t load_offset = pointer_prefix.find(load);
    pointer_prefix.replace(load_offset, load.size(),
        "%ptr2 = bitcast float* %ptr to float*\n"
        "%threshold = load float, float* %ptr2, align 4, !tbaa !50, !noalias !54");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(pointer_prefix).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        pointer_prefix).success);
  }
  {
    // Numeric LLVM names obey the same exact-token rule: %19 is not %190.
    std::string numeric_prefix = positive;
    const std::string gep = "%ptr = getelementptr";
    const std::size_t gep_offset = numeric_prefix.find(gep);
    numeric_prefix.replace(gep_offset, gep.size(), "%19 = getelementptr");
    const std::string load =
        "%threshold = load float, float* %ptr, align 4, !tbaa !50, !noalias !54";
    const std::size_t load_offset = numeric_prefix.find(load);
    numeric_prefix.replace(load_offset, load.size(),
        "%190 = bitcast float* %19 to float*\n"
        "%threshold = load float, float* %190, align 4, !tbaa !50, !noalias !54");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(numeric_prefix).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        numeric_prefix).success);
  }
  {
    auto malformed = positive;
    malformed.erase(malformed.rfind('}'));
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(malformed).instances.empty());
  }
  {
    auto duplicate_definition = positive;
    const std::size_t before_phi = duplicate_definition.find("%dither = phi");
    duplicate_definition.insert(before_phi,
        "%computed = fadd fast float %hi, 0.000000e+00\n");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(duplicate_definition).instances.empty());
  }
  {
    // Reaching the bounded backward traversal with pending dependencies is an
    // incomplete proof, not a successful match.
    std::string limited = positive;
    std::string chain;
    for (int index = 0; index < 2048; ++index) {
      const std::string value = "%chain" + std::to_string(index);
      const std::string input = index == 0 ? "%computed" :
          "%chain" + std::to_string(index - 1);
      chain += value + " = fadd fast float " + input + ", 0.000000e+00\n";
    }
    const std::size_t branch = limited.find("br label %merge");
    limited.insert(branch, chain);
    const std::string original = "%dither = phi float [ %computed, %enabled ]";
    const std::size_t phi = limited.find(original);
    limited.replace(phi, original.size(),
        "%dither = phi float [ %chain2047, %enabled ]");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(limited).instances.empty());
  }
  {
    // Consumer classification must not accept a graph truncated at 4096
    // nodes. The discard remains pending after the bounded traversal.
    std::string limited = Module(PrimitiveFunction("consumer_limit", ""));
    std::string chain;
    for (int index = 0; index < 4096; ++index) {
      const std::string value = "%consumer" + std::to_string(index);
      const std::string input = index == 0 ? "%dither" :
          "%consumer" + std::to_string(index - 1);
      chain += value + " = fadd fast float " + input + ", 0.000000e+00\n";
    }
    chain += "%score = fsub fast float %consumer4095, 0.500000e+00\n"
        "%kill = fcmp fast olt float %score, 0.000000e+00\n"
        "call void @dx.op.discard(i32 82, i1 %kill)\n";
    limited.insert(limited.rfind("\n}\n"), chain);
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(limited).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        limited).success);
  }
  {
    // Cause 1: real DXC threshold loads carry TBAA/noalias metadata after the
    // alignment suffix (or in place of it). That must be accepted; any other
    // trailing or duplicated operand syntax must remain fail-closed.
    const std::string load_line =
        "%threshold = load float, float* %ptr, align 4, !tbaa !50, !noalias !54";
    CHECK(positive.find(load_line) != std::string::npos);

    std::string no_align = positive;
    no_align.replace(no_align.find(load_line), load_line.size(),
        "%threshold = load float, float* %ptr, !tbaa !50, !noalias !54");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(no_align).instances.size() == 1);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(no_align)).success);

    std::string single_attachment = positive;
    single_attachment.replace(single_attachment.find(load_line), load_line.size(),
        "%threshold = load float, float* %ptr, align 4, !tbaa !50");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(single_attachment).instances.size() == 1);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(single_attachment)).success);

    const auto expect_load_rejected = [&](std::string_view replacement) {
      std::string ir = positive;
      ir.replace(ir.find(load_line), load_line.size(), std::string(replacement));
      CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(ir).instances.empty());
      CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          ir).success);
    };
    // Malformed attachment name: missing the leading '!'.
    expect_load_rejected(
        "%threshold = load float, float* %ptr, align 4, tbaa !50");
    // Malformed attachment reference: missing the '!id'.
    expect_load_rejected(
        "%threshold = load float, float* %ptr, align 4, !tbaa");
    // Arbitrary trailing syntax after an otherwise well-formed attachment.
    expect_load_rejected(
        "%threshold = load float, float* %ptr, align 4, !tbaa !50 extra");
    // An extra value operand disguised as a second pointer clause.
    expect_load_rejected(
        "%threshold = load float, float* %ptr, float* %ptr, align 4, !tbaa !50");
    // The load reads a different pointer entirely; metadata is irrelevant.
    expect_load_rejected(
        "%threshold = load float, float* %other, align 4, !tbaa !50, !noalias !54");
    // An unmatched alignment token must not be silently dropped in favor of
    // reinterpreting it as a metadata attachment.
    expect_load_rejected(
        "%threshold = load float, float* %ptr, align, !tbaa !50");
  }
  {
    // Cause 2: exactly two pure DXIL intrinsics -- FMin (dx.op.binary.f32
    // opcode 36) and Saturate (dx.op.unary.f32 opcode 7) -- may sit between
    // the verified fade primitive and its final consumer. Every other
    // dx.op.binary/unary opcode, wrong callee, wrong return type, wrong
    // arity, malformed operand, or trailing syntax must remain fail-closed.
    const std::string discard_via_fmin = Module(PrimitiveFunction(
        "discard_via_fmin",
        "%clamped = call float @dx.op.binary.f32(i32 36, float %dither, "
        "float 1.000000e+00)  ; FMin(a,b)\n"
        "%score = fsub fast float %clamped, 0.500000e+00\n"
        "%kill = fcmp fast olt float %score, 0.000000e+00\n"
        "call void @dx.op.discard(i32 82, i1 %kill)"));
    const auto fmin_result = wuwa_tfr::AnalyzeFadePrimitiveV1(discard_via_fmin);
    CHECK(fmin_result.instances.size() == 1);
    CHECK(fmin_result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::Discard);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(discard_via_fmin)).success);

    const std::string alpha_via_saturate = Module(PrimitiveFunction(
        "alpha_via_saturate",
        "%saturated = call float @dx.op.unary.f32(i32 7, float %dither)  "
        "; Saturate(value)\n"
        "%alpha = fmul fast float %saturated, %opacity\n"
        "call void @dx.op.storeOutput.f32(i32 5, i32 3, i32 0, i8 3, "
        "float %alpha)")) + SvTargetMetadata();
    const auto saturate_result =
        wuwa_tfr::AnalyzeFadePrimitiveV1(alpha_via_saturate);
    CHECK(saturate_result.instances.size() == 1);
    CHECK(saturate_result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
        WithQualifyingPreFadeFMin(alpha_via_saturate)).success);

    const auto expect_pure_call_rejected = [](std::string_view name,
                                               std::string_view call_line) {
      const std::string ir = Module(PrimitiveFunction(name,
          std::string(call_line) +
          "\n%score = fsub fast float %clamped, 0.500000e+00\n"
          "%kill = fcmp fast olt float %score, 0.000000e+00\n"
          "call void @dx.op.discard(i32 82, i1 %kill)"));
      const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
      CHECK(diagnostic.instances.size() == 1);
      CHECK(diagnostic.instances.front().consumer ==
          wuwa_tfr::FadePrimitiveConsumer::OtherVisibilityOrOutput);
      CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
          ir).success);
    };
    // Wrong opcode: FMax (35) instead of FMin (36).
    expect_pure_call_rejected("wrong_binary_opcode",
        "%clamped = call float @dx.op.binary.f32(i32 35, float %dither, "
        "float 1.000000e+00)");
    // Wrong opcode: Sqrt (24) instead of Saturate (7).
    expect_pure_call_rejected("wrong_unary_opcode",
        "%clamped = call float @dx.op.unary.f32(i32 24, float %dither)");
    // Wrong callee.
    expect_pure_call_rejected("wrong_callee",
        "%clamped = call float @dx.op.not_binary.f32(i32 36, float %dither, "
        "float 1.000000e+00)");
    // Wrong return type.
    expect_pure_call_rejected("wrong_return_type",
        "%clamped = call double @dx.op.binary.f32(i32 36, float %dither, "
        "float 1.000000e+00)");
    // Wrong arity: the second binary operand is missing entirely.
    expect_pure_call_rejected("wrong_arity",
        "%clamped = call float @dx.op.binary.f32(i32 36, float %dither)");
    // Malformed typed operand: the 'float' type tag is missing.
    expect_pure_call_rejected("malformed_operand_type",
        "%clamped = call float @dx.op.binary.f32(i32 36, %dither, "
        "float 1.000000e+00)");
    // Trailing junk after an otherwise well-formed call.
    expect_pure_call_rejected("trailing_junk",
        "%clamped = call float @dx.op.unary.f32(i32 7, float %dither) garbage");
    // An unrelated float-returning call is not a generic "all calls are
    // pure" pass; it remains ambiguous, side-effect-unknown evidence.
    expect_pure_call_rejected("unrelated_call",
        "%clamped = call float @sink(float %dither)");
  }
  return 0;
}

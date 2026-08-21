// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"

#include "test_check.hpp"
#include <string>
#include <string_view>

namespace {

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
%threshold = load float, float* %ptr, align 4
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
  return "; SV_Position              0   xyzw\n"
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
    CHECK(wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
        with_branch_metadata).success);
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
      CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
          malformed).success);
    }
  }
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(Module(
        PrimitiveFunction("alpha", "; SV_Target\n%alpha = fmul fast float %dither, %opacity\n"
            "call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)")));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::SvTargetAlpha);
  }
  {
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(Module(
        PrimitiveFunction("both", "; SV_Target\n%alpha = fmul fast float %dither, %opacity\n"
            "call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %alpha)\n" +
            DiscardConsumer())));
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer == wuwa_tfr::FadePrimitiveConsumer::DiscardAndSvTargetAlpha);
  }
  {
    const std::string rgb = Module(PrimitiveFunction(
        "target_rgb", SvTargetRgbConsumer())) + SvTargetMetadata();
    const auto result = wuwa_tfr::AnalyzeFadePrimitiveV1(rgb);
    CHECK(result.instances.size() == 1);
    CHECK(result.instances.front().consumer ==
        wuwa_tfr::FadePrimitiveConsumer::SvTargetRgb);
    const auto patched =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(rgb);
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
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
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
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
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
    const std::string load = "%threshold = load float, float* %ptr, align 4";
    const std::size_t load_offset = pointer_prefix.find(load);
    pointer_prefix.replace(load_offset, load.size(),
        "%ptr2 = bitcast float* %ptr to float*\n"
        "%threshold = load float, float* %ptr2, align 4");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(pointer_prefix).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
        pointer_prefix).success);
  }
  {
    // Numeric LLVM names obey the same exact-token rule: %19 is not %190.
    std::string numeric_prefix = positive;
    const std::string gep = "%ptr = getelementptr";
    const std::size_t gep_offset = numeric_prefix.find(gep);
    numeric_prefix.replace(gep_offset, gep.size(), "%19 = getelementptr");
    const std::string load = "%threshold = load float, float* %ptr, align 4";
    const std::size_t load_offset = numeric_prefix.find(load);
    numeric_prefix.replace(load_offset, load.size(),
        "%190 = bitcast float* %19 to float*\n"
        "%threshold = load float, float* %190, align 4");
    CHECK(wuwa_tfr::AnalyzeFadePrimitiveV1(numeric_prefix).instances.empty());
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
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
    CHECK(!wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
        limited).success);
  }
  return 0;
}

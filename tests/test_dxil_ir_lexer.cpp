// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dxil_ir_lexer.hpp"

#include "test_check.hpp"
#include <string>

using namespace wuwa_tfr::dxil_ir;

namespace {

void SsaAndTrim() {
  CHECK(IsSsaValue("%12"));
  CHECK(IsSsaValue("%foo.bar$1"));
  CHECK(!IsSsaValue("%"));
  CHECK(!IsSsaValue("12"));
  CHECK(Trim("  \t x  \r") == "x");
  CHECK(Trim("   ") == "");
}

void SplitCodeAndCommentTest() {
  const auto plain = SplitCodeAndComment("%a = add i32 %b, 1  ; note");
  CHECK(plain.code == "%a = add i32 %b, 1  ");
  CHECK(plain.comment == " note");

  const auto quoted = SplitCodeAndComment("!1 = !{!\"a;b\"}  ; real comment");
  CHECK(quoted.code == "!1 = !{!\"a;b\"}  ");
  CHECK(quoted.comment == " real comment");

  const auto none = SplitCodeAndComment("no comment here");
  CHECK(none.code == "no comment here");
  CHECK(none.comment.empty());
}

void SsaValuesTest() {
  const auto values = SsaValues("call void @f(%a, %b.1, i32 %c)");
  CHECK(values.size() == 3);
  CHECK(values[0] == "%a");
  CHECK(values[1] == "%b.1");
  CHECK(values[2] == "%c");
}

void FunctionIdentityTest() {
  CHECK(FunctionIdentity("define void @main() {") == "@main");
  CHECK(FunctionIdentity("define float @foo.bar(i32 %x) {") == "@foo.bar");
  CHECK(FunctionIdentity("no at sign here") == "");
}

void CallSuffixGrammar() {
  CHECK(IsWellFormedCallSuffix(""));
  CHECK(IsWellFormedCallSuffix("   "));
  CHECK(IsWellFormedCallSuffix("#3"));
  CHECK(IsWellFormedCallSuffix(" #12 "));
  CHECK(IsWellFormedCallSuffix(", !dbg !7"));
  CHECK(IsWellFormedCallSuffix(", !dbg !7, !tbaa !9"));
  CHECK(IsWellFormedCallSuffix("#3, !dbg !7"));
  CHECK(IsWellFormedCallSuffix(" #3 , !dbg !7 "));

  CHECK(!IsWellFormedCallSuffix("#"));
  CHECK(!IsWellFormedCallSuffix("#x"));
  CHECK(!IsWellFormedCallSuffix("garbage"));
  CHECK(!IsWellFormedCallSuffix(", garbage"));
  CHECK(!IsWellFormedCallSuffix(", !dbg"));
  CHECK(!IsWellFormedCallSuffix(", !dbg garbage"));
  CHECK(!IsWellFormedCallSuffix("!dbg !7"));

  CHECK(HasOnlyMetadataAttachments(""));
  CHECK(HasOnlyMetadataAttachments(", !dbg !7"));
  CHECK(!HasOnlyMetadataAttachments("#3"));
}

void CbufferLoadLegacyTest() {
  const std::string base =
      "call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, "
      "%dx.types.Handle %cb, i32 7)";
  for (const std::string suffix : {std::string(), std::string(" #2"),
      std::string(", !dbg !7"), std::string(" #2, !dbg !7")}) {
    const std::string call = base + suffix;
    CbufferLoadLegacyCall result;
    CHECK(ParseCbufferLoadLegacyCall(call, result));
    CHECK(result.handle == "%cb");
    CHECK(result.row_resolved && result.row == 7);
  }

  CbufferLoadLegacyCall dynamic;
  CHECK(ParseCbufferLoadLegacyCall(
      "call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, "
      "%dx.types.Handle %cb, i32 %idx)", dynamic));
  CHECK(dynamic.handle == "%cb");
  CHECK(!dynamic.row_resolved);

  CbufferLoadLegacyCall rejected;
  CHECK(!ParseCbufferLoadLegacyCall(base + " garbage", rejected));
  CHECK(!ParseCbufferLoadLegacyCall(
      "call %dx.types.CBufRet.f32 @dx.op.cbufferLoad.f32(i32 58, "
      "%dx.types.Handle %cb, i32 0, i32 4)", rejected));
}

void CbufferLoadByteTest() {
  const std::string base =
      "call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb, "
      "i32 648, i32 4)";
  for (const std::string suffix : {std::string(), std::string(" #1"),
      std::string(", !dbg !3"), std::string(" #1, !dbg !3")}) {
    const std::string call = base + suffix;
    CbufferLoadByteCall result;
    CHECK(ParseCbufferLoadByteCall(call, result));
    CHECK(result.handle == "%cb");
    CHECK(result.byte_offset_resolved && result.byte_offset == 648);
  }

  CbufferLoadByteCall bad_alignment;
  CHECK(!ParseCbufferLoadByteCall(
      "call float @dx.op.cbufferLoad.f32(i32 58, %dx.types.Handle %cb, "
      "i32 648, i32 %align)", bad_alignment));
}

void ExtractValueTest() {
  std::string_view aggregate;
  CHECK(ParseExtractValueAggregate(
      "extractvalue %dx.types.CBufRet.f32 %agg, 2", aggregate));
  CHECK(aggregate == "%agg");

  std::uint32_t component = 0;
  CHECK(ParseExtractValueComponent(
      "extractvalue %dx.types.CBufRet.f32 %agg, 2", component));
  CHECK(component == 2);
  CHECK(!ParseExtractValueComponent(
      "extractvalue %dx.types.CBufRet.f32 %agg, 4", component));
}

void CreateHandleTest() {
  CreateHandleCall full;
  CHECK(ParseCreateHandleCall(
      "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 3, "
      "i32 0, i1 false)", full));
  CHECK(full.resource_class_resolved && full.resource_class == 2);
  CHECK(full.range_id_resolved && full.range_id == 3);

  CreateHandleCall no_range;
  CHECK(ParseCreateHandleCall(
      "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 %n, "
      "i32 0, i1 false)", no_range));
  CHECK(no_range.resource_class_resolved);
  CHECK(!no_range.range_id_resolved);

  CreateHandleCall bad_class;
  CHECK(!ParseCreateHandleCall(
      "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 %rc, i32 3, "
      "i32 0, i1 false)", bad_class));
}

void FMinAndDxOpTest() {
  std::string_view a, b;
  CHECK(ParseFMinOperands(
      "call float @dx.op.binary.f32(i32 36, float %a, float %b)", a, b));
  CHECK(a == "%a" && b == "%b");
  CHECK(!ParseFMinOperands(
      "call float @dx.op.binary.f32(i32 35, float %a, float %b)", a, b));

  std::uint32_t opcode = 0;
  CHECK(ParseDxOpBinaryF32(
      "call float @dx.op.binary.f32(i32 36, float %a, float %b)", opcode));
  CHECK(opcode == 36);
  CHECK(ParseDxOpUnaryF32(
      "call float @dx.op.unary.f32(i32 7, float %a)", opcode));
  CHECK(opcode == 7);

  CHECK(!ParseDxOpBinaryF32(
      "call float @dx.op.binary.f32(i32 36, floaty %a, float %b)", opcode));
}

}  // namespace

int main() {
  SsaAndTrim();
  SplitCodeAndCommentTest();
  SsaValuesTest();
  FunctionIdentityTest();
  CallSuffixGrammar();
  CbufferLoadLegacyTest();
  CbufferLoadByteTest();
  ExtractValueTest();
  CreateHandleTest();
  FMinAndDxOpTest();
  return 0;
}

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "shader_preparation_outcome.hpp"

#include <initializer_list>

#include "test_check.hpp"

using wuwa_tfr::ClassifyShaderPreparation;
using wuwa_tfr::ShaderPreparationIsFailure;
using wuwa_tfr::ShaderPreparationOutcome;

namespace {

bool IsFailure(bool analysis_reached_verdict, bool matched,
    bool replacement_bytecode_produced) {
  return ShaderPreparationIsFailure(ClassifyShaderPreparation(
      analysis_reached_verdict, matched, replacement_bytecode_produced));
}

}  // namespace

int main() {
  // The case the counter used to be dominated by: the analysis ran, reached a
  // verdict, and this shader is simply not a transparency-filter target.
  // Every ordinary pixel shader in the game lands here, and none of them is a
  // failed replacement.
  {
    const auto outcome = ClassifyShaderPreparation(
        /*analysis_reached_verdict=*/true, /*matched=*/false,
        /*replacement_bytecode_produced=*/false);
    CHECK(outcome == ShaderPreparationOutcome::NotMatched);
    CHECK(!ShaderPreparationIsFailure(outcome));
  }

  // A matched shader that produced replacement bytecode is a success.
  {
    const auto outcome = ClassifyShaderPreparation(true, true, true);
    CHECK(outcome == ShaderPreparationOutcome::Prepared);
    CHECK(!ShaderPreparationIsFailure(outcome));
  }

  // A matched shader whose patch, assembly or validation failed produced no
  // bytecode: that is a genuine failure.
  {
    const auto outcome = ClassifyShaderPreparation(true, true, false);
    CHECK(outcome == ShaderPreparationOutcome::Failed);
    CHECK(ShaderPreparationIsFailure(outcome));
  }

  // Preparation that never reached a verdict -- no DXC runtime, refused
  // disassembly, an exception mid-analysis -- is a failure, not a silent
  // "not a target". Nothing proved this shader was uninteresting.
  {
    const auto outcome = ClassifyShaderPreparation(false, false, false);
    CHECK(outcome == ShaderPreparationOutcome::Failed);
    CHECK(ShaderPreparationIsFailure(outcome));
  }

  // Full truth table, so the one non-failure verdict is pinned exactly.
  CHECK(!IsFailure(true, false, false));   // not a target
  CHECK(!IsFailure(true, true, true));     // prepared
  CHECK(!IsFailure(true, false, true));    // bytecode wins over the verdict
  CHECK(!IsFailure(false, false, true));
  CHECK(!IsFailure(false, true, true));
  CHECK(IsFailure(true, true, false));     // matched, unpreparable
  CHECK(IsFailure(false, false, false));   // no verdict reached
  CHECK(IsFailure(false, true, false));

  // Exactly one of the eight input combinations that yields no bytecode is a
  // non-failure, and it is the not-a-target one.
  int non_failures_without_bytecode = 0;
  for (const bool verdict : {false, true}) {
    for (const bool matched : {false, true}) {
      if (!IsFailure(verdict, matched, false)) ++non_failures_without_bytecode;
    }
  }
  CHECK(non_failures_without_bytecode == 1);

  std::puts("test_shader_preparation_outcome: all tests passed");
  return 0;
}

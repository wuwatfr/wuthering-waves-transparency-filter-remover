// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

namespace wuwa_tfr {

enum class ShaderPreparationOutcome {
  NotMatched,
  Prepared,
  Failed,
};

constexpr ShaderPreparationOutcome ClassifyShaderPreparation(
    bool analysis_reached_verdict, bool matched,
    bool replacement_bytecode_produced) noexcept {
  if (replacement_bytecode_produced) return ShaderPreparationOutcome::Prepared;
  if (analysis_reached_verdict && !matched)
    return ShaderPreparationOutcome::NotMatched;
  return ShaderPreparationOutcome::Failed;
}

constexpr bool ShaderPreparationIsFailure(
    ShaderPreparationOutcome outcome) noexcept {
  return outcome == ShaderPreparationOutcome::Failed;
}

}  // namespace wuwa_tfr

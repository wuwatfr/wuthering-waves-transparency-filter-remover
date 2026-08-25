// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

namespace wuwa_tfr {

// How one shader's preparation ended. Telemetry classification only: nothing
// here participates in matching, patching, caching or the replacement
// lifecycle. It exists so an ordinary non-target pixel shader is not counted
// as a failed replacement.
enum class ShaderPreparationOutcome {
  // The structural analysis ran to a verdict and this shader carries no
  // verified Fade Primitive instance. The overwhelmingly common outcome for a
  // game's pixel shaders, and the expected one: nothing failed.
  NotMatched,
  // Replacement bytecode was produced.
  Prepared,
  // Either a matched shader could not be patched, assembled or validated, or
  // preparation never reached a verdict at all -- no DXC runtime, refused
  // disassembly, an exception. Both mean WuwaTFR set out to do work and could
  // not, which is what a failure count should surface.
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

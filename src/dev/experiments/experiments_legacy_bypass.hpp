// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Legacy single-target dither bypass. CONFIRMED DEAD CODE: SelectTargetBypass
// is never called from anywhere in this codebase (TargetBypassMode::
// LegacySelectedShader, the only mode that would activate it, is "retained
// only so existing single-target helper code stays inert while the Dev UI
// migrates to hash-group target modes; it is not selectable" -- see
// experiments_common.hpp's TargetBypassMode). Kept, uncalled, exactly as it
// was in addon.cpp before this refactor: this pass is organizational only,
// not a dead-code deletion pass.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <reshade.hpp>

#include "dev/experiments/experiments_common.hpp"
#include "dev/experiments/experiments_recipe.hpp"
#include "pipeline_replacement_state.hpp"
#include "production/addon_shared.hpp"

namespace wuwa_tfr::dev {

extern wuwa_tfr::PipelineReplacementState<DeviceIdentity, reshade::api::pipeline>
    g_target_bypass_replacements;
extern std::atomic<std::uint64_t> g_target_bypass_structural_successes;
extern std::atomic<std::uint64_t> g_target_bypass_structural_failures;
extern std::atomic<std::uint64_t> g_target_bypass_ir_patch_successes;
extern std::atomic<std::uint64_t> g_target_bypass_ir_patch_failures;
extern std::atomic<std::uint64_t> g_target_bypass_stage2_structural_successes;
extern std::atomic<std::uint64_t> g_target_bypass_stage2_structural_failures;
extern std::atomic<std::uint64_t> g_target_bypass_stage2_ir_patch_successes;
extern std::atomic<std::uint64_t> g_target_bypass_stage2_ir_patch_failures;
extern std::atomic<std::uint64_t> g_target_bypass_assembly_successes;
extern std::atomic<std::uint64_t> g_target_bypass_assembly_failures;
extern std::atomic<std::uint64_t> g_target_bypass_dxil_validation_successes;
extern std::atomic<std::uint64_t> g_target_bypass_dxil_validation_failures;
extern std::atomic<std::uint64_t> g_target_bypass_replacement_psos_created;
extern std::atomic<std::uint64_t> g_target_bypass_replacement_create_failures;
extern std::atomic<std::uint64_t> g_target_bypass_bind_hits;
extern std::atomic<std::uint64_t> g_target_bypass_original_bind_hits;

extern std::shared_ptr<const std::vector<std::uint8_t>>
    g_target_bypass_bytecode;
extern std::uint64_t g_target_bypass_bytecode_hash;
extern std::string g_target_bypass_status;
extern bool g_target_bypass_compile_attempted;

void ResetTargetBypassDiagnosticsLocked();
void SelectTargetBypass(std::uint64_t selected_hash);
void SetTargetBypassFailureLocked(const char* stage, const std::string& reason);

std::shared_ptr<const std::vector<std::uint8_t>> GetTargetBypassBytecode(
    std::uint64_t target_hash, const reshade::api::shader_desc& original);

void CreateTargetBypassReplacement(
    reshade::api::device* owner,
    const std::shared_ptr<const TargetBypassPipelineRecipe>& recipe,
    std::uint64_t target_hash);

}  // namespace wuwa_tfr::dev

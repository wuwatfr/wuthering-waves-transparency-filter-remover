// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The live Dev replacement runtime: a parallel implementation of what
// FadePrimitiveRuntime does in Production, reusing the same
// AnalyzeFadePrimitiveV1 / PatchAllVerifiedFadePrimitiveInstancesToIdentity
// functions but with its own (weaker, Dev-only) concurrency/identity/
// lifecycle machinery, driven either by "every fully verified shader" (All
// v1) or by a manually curated hash list.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <reshade.hpp>

#include "dev/experiments/experiments_common.hpp"
#include "dev/experiments/experiments_recipe.hpp"
#include "fade_primitive_detector.hpp"
#include "pipeline_replacement_state.hpp"
#include "production/addon_shared.hpp"

namespace wuwa_tfr::dev {

using TargetBypassReplacementState =
    wuwa_tfr::PipelineReplacementState<DeviceIdentity, reshade::api::pipeline>;

struct FadePrimitiveExecutionTarget {
  std::uint32_t verified_instance_count = 0;
  std::string consumers;
  bool bytecode_attempted = false;
  bool failure_recorded = false;
  std::shared_ptr<const std::vector<std::uint8_t>> bytecode;
  std::string failure;
  std::uint64_t live_replacements = 0;
  std::uint64_t replacements_created = 0;
  std::uint64_t replacements_failed = 0;
  std::uint64_t replacement_bind_hits = 0;
  std::uint64_t original_bind_hits = 0;
};

extern TargetBypassReplacementState g_fade_primitive_execution_replacements;
extern std::mutex g_fade_primitive_execution_mutex;
// Persistent Dev preparation cache. Entries survive activation-policy changes
// and retain both the verified v1 metadata and successfully validated DXIL.
extern std::unordered_map<std::uint64_t, FadePrimitiveExecutionTarget>
    g_fade_primitive_execution_targets;
extern std::unordered_map<std::uint64_t, bool> g_manual_fade_primitive_hashes;
extern std::string g_fade_primitive_execution_status;
extern std::atomic<std::uint64_t> g_fade_primitive_execution_replacements_created;
extern std::atomic<std::uint64_t> g_fade_primitive_execution_replacements_failed;
extern std::atomic<std::uint64_t>
    g_fade_primitive_execution_replacements_destroyed;
extern std::atomic<std::uint64_t> g_fade_primitive_execution_shaders_prepared;
extern std::atomic<std::uint64_t> g_fade_primitive_execution_bind_hits;
extern std::atomic<std::uint64_t>
    g_fade_primitive_execution_original_bind_hits;

bool IsFadePrimitiveExecutionPrepared(std::uint64_t shader_hash);
std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic);
std::optional<FadePrimitiveExecutionTarget> VerifiedFadePrimitiveTarget(
    std::uint64_t shader_hash);
bool EnsureFadePrimitiveExecutionPreparation(std::uint64_t shader_hash);
bool EnsureAllVerifiedV1Target(std::uint64_t shader_hash);
bool IsFadePrimitiveExecutionActive(std::uint64_t shader_hash);
bool HasFadePrimitiveExecutionReplacement(
    DeviceIdentity device_key, std::uint64_t application_pipeline);
void RecordFadePrimitiveExecutionFailure(
    std::uint64_t shader_hash, const char* stage, const std::string& reason);

std::shared_ptr<const std::vector<std::uint8_t>>
GetFadePrimitiveExecutionBytecode(
    std::uint64_t shader_hash, const reshade::api::shader_desc& original);

void CreateFadePrimitiveExecutionReplacement(
    reshade::api::device* owner,
    const std::shared_ptr<const TargetBypassPipelineRecipe>& recipe,
    std::uint64_t shader_hash);

void RebuildFadePrimitiveExecutionTargets();
void ImportCurrentCapturedPsosIntoManualList();
void SetManualFadePrimitiveEnabled(std::uint64_t shader_hash, bool enabled);
void ClearManualFadePrimitiveList();

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// See experiments_legacy_bypass.hpp: this entire file is confirmed dead code
// (SelectTargetBypass, its only entry point, is never called), reproduced
// verbatim from addon.cpp as part of an organization-only refactor.

#include "dev/experiments/experiments_legacy_bypass.hpp"

#include "dev/experiments/experiments_fade_primitive.hpp"
#include "dev/trace/trace_state.hpp"
#include "target_dither_bypass.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

wuwa_tfr::PipelineReplacementState<DeviceIdentity, pipeline>
    g_target_bypass_replacements;
std::atomic<std::uint64_t> g_target_bypass_structural_successes{0};
std::atomic<std::uint64_t> g_target_bypass_structural_failures{0};
std::atomic<std::uint64_t> g_target_bypass_ir_patch_successes{0};
std::atomic<std::uint64_t> g_target_bypass_ir_patch_failures{0};
std::atomic<std::uint64_t> g_target_bypass_stage2_structural_successes{0};
std::atomic<std::uint64_t> g_target_bypass_stage2_structural_failures{0};
std::atomic<std::uint64_t> g_target_bypass_stage2_ir_patch_successes{0};
std::atomic<std::uint64_t> g_target_bypass_stage2_ir_patch_failures{0};
std::atomic<std::uint64_t> g_target_bypass_assembly_successes{0};
std::atomic<std::uint64_t> g_target_bypass_assembly_failures{0};
std::atomic<std::uint64_t> g_target_bypass_dxil_validation_successes{0};
std::atomic<std::uint64_t> g_target_bypass_dxil_validation_failures{0};
std::atomic<std::uint64_t> g_target_bypass_replacement_psos_created{0};
std::atomic<std::uint64_t> g_target_bypass_replacement_create_failures{0};
std::atomic<std::uint64_t> g_target_bypass_bind_hits{0};
std::atomic<std::uint64_t> g_target_bypass_original_bind_hits{0};

std::shared_ptr<const std::vector<std::uint8_t>> g_target_bypass_bytecode;
std::uint64_t g_target_bypass_bytecode_hash = 0;
std::string g_target_bypass_status = "waiting for target shader PSO";
bool g_target_bypass_compile_attempted = false;

void ResetTargetBypassDiagnosticsLocked() {
  g_target_bypass_structural_successes.store(0, std::memory_order_relaxed);
  g_target_bypass_structural_failures.store(0, std::memory_order_relaxed);
  g_target_bypass_ir_patch_successes.store(0, std::memory_order_relaxed);
  g_target_bypass_ir_patch_failures.store(0, std::memory_order_relaxed);
  g_target_bypass_stage2_structural_successes.store(
      0, std::memory_order_relaxed);
  g_target_bypass_stage2_structural_failures.store(
      0, std::memory_order_relaxed);
  g_target_bypass_stage2_ir_patch_successes.store(
      0, std::memory_order_relaxed);
  g_target_bypass_stage2_ir_patch_failures.store(
      0, std::memory_order_relaxed);
  g_target_bypass_assembly_successes.store(0, std::memory_order_relaxed);
  g_target_bypass_assembly_failures.store(0, std::memory_order_relaxed);
  g_target_bypass_dxil_validation_successes.store(
      0, std::memory_order_relaxed);
  g_target_bypass_dxil_validation_failures.store(
      0, std::memory_order_relaxed);
  g_target_bypass_replacement_psos_created.store(0,
      std::memory_order_relaxed);
  g_target_bypass_replacement_create_failures.store(0,
      std::memory_order_relaxed);
  g_target_bypass_bind_hits.store(0, std::memory_order_relaxed);
  g_target_bypass_original_bind_hits.store(0, std::memory_order_relaxed);
}

void SelectTargetBypass(std::uint64_t selected_hash) {
  if (selected_hash == 0) return;
  g_target_bypass_mode.store(TargetBypassMode::LegacySelectedShader,
      std::memory_order_relaxed);

  std::vector<std::pair<DeviceIdentity, device*>> live_devices;
  std::vector<std::shared_ptr<const TargetBypassPipelineRecipe>> recipes;
  {
    std::lock_guard lock(g_target_bypass_mutex);
    if (selected_hash == g_target_bypass_selected_hash.load(
            std::memory_order_relaxed))
      return;
    g_target_bypass_selected_hash.store(selected_hash,
        std::memory_order_relaxed);
    g_target_bypass_bytecode.reset();
    g_target_bypass_bytecode_hash = 0;
    g_target_bypass_compile_attempted = false;
    ResetTargetBypassDiagnosticsLocked();
    g_target_bypass_status = "target changed; rebuilding cached target PSOs";
    live_devices.reserve(g_target_bypass_devices.size());
    for (const auto& entry : g_target_bypass_devices)
      live_devices.push_back(entry);
  }

  for (const auto& [device_key, owner] : live_devices) {
    auto active = g_device_activity.Acquire(device_key);
    if (!active) continue;
    const auto removed = g_target_bypass_replacements.DrainOwner(device_key);
    for (const auto& item : removed) {
      if (item.replacements.final_antifade)
        DestroyTargetBypassReplacement(owner, *item.replacements.final_antifade);
    }
  }

  {
    std::lock_guard trace_lock(g_trace_mutex);
    recipes.reserve(g_target_bypass_recipes.size());
    for (const auto& [key, recipe] : g_target_bypass_recipes) {
      if (key.owner == recipe->device && recipe->shader_hash == selected_hash)
        recipes.push_back(recipe);
    }
  }
  for (const auto& recipe : recipes) {
    auto active = g_device_activity.Acquire(recipe->device);
    if (!active) continue;
    device* owner = nullptr;
    {
      std::lock_guard bypass_lock(g_target_bypass_mutex);
      const auto device_it = g_target_bypass_devices.find(recipe->device);
      if (device_it != g_target_bypass_devices.end()) owner = device_it->second;
    }
    if (owner) CreateTargetBypassReplacement(owner, recipe, selected_hash);
  }
  if (recipes.empty()) {
    std::lock_guard lock(g_target_bypass_mutex);
    if (selected_hash == SelectedTargetBypassHash())
      g_target_bypass_status = "no live cached PSO for selected target";
  }
  Log(reshade::log::level::info,
      "single-target bypass selected " + Hex64(selected_hash) +
      "; old replacement PSOs cleared; rebuilt " +
      std::to_string(recipes.size()) + " cached target PSOs");
}

void SetTargetBypassFailureLocked(
    const char* stage,
    const std::string& reason) {
  Log(reshade::log::level::error,
      std::string("single-target bypass ") + stage + " failed: " + reason);
  g_target_bypass_status = std::string(stage) + " failed (see log)";
}

std::shared_ptr<const std::vector<std::uint8_t>>
GetTargetBypassBytecode(
    std::uint64_t target_hash,
    const shader_desc& original) {
  std::lock_guard bypass_lock(g_target_bypass_mutex);
  if (target_hash != SelectedTargetBypassHash()) return {};
  if (g_target_bypass_compile_attempted &&
      g_target_bypass_bytecode_hash == target_hash)
    return g_target_bypass_bytecode;
  g_target_bypass_compile_attempted = true;
  g_target_bypass_bytecode_hash = target_hash;

  std::lock_guard inspection_lock(g_inspection_mutex);
  if (!g_dxc) g_dxc = new wuwa_tfr::DxcBridge(g_addon_directory);
  if (!g_dxc->available()) {
    SetTargetBypassFailureLocked("disassembly", g_dxc->init_error());
    return {};
  }
  if (!g_dxc->assembly_available()) {
    g_target_bypass_assembly_failures.fetch_add(1,
        std::memory_order_relaxed);
    SetTargetBypassFailureLocked("assembly", g_dxc->assembly_error());
    return {};
  }

  auto inspection = g_dxc->InspectShader(original.code, original.code_size);
  if (!inspection.success) {
    SetTargetBypassFailureLocked("disassembly", inspection.error);
    return {};
  }
  const bool dual_stage_target = false;
  auto rewritten = dual_stage_target
      ? wuwa_tfr::PatchSelectedDualDitherStagesToIdentity(
          inspection.original_ir)
      : wuwa_tfr::PatchSelectedTargetDitherToIdentity(
          inspection.original_ir);
  if (dual_stage_target) {
    if (rewritten.stage1_structural_verification_succeeded) {
      g_target_bypass_structural_successes.fetch_add(1,
          std::memory_order_relaxed);
    } else {
      g_target_bypass_structural_failures.fetch_add(1,
          std::memory_order_relaxed);
    }
    if (rewritten.stage2_structural_verification_succeeded) {
      g_target_bypass_stage2_structural_successes.fetch_add(1,
          std::memory_order_relaxed);
    } else if (rewritten.stage1_structural_verification_succeeded) {
      g_target_bypass_stage2_structural_failures.fetch_add(1,
          std::memory_order_relaxed);
    }
  }
  if (!rewritten.success) {
    if (!dual_stage_target) {
      g_target_bypass_structural_failures.fetch_add(1,
          std::memory_order_relaxed);
    }
    SetTargetBypassFailureLocked(
        dual_stage_target &&
                rewritten.stage1_structural_verification_succeeded
            ? "stage 2 structural verification"
            : "stage 1 structural verification",
        rewritten.error);
    return {};
  }
  if (!dual_stage_target) {
    g_target_bypass_structural_successes.fetch_add(1,
        std::memory_order_relaxed);
  }
  if (!rewritten.ir_patch_succeeded) {
    g_target_bypass_ir_patch_failures.fetch_add(1,
        std::memory_order_relaxed);
    SetTargetBypassFailureLocked("stage 1 IR patch",
        "patcher returned no rewritten IR");
    return {};
  }
  g_target_bypass_ir_patch_successes.fetch_add(1,
      std::memory_order_relaxed);
  if (dual_stage_target) {
    if (!rewritten.stage1_ir_patch_succeeded) {
      g_target_bypass_ir_patch_failures.fetch_add(1,
          std::memory_order_relaxed);
      SetTargetBypassFailureLocked("stage 1 IR patch",
          "patcher returned no rewritten IR");
      return {};
    }
    if (!rewritten.stage2_ir_patch_succeeded) {
      g_target_bypass_stage2_ir_patch_failures.fetch_add(1,
          std::memory_order_relaxed);
      SetTargetBypassFailureLocked("stage 2 IR patch",
          "patcher returned no rewritten IR");
      return {};
    }
    g_target_bypass_stage2_ir_patch_successes.fetch_add(1,
        std::memory_order_relaxed);
  }

  auto bytecode = std::make_shared<std::vector<std::uint8_t>>();
  std::string error;
  wuwa_tfr::DxilAssemblyValidationOutput assembly_validation;
  if (!g_dxc->AssembleAndValidate(
          rewritten.llvm_ir, *bytecode, error, assembly_validation)) {
    if (assembly_validation.assembly_succeeded) {
      g_target_bypass_assembly_successes.fetch_add(1,
          std::memory_order_relaxed);
      g_target_bypass_dxil_validation_failures.fetch_add(1,
          std::memory_order_relaxed);
      SetTargetBypassFailureLocked("DXIL validation", error);
    } else {
      g_target_bypass_assembly_failures.fetch_add(1,
          std::memory_order_relaxed);
      SetTargetBypassFailureLocked("assembly", error);
    }
    return {};
  }

  g_target_bypass_assembly_successes.fetch_add(1,
      std::memory_order_relaxed);
  g_target_bypass_dxil_validation_successes.fetch_add(1,
      std::memory_order_relaxed);
  g_target_bypass_bytecode = std::move(bytecode);
  g_target_bypass_status = dual_stage_target
      ? "both dither stages, assembly, and validation succeeded"
      : "structural verification, patch, assembly, and validation succeeded";
  return g_target_bypass_bytecode;
}

void CreateTargetBypassReplacement(
    device* owner,
    const std::shared_ptr<const TargetBypassPipelineRecipe>& recipe,
    std::uint64_t target_hash) {
  if (!owner || !recipe || recipe->application_pipeline.handle == 0 ||
      recipe->device != DeviceKey(owner) || recipe->shader_hash != target_hash)
    return;

  TargetBypassMaterializedRecipe materialized;
  std::string materialize_error;
  if (!MaterializeTargetBypassRecipe(*recipe, materialized, materialize_error)) {
    g_target_bypass_replacement_create_failures.fetch_add(1,
        std::memory_order_relaxed);
    std::lock_guard lock(g_target_bypass_mutex);
    SetTargetBypassFailureLocked("replacement PSO creation",
        "cached pipeline recipe invalid: " + materialize_error);
    return;
  }

  const auto bytecode = GetTargetBypassBytecode(
      target_hash, *materialized.pixel_shader);
  if (!bytecode) return;

  shader_desc replacement_shader = *materialized.pixel_shader;
  replacement_shader.code = bytecode->data();
  replacement_shader.code_size = bytecode->size();
  bool replaced_shader = false;
  for (auto& subobject : materialized.subobjects) {
    if (subobject.type != pipeline_subobject_type::pixel_shader ||
        subobject.data != materialized.pixel_shader)
      continue;
    subobject.data = &replacement_shader;
    replaced_shader = true;
  }
  if (!replaced_shader) {
    g_target_bypass_replacement_create_failures.fetch_add(1,
        std::memory_order_relaxed);
    std::lock_guard lock(g_target_bypass_mutex);
    SetTargetBypassFailureLocked(
        "replacement PSO creation", "target replacement descriptor was not found");
    return;
  }

  pipeline replacement{};
  {
    ScopedThreadFlag internal_create(g_target_bypass_internal_create);
    if (!owner->create_pipeline(recipe->layout,
            static_cast<std::uint32_t>(materialized.subobjects.size()),
            materialized.subobjects.data(), &replacement) ||
        replacement.handle == 0) {
      g_target_bypass_replacement_create_failures.fetch_add(1,
          std::memory_order_relaxed);
      std::lock_guard lock(g_target_bypass_mutex);
      SetTargetBypassFailureLocked(
          "replacement PSO creation", "device::create_pipeline returned false");
      return;
    }
  }

  std::optional<pipeline> previous;
  bool application_still_live = false;
  {
    std::lock_guard bypass_lock(g_target_bypass_mutex);
    if (target_hash == SelectedTargetBypassHash()) {
      std::lock_guard trace_lock(g_trace_mutex);
      const auto live = g_trace_pipelines.find(
          {DeviceKey(owner), recipe->application_pipeline.handle});
      const auto current_recipe = g_target_bypass_recipes.find(
          {DeviceKey(owner), recipe->application_pipeline.handle});
      if (live != g_trace_pipelines.end() &&
          live->second.shader_hash == target_hash &&
          current_recipe != g_target_bypass_recipes.end() &&
          current_recipe->second == recipe) {
        previous = g_target_bypass_replacements.PutFinalAntiFade(
            DeviceKey(owner), recipe->application_pipeline.handle, replacement);
        application_still_live = true;
      }
    }
  }
  if (!application_still_live) {
    DestroyTargetBypassReplacement(owner, replacement);
    return;
  }
  if (previous) {
    DestroyTargetBypassReplacement(owner, *previous);
    g_fade_primitive_execution_replacements_destroyed.fetch_add(1,
        std::memory_order_relaxed);
  }
  g_target_bypass_replacement_psos_created.fetch_add(1,
      std::memory_order_relaxed);
  std::lock_guard lock(g_target_bypass_mutex);
  g_target_bypass_status = "target replacement PSO ready";
}

}  // namespace wuwa_tfr::dev

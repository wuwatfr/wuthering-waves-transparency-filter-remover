// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/experiments/experiments_fade_primitive.hpp"

#include <algorithm>
#include <chrono>

#include "dev/trace/trace_events.hpp"
#include "dev/trace/trace_state.hpp"
#include "target_dither_bypass.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

TargetBypassReplacementState g_fade_primitive_execution_replacements;
std::mutex g_fade_primitive_execution_mutex;
std::unordered_map<std::uint64_t, FadePrimitiveExecutionTarget>
    g_fade_primitive_execution_targets;
std::unordered_map<std::uint64_t, bool> g_manual_fade_primitive_hashes;
std::string g_fade_primitive_execution_status =
    "All v1 mode: waiting for observed verified fade primitive shaders";
std::atomic<std::uint64_t> g_fade_primitive_execution_replacements_created{0};
std::atomic<std::uint64_t> g_fade_primitive_execution_replacements_failed{0};
std::atomic<std::uint64_t> g_fade_primitive_execution_replacements_destroyed{0};
std::atomic<std::uint64_t> g_fade_primitive_execution_shaders_prepared{0};
std::atomic<std::uint64_t> g_fade_primitive_execution_bind_hits{0};
std::atomic<std::uint64_t> g_fade_primitive_execution_original_bind_hits{0};

bool IsFadePrimitiveExecutionPrepared(std::uint64_t shader_hash) {
  std::lock_guard lock(g_fade_primitive_execution_mutex);
  return g_fade_primitive_execution_targets.contains(shader_hash);
}

std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic) {
  std::vector<const char*> names;
  for (const auto& instance : diagnostic.instances) {
    const char* name = wuwa_tfr::FadePrimitiveConsumerName(instance.consumer);
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  std::string result;
  for (const char* name : names) {
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result.empty() ? "unknown" : result;
}

std::optional<FadePrimitiveExecutionTarget> VerifiedFadePrimitiveTarget(
    std::uint64_t shader_hash) {
  std::lock_guard lock(g_inspection_mutex);
  const auto inspection = g_inspections.find(shader_hash);
  if (inspection == g_inspections.end() || !inspection->second.success ||
      inspection->second.fade_primitive.instances.empty())
    return std::nullopt;
  return FadePrimitiveExecutionTarget{
      .verified_instance_count = static_cast<std::uint32_t>(
          inspection->second.fade_primitive.instances.size()),
      .consumers = FadePrimitiveConsumers(inspection->second.fade_primitive)};
}

bool EnsureFadePrimitiveExecutionPreparation(std::uint64_t shader_hash) {
  const auto verified = VerifiedFadePrimitiveTarget(shader_hash);
  if (!verified) return false;
  std::lock_guard lock(g_fade_primitive_execution_mutex);
  const auto [_, inserted] = g_fade_primitive_execution_targets.try_emplace(
      shader_hash, *verified);
  return inserted || g_fade_primitive_execution_targets.contains(shader_hash);
}

bool EnsureAllVerifiedV1Target(std::uint64_t shader_hash) {
  if (g_target_bypass_mode.load(std::memory_order_relaxed) !=
      TargetBypassMode::AllVerifiedV1)
    return false;
  return EnsureFadePrimitiveExecutionPreparation(shader_hash);
}

bool IsFadePrimitiveExecutionActive(std::uint64_t shader_hash) {
  std::lock_guard lock(g_fade_primitive_execution_mutex);
  if (!g_fade_primitive_execution_targets.contains(shader_hash)) return false;
  if (g_target_bypass_mode.load(std::memory_order_relaxed) ==
      TargetBypassMode::AllVerifiedV1)
    return true;
  const auto manual = g_manual_fade_primitive_hashes.find(shader_hash);
  return manual != g_manual_fade_primitive_hashes.end() && manual->second;
}

bool HasFadePrimitiveExecutionReplacement(
    DeviceIdentity device_key,
    std::uint64_t application_pipeline) {
  return g_fade_primitive_execution_replacements.WithSelected(
      device_key, application_pipeline, true, true,
      [](pipeline) {});
}

void RecordFadePrimitiveExecutionFailure(
    std::uint64_t shader_hash,
    const char* stage,
    const std::string& reason) {
  std::lock_guard lock(g_fade_primitive_execution_mutex);
  const auto target = g_fade_primitive_execution_targets.find(shader_hash);
  if (target == g_fade_primitive_execution_targets.end()) return;
  target->second.failure = std::string(stage) + ": " + reason;
  if (!target->second.failure_recorded) {
    target->second.failure_recorded = true;
    ++target->second.replacements_failed;
    g_fade_primitive_execution_replacements_failed.fetch_add(1,
        std::memory_order_relaxed);
  }
  Log(reshade::log::level::warning,
      "fade-primitive execution-set " + Hex64(shader_hash) + " " +
      target->second.failure);
}

std::shared_ptr<const std::vector<std::uint8_t>>
GetFadePrimitiveExecutionBytecode(
    std::uint64_t shader_hash,
    const shader_desc& original) {
  std::uint32_t expected_instances = 0;
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    const auto target = g_fade_primitive_execution_targets.find(shader_hash);
    if (target == g_fade_primitive_execution_targets.end()) return {};
    if (target->second.bytecode_attempted) return target->second.bytecode;
    target->second.bytecode_attempted = true;
    expected_instances = target->second.verified_instance_count;
  }

  std::lock_guard inspection_lock(g_inspection_mutex);
  if (!g_dxc) g_dxc = new wuwa_tfr::DxcBridge(g_addon_directory);
  if (!g_dxc->available()) {
    RecordFadePrimitiveExecutionFailure(shader_hash, "disassembly",
        g_dxc->init_error());
    return {};
  }
  const auto inspection = g_dxc->InspectShader(original.code, original.code_size);
  if (!inspection.success) {
    RecordFadePrimitiveExecutionFailure(shader_hash, "disassembly",
        inspection.error);
    return {};
  }
  const auto rewritten =
      wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesToIdentity(
          inspection.original_ir);
  if (!rewritten.success ||
      rewritten.verified_instance_count != expected_instances ||
      rewritten.patched_instance_count != expected_instances) {
    RecordFadePrimitiveExecutionFailure(shader_hash, "structural verification",
        rewritten.error.empty()
            ? "verified primitive instance count changed before patch"
            : rewritten.error);
    return {};
  }

  auto shared = std::make_shared<std::vector<std::uint8_t>>();
  std::string error;
  wuwa_tfr::DxilAssemblyValidationOutput assembly_validation;
  if (!g_dxc->AssembleAndValidate(
          rewritten.llvm_ir, *shared, error, assembly_validation)) {
    RecordFadePrimitiveExecutionFailure(shader_hash,
        assembly_validation.assembly_succeeded ? "DXIL validation" : "assembly",
        error);
    return {};
  }
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    const auto target = g_fade_primitive_execution_targets.find(shader_hash);
    if (target == g_fade_primitive_execution_targets.end()) return {};
    target->second.bytecode = shared;
  }
  g_fade_primitive_execution_shaders_prepared.fetch_add(1,
      std::memory_order_relaxed);
  Log(reshade::log::level::info,
      "fade-primitive execution-set " + Hex64(shader_hash) +
      " verified, patched, assembled, and validated");
  return shared;
}

void CreateFadePrimitiveExecutionReplacement(
    device* owner,
    const std::shared_ptr<const TargetBypassPipelineRecipe>& recipe,
    std::uint64_t shader_hash) {
  if (!owner || !recipe || recipe->application_pipeline.handle == 0 ||
      recipe->device != DeviceKey(owner) || recipe->shader_hash != shader_hash ||
      !IsFadePrimitiveExecutionActive(shader_hash) ||
      HasFadePrimitiveExecutionReplacement(
          recipe->device, recipe->application_pipeline.handle))
    return;

  TargetBypassMaterializedRecipe materialized;
  std::string materialize_error;
  if (!MaterializeTargetBypassRecipe(*recipe, materialized, materialize_error)) {
    RecordFadePrimitiveExecutionFailure(shader_hash, "replacement PSO creation",
        "cached pipeline recipe invalid: " + materialize_error);
    return;
  }
  const auto bytecode = GetFadePrimitiveExecutionBytecode(
      shader_hash, *materialized.pixel_shader);
  if (!bytecode) return;

  shader_desc replacement_shader = *materialized.pixel_shader;
  replacement_shader.code = bytecode->data();
  replacement_shader.code_size = bytecode->size();
  bool replaced_shader = false;
  for (auto& subobject : materialized.subobjects) {
    if (subobject.type == pipeline_subobject_type::pixel_shader &&
        subobject.data == materialized.pixel_shader) {
      subobject.data = &replacement_shader;
      replaced_shader = true;
    }
  }
  if (!replaced_shader) {
    RecordFadePrimitiveExecutionFailure(shader_hash, "replacement PSO creation",
        "target replacement descriptor was not found");
    return;
  }

  pipeline replacement{};
  {
    ScopedThreadFlag internal_create(g_target_bypass_internal_create);
    if (!owner->create_pipeline(recipe->layout,
            static_cast<std::uint32_t>(materialized.subobjects.size()),
            materialized.subobjects.data(), &replacement) ||
        replacement.handle == 0) {
      RecordFadePrimitiveExecutionFailure(shader_hash,
          "replacement PSO creation", "device::create_pipeline returned false");
      return;
    }
  }

  std::optional<pipeline> previous;
  bool application_still_live = false;
  {
    std::lock_guard batch_lock(g_fade_primitive_execution_mutex);
    if (g_fade_primitive_execution_targets.contains(shader_hash)) {
      std::lock_guard trace_lock(g_trace_mutex);
      const auto live = g_trace_pipelines.find(
          {DeviceKey(owner), recipe->application_pipeline.handle});
      const auto current_recipe = g_target_bypass_recipes.find(
          {DeviceKey(owner), recipe->application_pipeline.handle});
      if (live != g_trace_pipelines.end() &&
          live->second.shader_hash == shader_hash &&
          current_recipe != g_target_bypass_recipes.end() &&
          current_recipe->second == recipe) {
        previous = g_fade_primitive_execution_replacements.PutFinalAntiFade(
            DeviceKey(owner), recipe->application_pipeline.handle, replacement);
        application_still_live = true;
      }
    }
  }
  if (!application_still_live) {
    DestroyTargetBypassReplacement(owner, replacement);
    return;
  }
  if (previous) DestroyTargetBypassReplacement(owner, *previous);
  g_fade_primitive_execution_replacements_created.fetch_add(1,
      std::memory_order_relaxed);
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    const auto target = g_fade_primitive_execution_targets.find(shader_hash);
    if (target != g_fade_primitive_execution_targets.end()) {
      ++target->second.replacements_created;
      if (!previous) ++target->second.live_replacements;
    }
  }
}

void RebuildFadePrimitiveExecutionTargets() {
  using RebuildClock = std::chrono::steady_clock;
  const auto rebuild_started = RebuildClock::now();
  const TargetBypassMode mode = g_target_bypass_mode.load(
      std::memory_order_relaxed);
  std::size_t inspected_shader_count = 0;
  std::size_t enabled_manual_hash_count = 0;
  std::vector<std::pair<std::uint64_t, FadePrimitiveExecutionTarget>>
      newly_verified_targets;
  std::unordered_set<std::uint64_t> target_hashes;
  const auto target_selection_started = RebuildClock::now();
  if (mode == TargetBypassMode::AllVerifiedV1) {
    std::lock_guard inspection_lock(g_inspection_mutex);
    newly_verified_targets.reserve(g_inspections.size());
    for (const auto& [shader_hash, inspection] : g_inspections) {
      ++inspected_shader_count;
      if (!inspection.success || inspection.fade_primitive.instances.empty())
        continue;
      target_hashes.insert(shader_hash);
      newly_verified_targets.emplace_back(shader_hash, FadePrimitiveExecutionTarget{
          .verified_instance_count = static_cast<std::uint32_t>(
              inspection.fade_primitive.instances.size()),
          .consumers = FadePrimitiveConsumers(inspection.fade_primitive)});
    }
    std::lock_guard preparation_lock(g_fade_primitive_execution_mutex);
    for (auto& [shader_hash, target] : newly_verified_targets)
      g_fade_primitive_execution_targets.try_emplace(shader_hash,
          std::move(target));
  } else if (mode == TargetBypassMode::ManualShaderList) {
    std::vector<std::uint64_t> enabled_hashes;
    {
      std::lock_guard lock(g_fade_primitive_execution_mutex);
      for (const auto& [shader_hash, enabled] : g_manual_fade_primitive_hashes) {
        if (enabled) {
          enabled_hashes.push_back(shader_hash);
          ++enabled_manual_hash_count;
        }
      }
    }
    for (const std::uint64_t shader_hash : enabled_hashes) {
      if (EnsureFadePrimitiveExecutionPreparation(shader_hash))
        target_hashes.insert(shader_hash);
    }
  }
  const auto target_selection_finished = RebuildClock::now();

  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    g_fade_primitive_execution_status = mode == TargetBypassMode::AllVerifiedV1
        ? "All v1: every observed fully verified shader is eligible"
        : "Manual list: only enabled, fully verified v1 hashes are eligible";
  }

  const std::uint64_t prepared_before =
      g_fade_primitive_execution_shaders_prepared.load(
          std::memory_order_relaxed);
  const std::uint64_t replacements_created_before =
      g_fade_primitive_execution_replacements_created.load(
          std::memory_order_relaxed);
  const std::uint64_t replacements_failed_before =
      g_fade_primitive_execution_replacements_failed.load(
          std::memory_order_relaxed);
  std::vector<std::shared_ptr<const TargetBypassPipelineRecipe>> recipes;
  std::unordered_set<std::uint64_t> live_target_hashes;
  std::unordered_set<std::uint64_t> recipe_target_hashes;
  std::size_t live_pipeline_count = 0;
  std::size_t cached_recipe_count = 0;
  const auto recipe_scan_started = RebuildClock::now();
  {
    std::lock_guard trace_lock(g_trace_mutex);
    live_pipeline_count = g_trace_pipelines.size();
    cached_recipe_count = g_target_bypass_recipes.size();
    recipes.reserve(g_target_bypass_recipes.size());
    for (const auto& [key, pipeline] : g_trace_pipelines) {
      (void)key;
      if (target_hashes.contains(pipeline.shader_hash))
        live_target_hashes.insert(pipeline.shader_hash);
    }
    for (const auto& [key, recipe] : g_target_bypass_recipes) {
      (void)key;
      if (recipe && target_hashes.contains(recipe->shader_hash)) {
        recipes.push_back(recipe);
        recipe_target_hashes.insert(recipe->shader_hash);
      }
    }
  }
  const auto recipe_scan_finished = RebuildClock::now();
  for (const std::uint64_t shader_hash : live_target_hashes) {
    if (!recipe_target_hashes.contains(shader_hash)) {
      RecordFadePrimitiveExecutionFailure(shader_hash, "replacement PSO creation",
          "live eligible PSO has no supported cached pipeline recipe");
    }
  }
  const auto replacement_creation_started = RebuildClock::now();
  std::size_t retained_replacement_count = 0;
  std::size_t replacement_creation_attempts = 0;
  for (const auto& recipe : recipes) {
    if (HasFadePrimitiveExecutionReplacement(
            recipe->device, recipe->application_pipeline.handle)) {
      ++retained_replacement_count;
      continue;
    }
    auto active = g_device_activity.Acquire(recipe->device);
    if (!active) continue;
    device* owner = nullptr;
    {
      std::lock_guard lock(g_target_bypass_mutex);
      const auto device = g_target_bypass_devices.find(recipe->device);
      if (device != g_target_bypass_devices.end()) owner = device->second;
    }
    if (owner) {
      ++replacement_creation_attempts;
      CreateFadePrimitiveExecutionReplacement(owner, recipe, recipe->shader_hash);
    }
  }
  const auto replacement_creation_finished = RebuildClock::now();
  if (recipes.empty() && !target_hashes.empty()) {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    g_fade_primitive_execution_status += "; no live cached eligible PSOs";
  }
  Log(reshade::log::level::info,
      "Fade Primitive v1 target activation: " +
      std::to_string(target_hashes.size()) + " active shaders");
  const auto elapsed_ms = [](const RebuildClock::time_point& begin,
                              const RebuildClock::time_point& end) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin)
        .count();
  };
  Log(reshade::log::level::info,
      "Dev target-mode rebuild timing: mode=" + std::string(
          mode == TargetBypassMode::AllVerifiedV1 ? "all-v1" : "manual") +
      " total_ms=" + std::to_string(elapsed_ms(rebuild_started,
          RebuildClock::now())) +
      " target_selection_ms=" + std::to_string(elapsed_ms(
          target_selection_started, target_selection_finished)) +
      " inspected_shaders=" + std::to_string(inspected_shader_count) +
      " manual_enabled_hashes=" + std::to_string(enabled_manual_hash_count) +
      " active_shaders=" + std::to_string(target_hashes.size()) +
      " recipe_scan_ms=" + std::to_string(elapsed_ms(recipe_scan_started,
          recipe_scan_finished)) +
      " live_psos=" + std::to_string(live_pipeline_count) +
      " cached_recipes=" + std::to_string(cached_recipe_count) +
      " eligible_recipes=" + std::to_string(recipes.size()) +
      " retained_replacements=" + std::to_string(retained_replacement_count) +
      " replacement_create_attempts=" + std::to_string(
          replacement_creation_attempts) +
      " create_ms=" + std::to_string(elapsed_ms(replacement_creation_started,
          replacement_creation_finished)) +
      " shaders_prepared_now=" + std::to_string(
          g_fade_primitive_execution_shaders_prepared.load(
              std::memory_order_relaxed) - prepared_before) +
      " replacements_created_now=" + std::to_string(
          g_fade_primitive_execution_replacements_created.load(
              std::memory_order_relaxed) - replacements_created_before) +
      " replacements_failed_now=" + std::to_string(
          g_fade_primitive_execution_replacements_failed.load(
              std::memory_order_relaxed) - replacements_failed_before) +
      " replacements_destroyed_now=0");
}

void ImportCurrentCapturedPsosIntoManualList() {
  std::unordered_set<std::uint64_t> hashes;
  for (const auto& row : ConcreteTraceSnapshot())
    hashes.insert(row.pipeline.shader_hash);
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    for (const std::uint64_t shader_hash : hashes)
      g_manual_fade_primitive_hashes.try_emplace(shader_hash, false);
    g_fade_primitive_execution_status =
        "imported " + std::to_string(hashes.size()) +
        " captured shader hash groups into the manual list";
  }
}

void SetManualFadePrimitiveEnabled(std::uint64_t shader_hash, bool enabled) {
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    g_manual_fade_primitive_hashes[shader_hash] = enabled;
  }
  if (g_target_bypass_mode.load(std::memory_order_relaxed) ==
      TargetBypassMode::ManualShaderList)
    RebuildFadePrimitiveExecutionTargets();
}

void ClearManualFadePrimitiveList() {
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    g_manual_fade_primitive_hashes.clear();
  }
  if (g_target_bypass_mode.load(std::memory_order_relaxed) ==
      TargetBypassMode::ManualShaderList)
    RebuildFadePrimitiveExecutionTargets();
}

}  // namespace wuwa_tfr::dev

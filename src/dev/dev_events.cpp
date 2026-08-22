// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_events.hpp"

#include "dev/experiments/experiments_common.hpp"
#include "dev/experiments/experiments_fade_primitive.hpp"
#include "dev/experiments/experiments_legacy_bypass.hpp"
#include "dev/experiments/experiments_recipe.hpp"
#include "dev/trace/trace_events.hpp"
#include "dev/trace/trace_state.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

void OnInitDeviceHook(device* owner) {
  std::lock_guard lock(g_target_bypass_mutex);
  g_target_bypass_devices[DeviceKey(owner)] = owner;
}

void OnDestroyDeviceHook(device* owner) {
  {
    std::lock_guard lock(g_target_bypass_mutex);
    g_target_bypass_devices.erase(DeviceKey(owner));
  }
  const auto replacements = g_target_bypass_replacements.DrainOwner(
      DeviceKey(owner));
  for (const auto& item : replacements) {
    if (item.replacements.final_antifade)
      DestroyTargetBypassReplacement(owner, *item.replacements.final_antifade);
  }
  const DeviceIdentity device_key = DeviceKey(owner);
  std::unordered_map<std::uint64_t, std::uint64_t>
      batch_replacement_shader_hashes;
  {
    std::lock_guard lock(g_trace_mutex);
    for (const auto& [key, pipeline] : g_trace_pipelines) {
      if (key.owner == device_key)
        batch_replacement_shader_hashes.emplace(key.handle, pipeline.shader_hash);
    }
  }
  const auto batch_replacements =
      g_fade_primitive_execution_replacements.DrainOwner(device_key);
  std::unordered_map<std::uint64_t, std::uint64_t>
      released_replacements_by_shader;
  for (const auto& item : batch_replacements) {
    if (item.replacements.final_antifade) {
      DestroyTargetBypassReplacement(owner, *item.replacements.final_antifade);
      g_fade_primitive_execution_replacements_destroyed.fetch_add(1,
          std::memory_order_relaxed);
      if (const auto shader = batch_replacement_shader_hashes.find(
              item.application_pipeline);
          shader != batch_replacement_shader_hashes.end())
        ++released_replacements_by_shader[shader->second];
    }
  }
  if (!released_replacements_by_shader.empty()) {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    for (const auto& [shader_hash, released] : released_replacements_by_shader) {
      const auto target = g_fade_primitive_execution_targets.find(shader_hash);
      if (target == g_fade_primitive_execution_targets.end()) continue;
      target->second.live_replacements =
          target->second.live_replacements > released
          ? target->second.live_replacements - released
          : 0;
    }
  }
  {
    std::lock_guard lock(g_trace_mutex);
    g_trace_pso_incarnations.DestroyWhere(
        [device_key](const auto& key) { return key.owner == device_key; });
    g_trace_resource_incarnations.DestroyWhere(
        [device_key](const auto& key) { return key.owner == device_key; });
    g_trace_view_incarnations.DestroyWhere(
        [device_key](const auto& key) { return key.owner == device_key; });
    std::erase_if(g_trace_pipelines, [device_key](const auto& entry) {
      return entry.first.owner == device_key;
    });
    std::erase_if(g_target_bypass_recipes, [device_key](const auto& entry) {
      return entry.first.owner == device_key;
    });
  }
}

void RegisterDevEvents() {
  reshade::register_event<reshade::addon_event::init_resource>(
      OnInitTraceResource);
  reshade::register_event<reshade::addon_event::destroy_resource>(
      OnDestroyTraceResource);
  reshade::register_event<reshade::addon_event::init_resource_view>(
      OnInitTraceResourceView);
  reshade::register_event<reshade::addon_event::destroy_resource_view>(
      OnDestroyTraceResourceView);
  reshade::register_event<reshade::addon_event::init_pipeline>(
      OnInitTracePipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(
      OnDestroyTracePipeline);
  reshade::register_event<reshade::addon_event::init_command_list>(
      OnInitTraceCommandList);
  reshade::register_event<reshade::addon_event::destroy_command_list>(
      OnDestroyTraceCommandList);
  reshade::register_event<reshade::addon_event::reset_command_list>(
      OnResetTraceCommandList);
  reshade::register_event<reshade::addon_event::bind_pipeline>(
      OnBindTracePipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline_states>(
      OnBindTracePipelineStates);
  reshade::register_event<reshade::addon_event::bind_vertex_buffers>(
      OnBindTraceVertexBuffers);
  reshade::register_event<reshade::addon_event::bind_index_buffer>(
      OnBindTraceIndexBuffer);
  reshade::register_event<
      reshade::addon_event::bind_render_targets_and_depth_stencil>(
      OnBindTraceRenderTargets);
  reshade::register_event<reshade::addon_event::begin_render_pass>(
      OnBeginTraceRenderPass);
  reshade::register_event<reshade::addon_event::end_render_pass>(
      OnEndTraceRenderPass);
  reshade::register_event<reshade::addon_event::push_constants>(
      OnPushTraceConstants);
  reshade::register_event<reshade::addon_event::push_descriptors>(
      OnPushTraceDescriptors);
  reshade::register_event<reshade::addon_event::bind_descriptor_tables>(
      OnBindTraceDescriptorTables);
  reshade::register_event<reshade::addon_event::draw>(OnTraceDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(
      OnTraceDrawIndexed);
  reshade::register_event<reshade::addon_event::dispatch_mesh>(
      OnTraceDispatchMesh);
  reshade::register_event<
      reshade::addon_event::draw_or_dispatch_indirect>(OnTraceIndirect);
  reshade::register_event<
      reshade::addon_event::execute_secondary_command_list>(
      OnExecuteSecondaryTrace);
  reshade::register_event<reshade::addon_event::execute_command_list>(
      OnExecuteTrace);
  reshade::register_event<reshade::addon_event::present>(OnTracePresent);
}

}  // namespace wuwa_tfr::dev

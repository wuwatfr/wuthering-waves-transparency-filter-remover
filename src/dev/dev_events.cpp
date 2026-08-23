// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_events.hpp"

#include "dev/dev_runtime.hpp"
#include "dev/trace/trace_events.hpp"
#include "dev/trace/trace_state.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

void OnDestroyDeviceHook(device* owner) {
  const DeviceIdentity device_key = DeviceKey(owner);
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
}

void RegisterDevEvents() {
  reshade::register_event<reshade::addon_event::destroy_device>(
      OnDestroyDeviceHook);

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

  // The shared FadePrimitiveRuntime's own lifecycle, registered as additional
  // handlers alongside the trace/experiment ones above. It is a second,
  // independent object of the same class Production uses -- see
  // dev/dev_runtime.hpp.
  reshade::register_event<reshade::addon_event::init_device>(
      OnInitDevRuntimeDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(
      OnDestroyDevRuntimeDevice);
  reshade::register_event<reshade::addon_event::init_pipeline>(
      OnInitDevRuntimePipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(
      OnDestroyDevRuntimePipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline>(
      OnBindDevRuntimePipeline);

  // g_dev_prefade_hypothesis_runtime's lifecycle -- a second, independent
  // FadePrimitiveRuntime instance running the experimental pre-Fade FMin
  // operand-1 hypothesis patch instead of the identity-phi one. See
  // dev/dev_runtime.hpp.
  reshade::register_event<reshade::addon_event::init_device>(
      OnInitPreFadeHypothesisDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(
      OnDestroyPreFadeHypothesisDevice);
  reshade::register_event<reshade::addon_event::init_pipeline>(
      OnInitPreFadeHypothesisPipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(
      OnDestroyPreFadeHypothesisPipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline>(
      OnBindPreFadeHypothesisPipeline);
}

}  // namespace wuwa_tfr::dev

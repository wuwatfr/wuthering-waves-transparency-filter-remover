// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/trace/trace_events.hpp"

#include <algorithm>
#include <cmath>

#include "dev/capture/fade_control_runtime.hpp"
#include "dev/dev_inspection.hpp"
#include "dev/dev_runtime.hpp"
#include "dev/trace/trace_report.hpp"
#include "pixel_shader_identity.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

bool IsNormalOnlyRimSkipRow(const ConcreteTraceRow& row) noexcept {
  return wuwa_tfr::TraceGeometryIsConcrete(row.key.geometry) &&
      (row.key.geometry.kind == wuwa_tfr::TraceDrawKind::Direct ||
          row.key.geometry.kind == wuwa_tfr::TraceDrawKind::Indexed) &&
      wuwa_tfr::TraceNormalOnlySubmissionCandidate(row.windows);
}

void OnInitTracePipeline(
    device* owner,
    pipeline_layout layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    pipeline handle) {
  if (g_dev_runtime_internal_pipeline_event) return;
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      handle.handle == 0)
    return;

  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return;

  const TracePipelineKey key{DeviceKey(owner), handle.handle};
  const shader_desc* descriptor = nullptr;
  std::uint64_t shader_hash = 0;
  if (!wuwa_tfr::FindDxilPixelShader(
          subobject_count, subobjects, descriptor, shader_hash)) {
    std::lock_guard lock(g_trace_mutex);
    g_trace_pso_incarnations.Destroy(key);
    g_trace_pipelines.erase(key);
    return;
  }

  wuwa_tfr::ExecutionPipelineIdentity pipeline_info = DescribeTracePipeline(
      subobject_count, subobjects, shader_hash);

  std::uint64_t creation_fingerprint = kTraceFnvOffset;
  TraceHashValue(creation_fingerprint, layout.handle);
  TraceHashValue(creation_fingerprint, pipeline_info.shader_hash);
  TraceHashValue(creation_fingerprint, pipeline_info.context_hash);
  TraceHashValue(creation_fingerprint, pipeline_info.primitive_topology);
  const TracePsoIdentity identity{
      creation_fingerprint, shader_hash, pipeline_info.context_hash};

  {
    std::lock_guard lock(g_trace_mutex);
    const auto activated = g_trace_pso_incarnations.Activate(key, identity);
    if (activated.rotated_without_destroy && activated.previous_identity) {
      g_trace_pso_lifecycle_ambiguities.Record(key,
          *activated.previous_identity, identity,
          g_trace_frame_id.load(std::memory_order_relaxed),
          ++g_trace_lifecycle_event_serial);
    }
    pipeline_info.incarnation_id = activated.id;
    pipeline_info.device = key.owner;
    pipeline_info.application_pipeline = key.handle;
    pipeline_info.pso_fingerprint = creation_fingerprint;
    g_trace_pipelines[key] = pipeline_info;
    g_trace_shaders.try_emplace(shader_hash);
    const std::size_t pruned =
        g_trace_pso_incarnations.PruneTo(kMaxTrackedPsoIncarnations);
    if (pruned != 0) {
      g_trace_incarnation_prunes += pruned;
      g_trace_identity_capacity_exceeded = true;
      std::erase_if(g_trace_pipelines, [](const auto& entry) {
        return g_trace_pso_incarnations.FindActive(entry.first) == nullptr;
      });
    }
  }
}

void OnDestroyTracePipeline(device* owner, pipeline handle) {
  if (g_dev_runtime_internal_pipeline_event) return;
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      handle.handle == 0)
    return;
  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return;

  const TracePipelineKey key{DeviceKey(owner), handle.handle};
  std::lock_guard lock(g_trace_mutex);
  g_trace_pso_incarnations.Destroy(key);
  g_trace_pipelines.erase(key);
}

TraceResourceIdentity ResourceIdentity(
    const resource_desc& desc, resource_usage initial_usage) noexcept {
  std::uint64_t hash = kTraceFnvOffset;
  TraceHashValue(hash, desc.type);
  if (desc.type == resource_type::buffer) {
    TraceHashValue(hash, desc.buffer.size);
    TraceHashValue(hash, desc.buffer.structured.stride);
  } else {
    TraceHashValue(hash, desc.texture.width);
    TraceHashValue(hash, desc.texture.height);
    TraceHashValue(hash, desc.texture.depth_or_layers);
    TraceHashValue(hash, desc.texture.levels);
    TraceHashValue(hash, desc.texture.format);
    TraceHashValue(hash, desc.texture.samples);
  }
  TraceHashValue(hash, desc.heap);
  TraceHashValue(hash, desc.usage);
  TraceHashValue(hash, desc.flags);
  TraceHashValue(hash, initial_usage);
  constexpr std::uint32_t kKnownDynamicUsage =
      static_cast<std::uint32_t>(resource_usage::copy_dest) |
      static_cast<std::uint32_t>(resource_usage::unordered_access) |
      static_cast<std::uint32_t>(resource_usage::stream_output);
  const std::uint32_t usage = static_cast<std::uint32_t>(desc.usage) |
      static_cast<std::uint32_t>(initial_usage);
  return {hash, desc.heap != memory_heap::default_ ||
                    (usage & kKnownDynamicUsage) != 0};
}

void OnInitTraceResource(device* owner, const resource_desc& desc,
    const subresource_data*, resource_usage initial_usage, resource handle) {
  if (!g_target_process || !owner || handle.handle == 0) return;
  std::lock_guard lock(g_trace_mutex);
  const TracePipelineKey key{DeviceKey(owner), handle.handle};
  const TraceResourceIdentity identity = ResourceIdentity(desc, initial_usage);
  const auto result = g_trace_resource_incarnations.Activate(
      key, identity);
  if (result.rotated_without_destroy && result.previous_identity) {
    g_trace_resource_lifecycle_ambiguities.Record(key,
        *result.previous_identity, identity,
        g_trace_frame_id.load(std::memory_order_relaxed),
        ++g_trace_lifecycle_event_serial);
  }
  const std::size_t pruned = g_trace_resource_incarnations.PruneTo(
      kMaxTrackedResourceIncarnations);
  if (pruned != 0) {
    g_trace_incarnation_prunes += pruned;
    g_trace_identity_capacity_exceeded = true;
  }
}

void OnDestroyTraceResource(device* owner, resource handle) {
  if (!g_target_process || !owner || handle.handle == 0) return;
  std::lock_guard lock(g_trace_mutex);
  g_trace_resource_incarnations.Destroy({DeviceKey(owner), handle.handle});
}

void OnInitTraceResourceView(device* owner, resource resource_handle,
    resource_usage usage, const resource_view_desc& desc,
    resource_view view) {
  if (!g_target_process || !owner || view.handle == 0) return;
  std::lock_guard lock(g_trace_mutex);
  std::uint64_t identity = kTraceFnvOffset;
  TraceHashValue(identity,
      ActiveResourceIncarnationLocked(
          DeviceKey(owner), resource_handle).incarnation);
  TraceHashValue(identity, usage);
  TraceHashValue(identity, desc.type);
  TraceHashValue(identity, desc.format);
  if (desc.type == resource_view_type::buffer ||
      desc.type == resource_view_type::acceleration_structure) {
    TraceHashValue(identity, desc.buffer.offset);
    TraceHashValue(identity, desc.buffer.size);
  } else {
    TraceHashValue(identity, desc.texture.first_level);
    TraceHashValue(identity, desc.texture.levels);
    TraceHashValue(identity, desc.texture.first_layer);
    TraceHashValue(identity, desc.texture.layers);
  }
  wuwa_tfr::UpdateTraceVersionedSlot(g_trace_view_incarnations,
      {DeviceKey(owner), view.handle}, identity);
  const std::size_t pruned =
      g_trace_view_incarnations.PruneTo(kMaxTrackedViewIncarnations);
  if (pruned != 0) {
    g_trace_incarnation_prunes += pruned;
    g_trace_identity_capacity_exceeded = true;
  }
}

void OnDestroyTraceResourceView(device* owner, resource_view view) {
  if (!g_target_process || !owner || view.handle == 0) return;
  std::lock_guard lock(g_trace_mutex);
  g_trace_view_incarnations.Destroy({DeviceKey(owner), view.handle});
}

void OnInitTraceCommandList(command_list* cmd_list) {
  if (!g_target_process || !cmd_list) return;
  device* owner = cmd_list->get_device();
  if (!owner || owner->get_api() != device_api::d3d12) return;
  auto* trace = cmd_list->create_private_data<CommandListTrace>();
  if (trace) trace->device = DeviceKey(owner);
}

void OnDestroyTraceCommandList(command_list* cmd_list) {
  if (!cmd_list || !cmd_list->get_private_data<CommandListTrace>()) return;
  cmd_list->destroy_private_data<CommandListTrace>();
}

void OnResetTraceCommandList(command_list* cmd_list) {
  if (!cmd_list) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (trace) trace->Reset();
}

void OnBindTracePipeline(
    command_list* cmd_list,
    pipeline_stage stages,
    pipeline handle) {
  if (g_dev_runtime_internal_pipeline_event) return;
  if (!cmd_list ||
      (stages & pipeline_stage::pixel_shader) != pipeline_stage::pixel_shader)
    return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  std::lock_guard lock(g_trace_mutex);
  const auto pipeline_it = g_trace_pipelines.find(
      {trace->device, handle.handle});
  if (pipeline_it == g_trace_pipelines.end()) {
    trace->bound_pso_incarnation = 0;
    trace->bound_pipeline.reset();
    return;
  }
  trace->bound_pso_incarnation = pipeline_it->second.incarnation_id;
  trace->bound_pipeline = pipeline_it->second;
}

void OnBindTracePipelineStates(command_list* cmd_list, std::uint32_t count,
    const dynamic_state* states, const std::uint32_t* values) {
  if (!cmd_list || count == 0 || !states || !values) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (states[i] != dynamic_state::primitive_topology) continue;
    trace->primitive_topology = values[i];
    trace->topology_observed = true;
  }
}

void OnBindTraceVertexBuffers(command_list* cmd_list, std::uint32_t first,
    std::uint32_t count, const resource* buffers,
    const std::uint64_t* offsets, const std::uint32_t* strides) {
  if (!cmd_list || count == 0 || !buffers || !offsets) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  std::lock_guard lock(g_trace_mutex);
  trace->vertex_buffers_observed = true;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t slot = first + i;
    if (buffers[i].handle == 0) {
      trace->vertex_buffers.erase(slot);
      continue;
    }
    const auto resource =
        ActiveResourceIncarnationLocked(trace->device, buffers[i]);
    trace->vertex_buffers[slot] = {
        slot, resource.incarnation, offsets[i],
        strides ? strides[i] : 0, resource.dynamic_contents};
  }
}

void OnBindTraceIndexBuffer(command_list* cmd_list, resource buffer,
    std::uint64_t offset, std::uint32_t index_size) {
  if (!cmd_list) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  std::lock_guard lock(g_trace_mutex);
  trace->index_buffer_observed = true;
  if (buffer.handle == 0) {
    trace->index_buffer.reset();
    return;
  }
  const auto resource =
      ActiveResourceIncarnationLocked(trace->device, buffer);
  trace->index_buffer = wuwa_tfr::TraceIndexBinding{
      resource.incarnation, offset, index_size, resource.dynamic_contents};
}

void SetTracePass(command_list* cmd_list, std::uint32_t count,
    const resource_view* rtvs, resource_view dsv,
    std::uint64_t pass_metadata) {
  if (!cmd_list) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  std::lock_guard lock(g_trace_mutex);
  std::vector<std::uint64_t> views;
  views.reserve(count);
  bool all_known = rtvs != nullptr || count == 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint64_t id = rtvs
        ? ActiveViewIncarnationLocked(trace->device, rtvs[i]) : 0;
    views.push_back(id);
    if (rtvs && rtvs[i].handle != 0 && id == 0) all_known = false;
  }
  const std::uint64_t depth_id =
      ActiveViewIncarnationLocked(trace->device, dsv);
  if (dsv.handle != 0 && depth_id == 0) all_known = false;
  trace->pass_fingerprint = PassFingerprint(views, depth_id);
  std::size_t pass_hash = static_cast<std::size_t>(trace->pass_fingerprint);
  wuwa_tfr::TraceHashCombine(pass_hash, pass_metadata);
  trace->pass_fingerprint = static_cast<std::uint64_t>(pass_hash);
  trace->pass_observed = all_known;
}

void OnBindTraceRenderTargets(command_list* cmd_list, std::uint32_t count,
    const resource_view* rtvs, resource_view dsv) {
  SetTracePass(cmd_list, count, rtvs, dsv, 1);
}

bool OnBeginTraceRenderPass(command_list* cmd_list, std::uint32_t count,
    const render_pass_render_target_desc* rts,
    const render_pass_depth_stencil_desc* ds, render_pass_flags flags) {
  std::vector<resource_view> views;
  views.reserve(count);
  std::uint64_t metadata = kTraceFnvOffset;
  TraceHashValue(metadata, std::uint32_t{2});
  TraceHashValue(metadata, flags);
  for (std::uint32_t i = 0; i < count; ++i) {
    views.push_back(rts ? rts[i].view : resource_view{});
    if (rts) {
      TraceHashValue(metadata, rts[i].load_op);
      TraceHashValue(metadata, rts[i].store_op);
      TraceHashAppend(metadata, rts[i].clear_color,
          sizeof(rts[i].clear_color));
    }
  }
  if (ds) {
    TraceHashValue(metadata, ds->depth_load_op);
    TraceHashValue(metadata, ds->depth_store_op);
    TraceHashValue(metadata, ds->stencil_load_op);
    TraceHashValue(metadata, ds->stencil_store_op);
    TraceHashValue(metadata, ds->clear_depth);
    TraceHashValue(metadata, ds->clear_stencil);
  }
  SetTracePass(cmd_list, count, views.data(), ds ? ds->view : resource_view{},
      metadata);
  return false;
}

bool OnEndTraceRenderPass(command_list* cmd_list) {
  if (cmd_list) {
    auto* trace = cmd_list->get_private_data<CommandListTrace>();
    if (trace) {
      trace->pass_fingerprint = 0;
      trace->pass_observed = false;
    }
  }
  return false;
}

void OnPushTraceConstants(
    command_list* cmd_list,
    shader_stage stages,
    pipeline_layout layout,
    std::uint32_t layout_param,
    std::uint32_t first,
    std::uint32_t count,
    const void* values) {
  if (!cmd_list || !HasPixelStage(stages) || count == 0 || !values) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  EnsureTraceLayout(*trace, layout.handle);
  const auto* words = static_cast<const std::uint32_t*>(values);
  for (std::uint32_t i = 0; i < count; ++i) {
    const RootConstantKey key{layout.handle, layout_param, first + i};
    const auto [it, inserted] = trace->root_constants.try_emplace(key, words[i]);
    if (!inserted) {
      trace->root_constant_fingerprint ^=
          TraceRootConstantEntryHash(key, it->second);
      it->second = words[i];
    }
    trace->root_constant_fingerprint ^=
        TraceRootConstantEntryHash(key, words[i]);
  }
  trace->observed_bindings |= 0x1;
}

void OnPushTraceDescriptors(
    command_list* cmd_list,
    shader_stage stages,
    pipeline_layout layout,
    std::uint32_t layout_param,
    const descriptor_table_update& update) {
  if (!cmd_list || !HasPixelStage(stages) || update.count == 0 ||
      !update.descriptors ||
      (update.type != descriptor_type::constant_buffer &&
       update.type != descriptor_type::constant_buffer_with_dynamic_offset))
    return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;

  std::lock_guard lock(g_trace_mutex);
  EnsureTraceLayout(*trace, layout.handle);
  TraceHashValue(trace->pushed_cbv_fingerprint, layout.handle);
  TraceHashValue(trace->pushed_cbv_fingerprint, layout_param);
  TraceHashValue(trace->pushed_cbv_fingerprint, update.binding);
  TraceHashValue(trace->pushed_cbv_fingerprint, update.array_offset);
  TraceHashValue(trace->pushed_cbv_fingerprint, update.count);
  TraceHashValue(trace->pushed_cbv_fingerprint, update.type);
  const auto* ranges = static_cast<const buffer_range*>(update.descriptors);
  for (std::uint32_t i = 0; i < update.count; ++i) {
    TraceHashValue(trace->pushed_cbv_fingerprint, ranges[i].buffer.handle);
    TraceHashValue(trace->pushed_cbv_fingerprint, ranges[i].offset);
    TraceHashValue(trace->pushed_cbv_fingerprint, ranges[i].size);
    const RootCbvKey binding_key{
        layout.handle, layout_param, update.binding + i};
    if (ranges[i].buffer.handle == 0) {
      trace->root_cbv_bindings.erase(binding_key);
    } else {
      trace->root_cbv_bindings[binding_key] = RootCbvBinding{
          ranges[i].buffer.handle,
          ActiveResourceIncarnationLocked(trace->device, ranges[i].buffer)
              .incarnation,
          ranges[i].offset, ranges[i].size};
    }
  }
  trace->observed_bindings |= 0x2;
}

void OnBindTraceDescriptorTables(
    command_list* cmd_list,
    shader_stage stages,
    pipeline_layout layout,
    std::uint32_t first,
    std::uint32_t count,
    const descriptor_table* tables,
    std::uint32_t dynamic_offset_count,
    const std::uint32_t* dynamic_offsets) {
  if (!cmd_list || !HasPixelStage(stages) || count == 0 || !tables) return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;

  EnsureTraceLayout(*trace, layout.handle);
  TraceHashValue(trace->descriptor_table_fingerprint, layout.handle);
  TraceHashValue(trace->descriptor_table_fingerprint, first);
  TraceHashValue(trace->descriptor_table_fingerprint, count);
  for (std::uint32_t i = 0; i < count; ++i)
    TraceHashValue(trace->descriptor_table_fingerprint, tables[i].handle);
  TraceHashValue(trace->descriptor_table_fingerprint, dynamic_offset_count);
  if (dynamic_offset_count != 0 && dynamic_offsets)
    TraceHashAppend(trace->descriptor_table_fingerprint, dynamic_offsets,
        static_cast<std::size_t>(dynamic_offset_count) *
            sizeof(std::uint32_t));
  trace->observed_bindings |= 0x4;

  const bool dynamic_offsets_present = dynamic_offset_count != 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    const BoundDescriptorTableKey key{layout.handle, first + i};
    if (tables[i].handle == 0) {
      trace->bound_descriptor_tables.erase(key);
      continue;
    }
    trace->bound_descriptor_tables[key] =
        BoundDescriptorTable{tables[i].handle, dynamic_offsets_present};
  }
}

bool RecordOrSuppressTraceDraw(command_list* cmd_list,
    wuwa_tfr::TraceGeometryKey geometry) {
  if (!cmd_list) return false;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace || trace->bound_pso_incarnation == 0 || !trace->bound_pipeline)
    return false;

  const wuwa_tfr::TraceConcreteDrawKey concrete{
      trace->bound_pso_incarnation, std::move(geometry),
      trace->pass_fingerprint};
  {
    std::lock_guard lock(g_trace_mutex);
    if (wuwa_tfr::TraceGeometryIsSkipEligible(concrete.geometry) &&
        g_manual_test_draws.contains(concrete)) {
      g_manual_test_record_hits.fetch_add(1, std::memory_order_relaxed);
      g_manual_test_suppressed_commands.fetch_add(
          1, std::memory_order_relaxed);
      return true;
    }
  }

  const RecordedTraceDrawKey recorded{
      concrete,
      trace->root_constant_fingerprint,
      trace->pushed_cbv_fingerprint,
      trace->descriptor_table_fingerprint,
      trace->observed_bindings};
  auto draw = trace->recorded_draws.find(recorded);
  if (draw == trace->recorded_draws.end()) {
    if (trace->recorded_draws.size() >= kMaxRecordedDrawsPerCommandList) {
      trace->recorded_draw_capacity_exceeded = true;
      return false;
    }
    draw = trace->recorded_draws.try_emplace(recorded).first;
    draw->second.pipeline = *trace->bound_pipeline;
  }
  ++draw->second.commands;

  SampleFadeControlValuesOnDraw(*trace, concrete, draw->second);

  return false;
}

bool OnTraceDraw(command_list* cmd_list, std::uint32_t vertex_count,
    std::uint32_t instance_count, std::uint32_t first_vertex,
    std::uint32_t first_instance) {
  const auto* trace = cmd_list
      ? cmd_list->get_private_data<CommandListTrace>() : nullptr;
  if (!trace) return false;
  return RecordOrSuppressTraceDraw(cmd_list, MakeTraceGeometry(*trace,
      wuwa_tfr::TraceDrawKind::Direct,
      {vertex_count, instance_count, first_vertex, first_instance, 0}));
}

bool OnTraceDrawIndexed(command_list* cmd_list, std::uint32_t index_count,
    std::uint32_t instance_count, std::uint32_t first_index,
    std::int32_t vertex_offset, std::uint32_t first_instance) {
  const auto* trace = cmd_list
      ? cmd_list->get_private_data<CommandListTrace>() : nullptr;
  if (!trace) return false;
  return RecordOrSuppressTraceDraw(cmd_list, MakeTraceGeometry(*trace,
      wuwa_tfr::TraceDrawKind::Indexed,
      {index_count, instance_count, first_index,
          static_cast<std::uint32_t>(vertex_offset), first_instance}));
}

bool OnTraceDispatchMesh(command_list* cmd_list, std::uint32_t group_x,
    std::uint32_t group_y, std::uint32_t group_z) {
  const auto* trace = cmd_list
      ? cmd_list->get_private_data<CommandListTrace>() : nullptr;
  if (!trace) return false;
  return RecordOrSuppressTraceDraw(cmd_list, MakeTraceGeometry(*trace,
      wuwa_tfr::TraceDrawKind::Mesh, {group_x, group_y, group_z, 0, 0}));
}

bool OnTraceIndirect(command_list* cmd_list, indirect_command type,
    resource argument_buffer, std::uint64_t argument_offset,
    std::uint32_t draw_count, std::uint32_t stride) {
  if (!cmd_list || draw_count == 0) return false;
  const auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return false;
  wuwa_tfr::TraceDrawKind kind;
  if (type == indirect_command::draw)
    kind = wuwa_tfr::TraceDrawKind::IndirectDraw;
  else if (type == indirect_command::draw_indexed)
    kind = wuwa_tfr::TraceDrawKind::IndirectIndexed;
  else if (type == indirect_command::dispatch_mesh)
    kind = wuwa_tfr::TraceDrawKind::IndirectMesh;
  else
    return false;
  auto geometry = MakeTraceGeometry(*trace, kind, {});
  geometry.observations |= wuwa_tfr::TraceIndirectArgumentsUnknown;
  geometry.indirect_offset = argument_offset;
  geometry.indirect_declared_count = draw_count;
  geometry.indirect_stride = stride;
  {
    std::lock_guard lock(g_trace_mutex);
    geometry.indirect_resource_incarnation =
        ActiveResourceIncarnationLocked(
            trace->device, argument_buffer).incarnation;
  }
  return RecordOrSuppressTraceDraw(cmd_list, std::move(geometry));
}

void OnExecuteSecondaryTrace(
    command_list* primary,
    command_list* secondary) {
  if (!primary || !secondary) return;
  auto* primary_trace = primary->get_private_data<CommandListTrace>();
  const auto* secondary_trace =
      secondary->get_private_data<CommandListTrace>();
  if (!primary_trace || !secondary_trace ||
      primary_trace->device != secondary_trace->device)
    return;
  primary_trace->recorded_draw_capacity_exceeded |=
      secondary_trace->recorded_draw_capacity_exceeded;
  for (const auto& [secondary_key, secondary_draw] :
       secondary_trace->recorded_draws) {
    auto draw_key = secondary_key;
    draw_key.concrete = wuwa_tfr::WithInheritedTracePass(
        std::move(draw_key.concrete), primary_trace->pass_fingerprint,
        primary_trace->pass_observed);
    auto primary_draw = primary_trace->recorded_draws.find(draw_key);
    if (primary_draw == primary_trace->recorded_draws.end()) {
      if (primary_trace->recorded_draws.size() >=
          kMaxRecordedDrawsPerCommandList) {
        primary_trace->recorded_draw_capacity_exceeded = true;
        continue;
      }
      primary_draw = primary_trace->recorded_draws.try_emplace(draw_key).first;
      primary_draw->second.pipeline = secondary_draw.pipeline;
    }
    primary_draw->second.commands += secondary_draw.commands;

    for (auto pending : secondary_draw.pending_fade_observations) {
      pending.key.route = draw_key.concrete;
      primary_draw->second.pending_fade_observations.push_back(
          std::move(pending));
    }
    for (auto pending : secondary_draw.pending_fade_snapshots) {
      pending.key.route = draw_key.concrete;
      primary_draw->second.pending_fade_snapshots.push_back(
          std::move(pending));
    }
  }
}

void OnExecuteTrace(command_queue*, command_list* cmd_list) {
  const std::uint64_t token =
      g_trace_token.load(std::memory_order_acquire);
  if (token == 0 || !cmd_list) return;
  const auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace || (trace->recorded_draws.empty() &&
                    !trace->recorded_draw_capacity_exceeded))
    return;

  std::lock_guard lock(g_trace_mutex);
  if (g_trace_token.load(std::memory_order_relaxed) != token) return;
  if (trace->recorded_draw_capacity_exceeded)
    g_concrete_trace_capacity_exceeded = true;
  const std::uint64_t frame =
      g_trace_frame_id.load(std::memory_order_relaxed);
  const std::size_t window_index =
      TraceWindowIndex(TraceWindowFromToken(token));
  const std::uint64_t submission_serial = ++g_trace_submission_serial;
  std::unordered_set<wuwa_tfr::TraceConcreteDrawKey,
      wuwa_tfr::TraceConcreteDrawKeyHash> submitted_concrete;
  submitted_concrete.reserve(trace->recorded_draws.size());
  for (const auto& [draw_key, draw] : trace->recorded_draws) {
    const auto& pipeline_info = draw.pipeline;
    auto concrete = g_concrete_trace.find(draw_key.concrete);
    if (concrete == g_concrete_trace.end()) {
      if (g_concrete_trace.size() < kMaxConcreteTraceRecords) {
        concrete = g_concrete_trace.try_emplace(draw_key.concrete).first;
        concrete->second.pipeline = pipeline_info;
      } else {
        g_concrete_trace_capacity_exceeded = true;
      }
    }
    if (concrete != g_concrete_trace.end()) {
      const bool first_in_submission =
          submitted_concrete.insert(draw_key.concrete).second;
      wuwa_tfr::AccumulateTraceSubmission(
          static_cast<ConcreteSubmissionRecord&>(concrete->second),
          window_index, draw.commands, first_in_submission, frame);
      concrete->second.last_submission_serial = submission_serial;
    }
    auto& record = g_trace_shaders[pipeline_info.shader_hash];
    auto& metrics = record.windows[window_index];
    metrics.draws += draw.commands;
    if (metrics.last_frame != frame) {
      metrics.last_frame = frame;
      ++metrics.active_frames;
    }
    metrics.pso_contexts.insert(pipeline_info.context_hash);
    if ((draw_key.observed_bindings & 0x1) != 0)
      metrics.root_constant_fingerprints.insert(draw_key.root_constants);
    if ((draw_key.observed_bindings & 0x2) != 0)
      metrics.pushed_cbv_fingerprints.insert(draw_key.pushed_cbvs);
    if ((draw_key.observed_bindings & 0x4) != 0)
      metrics.descriptor_table_fingerprints.insert(
          draw_key.descriptor_tables);
    record.any_rt0_blend |= pipeline_info.rt0_blend;
    record.any_alpha_to_coverage |= pipeline_info.alpha_to_coverage;
    record.any_depth_test |= pipeline_info.depth_test;
    record.any_depth_write |= pipeline_info.depth_write;
    record.max_render_target_count = std::max(
        record.max_render_target_count,
        pipeline_info.render_target_count);
    record.max_sample_count =
        std::max(record.max_sample_count, pipeline_info.sample_count);
  }
}

void OnTracePresent(
    command_queue*, swapchain* presented_swapchain, const rect*, const rect*,
    std::uint32_t, const rect*) {
  const std::uint64_t token =
      g_trace_token.load(std::memory_order_acquire);
  if (token == 0) return;

  std::lock_guard lock(g_trace_mutex);
  if (g_trace_token.load(std::memory_order_relaxed) != token) return;
  const std::uintptr_t swapchain_id =
      reinterpret_cast<std::uintptr_t>(presented_swapchain);
  if (g_trace_swapchain == 0)
    g_trace_swapchain = swapchain_id;
  else if (g_trace_swapchain != swapchain_id)
    return;
  g_trace_frame_id.fetch_add(1, std::memory_order_relaxed);
  const TraceWindow window = TraceWindowFromToken(token);
  const std::uint32_t remaining =
      g_trace_frames_remaining.load(std::memory_order_relaxed);
  if (remaining == 0) return;
  g_trace_frames_remaining.store(remaining - 1, std::memory_order_relaxed);

  const std::size_t window_index = TraceWindowIndex(window);
  g_trace_captured_frames[window_index].fetch_add(
      1, std::memory_order_relaxed);

  if (remaining == 1) {
    g_trace_capture_complete[window_index] = true;
    g_trace_token.store(0, std::memory_order_release);
  }
}

void ResetTraceWindowLocked(TraceWindow window) {
  const std::size_t window_index = TraceWindowIndex(window);
  for (auto& [hash, record] : g_trace_shaders) {
    (void)hash;
    record.windows[window_index] = {};
  }
  for (auto& [key, record] : g_concrete_trace) {
    (void)key;
    record.windows[window_index] = {};
  }
  g_trace_captured_frames[window_index].store(
      0, std::memory_order_relaxed);
  g_trace_capture_complete[window_index] = false;
}

void SetPinnedDrawRouteLocked(
    std::optional<wuwa_tfr::TraceDrawRouteKey> route) {
  g_pinned_draw_route = std::move(route);
}

void ResetShaderFamilySkipAccountingLocked() {
  g_shader_family_skip_hashes.clear();
  g_shader_family_skip_requested_rows = 0;
  g_shader_family_skip_requested_psos = 0;
}

void StartTraceWindow(TraceWindow window) {
  const auto frames = static_cast<std::uint32_t>(
      std::clamp(g_trace_window_length, 30, 600));
  std::lock_guard lock(g_trace_mutex);
  g_manual_test_draws.clear();
  ResetShaderFamilySkipAccountingLocked();
  g_filtered_concrete_rows.clear();
  g_trace_investigation_view =
      wuwa_tfr::TraceInvestigationView::NormalPartialNoDiscard;
  g_trace_candidate_range_initialized = false;
  g_manual_test_record_hits.store(0, std::memory_order_relaxed);
  g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
  if (window == TraceWindow::PartialFade &&
      !g_trace_capture_complete[TraceWindowIndex(TraceWindow::Normal)]) {
    g_trace_ui_status = "capture a complete normal window first";
    return;
  }
  if (window == TraceWindow::FullFade &&
      !g_trace_capture_complete[TraceWindowIndex(TraceWindow::PartialFade)]) {
    g_trace_ui_status = "capture a complete partial-fade window first";
    return;
  }
  g_trace_token.store(0, std::memory_order_release);
  g_trace_frames_remaining.store(0, std::memory_order_relaxed);
  if (window == TraceWindow::Normal) {
    ResetLifecycleAmbiguityDiagnosticsLocked();
    g_trace_shaders.clear();
    g_concrete_trace.clear();
    g_concrete_trace_capacity_exceeded = false;
    for (auto& captured : g_trace_captured_frames)
      captured.store(0, std::memory_order_relaxed);
    g_trace_capture_complete.fill(false);
    g_trace_swapchain = 0;
  } else if (window == TraceWindow::PartialFade) {
    ResetTraceWindowLocked(TraceWindow::PartialFade);
    ResetTraceWindowLocked(TraceWindow::FullFade);
  } else {
    ResetTraceWindowLocked(TraceWindow::FullFade);
  }
  ++g_trace_generation;
  g_trace_frames_remaining.store(frames, std::memory_order_relaxed);
  g_trace_token.store(
      MakeTraceToken(g_trace_generation, window),
      std::memory_order_release);
  if (window == TraceWindow::Normal)
    g_trace_ui_status = "capturing normal window";
  else if (window == TraceWindow::PartialFade)
    g_trace_ui_status = "capturing partial-fade window";
  else
    g_trace_ui_status = "capturing full-fade window";
}

void ClearTraceComparison() {
  std::lock_guard lock(g_trace_mutex);
  g_manual_test_draws.clear();
  ResetShaderFamilySkipAccountingLocked();
  g_pinned_draw_route.reset();
  g_manual_test_record_hits.store(0, std::memory_order_relaxed);
  g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
  g_trace_token.store(0, std::memory_order_release);
  g_trace_frames_remaining.store(0, std::memory_order_relaxed);
  ++g_trace_generation;
  g_trace_shaders.clear();
  g_concrete_trace.clear();
  g_filtered_concrete_rows.clear();
  g_trace_investigation_view =
      wuwa_tfr::TraceInvestigationView::NormalPartialNoDiscard;
  g_trace_candidate_range_initialized = false;
  ResetLifecycleAmbiguityDiagnosticsLocked();
  g_concrete_trace_capacity_exceeded = false;
  for (auto& frames : g_trace_captured_frames)
    frames.store(0, std::memory_order_relaxed);
  g_trace_capture_complete.fill(false);
  g_trace_swapchain = 0;
  g_trace_ui_status = "comparison cleared";
}

std::vector<TraceSnapshotRow> TraceSnapshot(
    bool include_unobserved_dither) {
  std::array<std::uint32_t, kTraceWindowCount> captured_frames{};
  std::vector<TraceSnapshotRow> rows;
  {
    std::lock_guard lock(g_trace_mutex);
    for (std::size_t i = 0; i < kTraceWindowCount; ++i)
      captured_frames[i] =
          g_trace_captured_frames[i].load(std::memory_order_relaxed);
    rows.reserve(g_trace_shaders.size());
    for (const auto& [shader_hash, record] : g_trace_shaders) {
      TraceSnapshotRow row;
      row.shader_hash = shader_hash;
      bool observed = false;
      for (std::size_t i = 0; i < kTraceWindowCount; ++i) {
        const auto& metrics = record.windows[i];
        row.draws[i] = metrics.draws;
        row.active_frames[i] = metrics.active_frames;
        row.pso_context_count[i] = metrics.pso_contexts.size();
        row.root_constant_state_count[i] =
            metrics.root_constant_fingerprints.size();
        row.pushed_cbv_state_count[i] =
            metrics.pushed_cbv_fingerprints.size();
        row.descriptor_table_state_count[i] =
            metrics.descriptor_table_fingerprints.size();
        if (captured_frames[i] != 0)
          row.draws_per_frame[i] =
              static_cast<double>(metrics.draws) / captured_frames[i];
        observed |= metrics.draws != 0;
      }
      if (!observed) continue;
      row.partial_minus_normal =
          row.draws_per_frame[1] - row.draws_per_frame[0];
      row.full_minus_partial =
          row.draws_per_frame[2] - row.draws_per_frame[1];
      row.comparison_score = std::fabs(row.partial_minus_normal) +
          std::fabs(row.full_minus_partial);
      row.any_rt0_blend = record.any_rt0_blend;
      row.any_alpha_to_coverage = record.any_alpha_to_coverage;
      row.any_depth_test = record.any_depth_test;
      row.any_depth_write = record.any_depth_write;
      row.max_render_target_count = record.max_render_target_count;
      row.max_sample_count = record.max_sample_count;
      row.normal_partial_pso_changed =
          record.windows[0].pso_contexts != record.windows[1].pso_contexts;
      row.partial_full_pso_changed =
          record.windows[1].pso_contexts != record.windows[2].pso_contexts;
      row.normal_partial_root_constants_changed =
          record.windows[0].root_constant_fingerprints !=
          record.windows[1].root_constant_fingerprints;
      row.partial_full_root_constants_changed =
          record.windows[1].root_constant_fingerprints !=
          record.windows[2].root_constant_fingerprints;
      row.normal_partial_pushed_cbvs_changed =
          record.windows[0].pushed_cbv_fingerprints !=
          record.windows[1].pushed_cbv_fingerprints;
      row.partial_full_pushed_cbvs_changed =
          record.windows[1].pushed_cbv_fingerprints !=
          record.windows[2].pushed_cbv_fingerprints;
      row.normal_partial_descriptor_tables_changed =
          record.windows[0].descriptor_table_fingerprints !=
          record.windows[1].descriptor_table_fingerprints;
      row.partial_full_descriptor_tables_changed =
          record.windows[1].descriptor_table_fingerprints !=
          record.windows[2].descriptor_table_fingerprints;
      row.state_change_count =
          static_cast<std::uint32_t>(row.normal_partial_pso_changed) +
          static_cast<std::uint32_t>(row.partial_full_pso_changed) +
          static_cast<std::uint32_t>(
              row.normal_partial_root_constants_changed) +
          static_cast<std::uint32_t>(
              row.partial_full_root_constants_changed) +
          static_cast<std::uint32_t>(row.normal_partial_pushed_cbvs_changed) +
          static_cast<std::uint32_t>(row.partial_full_pushed_cbvs_changed) +
          static_cast<std::uint32_t>(
              row.normal_partial_descriptor_tables_changed) +
          static_cast<std::uint32_t>(
              row.partial_full_descriptor_tables_changed);
      rows.push_back(row);
    }
  }
  {
    std::lock_guard lock(g_inspection_mutex);
    std::unordered_set<std::uint64_t> row_hashes;
    row_hashes.reserve(rows.size());
    for (auto& row : rows) {
      row_hashes.insert(row.shader_hash);
      const auto inspection = g_inspections.find(row.shader_hash);
      if (inspection == g_inspections.end()) continue;
      row.discard_calls = inspection->second.dither.discard_calls;
      row.strict_spatial_dither_discards =
          inspection->second.dither.strict_spatial_dither_discards;
      row.strict_spatial_dither =
          inspection->second.dither.classification ==
          wuwa_tfr::SpatialDitherClassification::StrictSpatialDither;
      row.ambiguous_spatial_dither =
          inspection->second.dither.classification ==
          wuwa_tfr::SpatialDitherClassification::
              AmbiguousStrictSpatialDither;
    }
    if (include_unobserved_dither) {
      for (const auto& [hash, inspection] : g_inspections) {
        if (row_hashes.contains(hash) ||
            inspection.dither.classification !=
                wuwa_tfr::SpatialDitherClassification::StrictSpatialDither)
          continue;
        TraceSnapshotRow row;
        row.shader_hash = hash;
        row.discard_calls = inspection.dither.discard_calls;
        row.strict_spatial_dither_discards =
            inspection.dither.strict_spatial_dither_discards;
        row.strict_spatial_dither = true;
        rows.push_back(row);
      }
    }
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    if (left.strict_spatial_dither != right.strict_spatial_dither)
      return left.strict_spatial_dither;
    if (left.state_change_count != right.state_change_count)
      return left.state_change_count > right.state_change_count;
    return left.comparison_score > right.comparison_score;
  });
  return rows;
}

std::optional<ConcreteTraceRow> MakeConcreteTraceRowLocked(
    const wuwa_tfr::TraceConcreteDrawKey& key,
    const ConcreteTraceRecord& record) {
  ConcreteTraceRow row;
  row.key = key;
  row.pipeline = record.pipeline;
  const auto live = g_trace_pipelines.find(
      {row.pipeline.device, row.pipeline.application_pipeline});
  row.pipeline_live = live != g_trace_pipelines.end() &&
      live->second.incarnation_id == key.pso_incarnation;
  row.windows = record.windows;
  row.last_submission_serial = record.last_submission_serial;
  row.geometry_fingerprint = static_cast<std::uint64_t>(
      wuwa_tfr::TraceGeometryKeyHash{}(key.geometry));
  row.concrete = wuwa_tfr::TraceGeometryIsConcrete(key.geometry);
  row.skip_eligible = wuwa_tfr::TraceGeometryIsSkipEligible(key.geometry);
  return row;
}

std::vector<ConcreteTraceRow> ConcreteTraceSnapshot() {
  std::vector<ConcreteTraceRow> rows;
  std::lock_guard lock(g_trace_mutex);
  rows.reserve(g_concrete_trace.size());
  for (const auto& [key, record] : g_concrete_trace) {
    auto row = MakeConcreteTraceRowLocked(key, record);
    if (row) rows.push_back(std::move(*row));
  }
  std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
    return left.last_submission_serial > right.last_submission_serial;
  });
  return rows;
}

bool GenerateFilteredConcreteRows() {
  bool capacity_exceeded = false;
  {
    std::lock_guard lock(g_trace_mutex);
    if (g_trace_token.load(std::memory_order_relaxed) != 0 ||
        !std::all_of(g_trace_capture_complete.begin(),
            g_trace_capture_complete.end(), [](bool complete) {
              return complete;
            })) {
      g_trace_ui_status = "finish all three windows before generating the list";
      return false;
    }
    capacity_exceeded = g_concrete_trace_capacity_exceeded ||
        g_trace_identity_capacity_exceeded;
  }

  auto rows = ConcreteTraceSnapshot();
  const auto shader_rows = TraceSnapshot();
  std::unordered_map<std::uint64_t, std::uint32_t> shader_reasons;
  std::unordered_map<std::uint64_t, bool> shader_ambiguous_dither;
  for (const auto& shader : shader_rows) {
    std::uint32_t reasons = 0;
    if (shader.state_change_count != 0)
      reasons |= TraceBindingStateChanged;
    if (shader.discard_calls != 0)
      reasons |= TraceDiscardClue;
    if (shader.strict_spatial_dither || shader.ambiguous_spatial_dither)
      reasons |= TraceDitherClue;
    if (shader.strict_spatial_dither)
      reasons |= TraceStrictSpatialDitherClue;
    if (shader.any_rt0_blend || shader.any_alpha_to_coverage)
      reasons |= TraceBlendCoverageClue;
    shader_reasons.emplace(shader.shader_hash, reasons);
    shader_ambiguous_dither.emplace(
        shader.shader_hash, shader.ambiguous_spatial_dither);
  }

  struct RouteAggregate {
    std::array<std::uint64_t, kTraceWindowCount> submissions{};
    std::array<std::unordered_set<std::uint64_t>, kTraceWindowCount> psos;
    std::array<std::unordered_set<std::uint64_t>, kTraceWindowCount> shaders;
    std::vector<std::size_t> row_indices;
    std::uint32_t reasons = 0;
    bool exact_pass_observed = true;
    bool dynamic_contents_heuristic = false;
  };
  std::unordered_map<wuwa_tfr::TraceDrawRouteKey, RouteAggregate,
      wuwa_tfr::TraceDrawRouteKeyHash> routes;

  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    auto& row = rows[row_index];
    auto& route = routes[wuwa_tfr::MakeTraceDrawRoute(row.key)];
    route.row_indices.push_back(row_index);
    route.exact_pass_observed &=
        (row.key.geometry.observations & wuwa_tfr::TraceObservedPass) != 0;
    route.dynamic_contents_heuristic |=
        wuwa_tfr::TraceGeometryUsesDynamicContents(row.key.geometry);
    if (const auto clue = shader_reasons.find(row.pipeline.shader_hash);
        clue != shader_reasons.end()) {
      route.reasons |= clue->second;
      row.investigation_has_discard =
          (clue->second & TraceDiscardClue) != 0;
      row.investigation_strict_spatial_dither =
          (clue->second & TraceStrictSpatialDitherClue) != 0;
      if (const auto ambiguous = shader_ambiguous_dither.find(
              row.pipeline.shader_hash);
          ambiguous != shader_ambiguous_dither.end())
        row.investigation_ambiguous_spatial_dither = ambiguous->second;
    }
    for (std::size_t window = 0; window < kTraceWindowCount; ++window) {
      const auto submissions = row.windows[window].command_list_submissions;
      route.submissions[window] += submissions;
      if (submissions != 0) {
        route.psos[window].insert(row.key.pso_incarnation);
        route.shaders[window].insert(row.pipeline.shader_hash);
      }
    }
  }

  std::vector<ConcreteTraceRow> frozen;
  for (auto& [route_key, route] : routes) {
    (void)route_key;
    const std::array<bool, kTraceWindowCount> present{
        route.submissions[0] != 0,
        route.submissions[1] != 0,
        route.submissions[2] != 0};
    if (present[0] != present[1] || present[1] != present[2])
      route.reasons |= TracePresenceChanged;
    if (route.psos[0] != route.psos[1] ||
        route.psos[1] != route.psos[2])
      route.reasons |= TracePsoChanged;
    if (route.shaders[0] != route.shaders[1] ||
        route.shaders[1] != route.shaders[2])
      route.reasons |= TraceShaderChanged;
    const auto conclusion = wuwa_tfr::ClassifyTraceRoute(
        present[0] || present[1], present[2], route.exact_pass_observed,
        route.dynamic_contents_heuristic, capacity_exceeded);
    for (const auto row_index : route.row_indices) {
      auto row = std::move(rows[row_index]);
      row.filter_reasons = route.reasons;
      row.exact_pass_observed = route.exact_pass_observed;
      row.dynamic_contents_heuristic =
          route.dynamic_contents_heuristic;
      row.conclusion = conclusion;
      frozen.push_back(std::move(row));
    }
  }

  std::sort(frozen.begin(), frozen.end(), [](const auto& left,
                                               const auto& right) {
    if (left.conclusion != right.conclusion)
      return left.conclusion < right.conclusion;
    if (left.filter_reasons != right.filter_reasons)
      return left.filter_reasons > right.filter_reasons;
    return left.last_submission_serial > right.last_submission_serial;
  });
  {
    std::lock_guard lock(g_trace_mutex);
    g_filtered_concrete_rows = std::move(frozen);
    g_trace_investigation_view =
        wuwa_tfr::TraceInvestigationView::NormalPartialNoDiscard;
    g_trace_candidate_range_initialized = false;
    g_manual_test_draws.clear();
    ResetShaderFamilySkipAccountingLocked();
    g_manual_test_record_hits.store(0, std::memory_order_relaxed);
    g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
    g_trace_ui_status = "generated frozen Draw list";
  }
  return true;
}

std::vector<ShaderFamilyGroup> BuildShaderFamilyGroups(
    const std::vector<ConcreteTraceRow>& frozen_rows) {
  std::unordered_map<std::uint64_t, std::size_t> group_positions;
  std::vector<ShaderFamilyGroup> groups;
  group_positions.reserve(frozen_rows.size());
  groups.reserve(frozen_rows.size());
  for (std::size_t row_index = 0; row_index < frozen_rows.size(); ++row_index) {
    const auto& row = frozen_rows[row_index];
    const auto [position, inserted] = group_positions.emplace(
        row.pipeline.shader_hash, groups.size());
    if (inserted) groups.push_back({.shader_hash = row.pipeline.shader_hash});
    auto& group = groups[position->second];
    group.row_indices.push_back(row_index);
    if (row.concrete) ++group.concrete_row_count;
    group.application_psos.insert(
        {row.pipeline.device, row.pipeline.application_pipeline});
    group.routes.insert(wuwa_tfr::MakeTraceDrawRoute(row.key));
    for (std::size_t window = 0; window < kTraceWindowCount; ++window)
      group.submissions[window] +=
          row.windows[window].command_list_submissions;
    group.all_fade_transition_candidates &=
        wuwa_tfr::TraceFadeTransitionCandidate(row.windows);
    group.any_discard |= row.investigation_has_discard;
    group.any_strict_spatial_dither |=
        row.investigation_strict_spatial_dither;
    group.any_ambiguous_spatial_dither |=
        row.investigation_ambiguous_spatial_dither;
    group.any_blend |= row.pipeline.rt0_blend;
    group.any_alpha_to_coverage |= row.pipeline.alpha_to_coverage;
  }
  std::sort(groups.begin(), groups.end(), [](const auto& left,
                                              const auto& right) {
    if (left.all_fade_transition_candidates !=
        right.all_fade_transition_candidates)
      return left.all_fade_transition_candidates;
    if (left.row_indices.size() != right.row_indices.size())
      return left.row_indices.size() > right.row_indices.size();
    return left.shader_hash < right.shader_hash;
  });
  return groups;
}

void SetShaderFamilySkipLocked(
    const std::vector<ConcreteTraceRow>& frozen_rows,
    const ShaderFamilyGroup& group, bool selected) {
  if (selected)
    g_shader_family_skip_hashes.insert(group.shader_hash);
  else
    g_shader_family_skip_hashes.erase(group.shader_hash);
  for (const auto row_index : group.row_indices) {
    if (row_index >= frozen_rows.size()) continue;
    const auto& row = frozen_rows[row_index];
    if (!selected) {
      g_manual_test_draws.erase(row.key);
      continue;
    }
    if (!row.skip_eligible) continue;
    const TracePipelineKey pipeline_key{
        row.pipeline.device, row.pipeline.application_pipeline};
    const auto live = g_trace_pipelines.find(pipeline_key);
    if (live != g_trace_pipelines.end() &&
        live->second.incarnation_id == row.key.pso_incarnation)
      g_manual_test_draws.insert(row.key);
  }
  std::unordered_set<std::uint64_t> requested_psos;
  std::size_t requested_rows = 0;
  for (const auto& row : frozen_rows) {
    if (g_shader_family_skip_hashes.contains(row.pipeline.shader_hash) &&
        g_manual_test_draws.contains(row.key)) {
      ++requested_rows;
      requested_psos.insert(row.key.pso_incarnation);
    }
  }
  g_shader_family_skip_requested_rows = requested_rows;
  g_shader_family_skip_requested_psos = requested_psos.size();
  g_manual_test_record_hits.store(0, std::memory_order_relaxed);
  g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
}

}  // namespace wuwa_tfr::dev

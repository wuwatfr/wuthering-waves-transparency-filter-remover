// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// ReShade event handlers for the Dev-only runtime differential trace (PSO,
// resource, resource-view, command-list, binding, and draw/dispatch/present
// tracking), plus the window-management and snapshot/list-generation
// functions the overlay (dev/dev_overlay.*) drives.
//
// This is observation only: it never creates, patches, or binds a
// replacement pipeline. Fade-primitive replacement is owned entirely by
// g_dev_antifade_runtime (dev/dev_runtime.hpp), a separate, independent
// instance of the same runtime class Production uses.

#pragma once

#include <cstdint>
#include <vector>

#include <reshade.hpp>

#include "dev/trace/trace_state.hpp"

namespace wuwa_tfr::dev {

void OnInitTracePipeline(
    reshade::api::device* owner,
    reshade::api::pipeline_layout layout,
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline handle);
void OnDestroyTracePipeline(
    reshade::api::device* owner, reshade::api::pipeline handle);

void OnInitTraceResource(
    reshade::api::device* owner, const reshade::api::resource_desc& desc,
    const reshade::api::subresource_data*,
    reshade::api::resource_usage initial_usage, reshade::api::resource handle);
void OnDestroyTraceResource(
    reshade::api::device* owner, reshade::api::resource handle);
void OnInitTraceResourceView(
    reshade::api::device* owner, reshade::api::resource resource_handle,
    reshade::api::resource_usage usage,
    const reshade::api::resource_view_desc& desc,
    reshade::api::resource_view view);
void OnDestroyTraceResourceView(
    reshade::api::device* owner, reshade::api::resource_view view);

void OnInitTraceCommandList(reshade::api::command_list* cmd_list);
void OnDestroyTraceCommandList(reshade::api::command_list* cmd_list);
void OnResetTraceCommandList(reshade::api::command_list* cmd_list);

void OnBindTracePipeline(
    reshade::api::command_list* cmd_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline handle);
void OnBindTracePipelineStates(
    reshade::api::command_list* cmd_list, std::uint32_t count,
    const reshade::api::dynamic_state* states, const std::uint32_t* values);
void OnBindTraceVertexBuffers(
    reshade::api::command_list* cmd_list, std::uint32_t first,
    std::uint32_t count, const reshade::api::resource* buffers,
    const std::uint64_t* offsets, const std::uint32_t* strides);
void OnBindTraceIndexBuffer(
    reshade::api::command_list* cmd_list, reshade::api::resource buffer,
    std::uint64_t offset, std::uint32_t index_size);
void OnBindTraceRenderTargets(
    reshade::api::command_list* cmd_list, std::uint32_t count,
    const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv);
bool OnBeginTraceRenderPass(
    reshade::api::command_list* cmd_list, std::uint32_t count,
    const reshade::api::render_pass_render_target_desc* rts,
    const reshade::api::render_pass_depth_stencil_desc* ds,
    reshade::api::render_pass_flags flags);
bool OnEndTraceRenderPass(reshade::api::command_list* cmd_list);

void OnPushTraceConstants(
    reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout, std::uint32_t layout_param,
    std::uint32_t first, std::uint32_t count, const void* values);
void OnPushTraceDescriptors(
    reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout, std::uint32_t layout_param,
    const reshade::api::descriptor_table_update& update);
void OnBindTraceDescriptorTables(
    reshade::api::command_list* cmd_list, reshade::api::shader_stage stages,
    reshade::api::pipeline_layout layout, std::uint32_t first,
    std::uint32_t count, const reshade::api::descriptor_table* tables,
    std::uint32_t dynamic_offset_count,
    const std::uint32_t* dynamic_offsets);

bool OnTraceDraw(
    reshade::api::command_list* cmd_list, std::uint32_t vertex_count,
    std::uint32_t instance_count, std::uint32_t first_vertex,
    std::uint32_t first_instance);
bool OnTraceDrawIndexed(
    reshade::api::command_list* cmd_list, std::uint32_t index_count,
    std::uint32_t instance_count, std::uint32_t first_index,
    std::int32_t vertex_offset, std::uint32_t first_instance);
bool OnTraceDispatchMesh(
    reshade::api::command_list* cmd_list, std::uint32_t group_x,
    std::uint32_t group_y, std::uint32_t group_z);
bool OnTraceIndirect(
    reshade::api::command_list* cmd_list, reshade::api::indirect_command type,
    reshade::api::resource argument_buffer, std::uint64_t argument_offset,
    std::uint32_t draw_count, std::uint32_t stride);
void OnExecuteSecondaryTrace(
    reshade::api::command_list* primary, reshade::api::command_list* secondary);
void OnExecuteTrace(
    reshade::api::command_queue*, reshade::api::command_list* cmd_list);
void OnTracePresent(
    reshade::api::command_queue*, reshade::api::swapchain* presented_swapchain,
    const reshade::api::rect*, const reshade::api::rect*, std::uint32_t,
    const reshade::api::rect*);

void SetPinnedDrawRouteLocked(
    std::optional<wuwa_tfr::TraceDrawRouteKey> route);
void ResetShaderFamilySkipAccountingLocked();
void StartTraceWindow(TraceWindow window);
void ClearTraceComparison();

bool IsNormalOnlyRimSkipRow(const ConcreteTraceRow& row) noexcept;

std::vector<TraceSnapshotRow> TraceSnapshot(
    bool include_unobserved_dither = false);
std::vector<ConcreteTraceRow> ConcreteTraceSnapshot();
bool GenerateFilteredConcreteRows();
std::vector<ShaderFamilyGroup> BuildShaderFamilyGroups(
    const std::vector<ConcreteTraceRow>& frozen_rows);
void SetShaderFamilySkipLocked(
    const std::vector<ConcreteTraceRow>& frozen_rows,
    const ShaderFamilyGroup& group, bool selected);

}  // namespace wuwa_tfr::dev

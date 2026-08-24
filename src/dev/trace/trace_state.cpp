// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/trace/trace_state.hpp"

#include <algorithm>
#include <cstring>

#include "dev/diagnostics/dev_diagnostics.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

std::mutex g_trace_mutex;
wuwa_tfr::TraceIncarnationIndex<TracePsoIdentity> g_trace_pso_incarnations;
wuwa_tfr::TraceIncarnationIndex<std::uint64_t> g_trace_view_incarnations;
TracePsoAmbiguityDiagnostics g_trace_pso_lifecycle_ambiguities;
TraceResourceAmbiguityDiagnostics g_trace_resource_lifecycle_ambiguities;
std::uint64_t g_trace_lifecycle_event_serial = 0;
std::unordered_map<TracePipelineKey, wuwa_tfr::ExecutionPipelineIdentity, TracePipelineKeyHash>
    g_trace_pipelines;
std::unordered_map<std::uint64_t, ShaderTraceRecord> g_trace_shaders;
std::unordered_map<wuwa_tfr::TraceConcreteDrawKey, ConcreteTraceRecord,
    wuwa_tfr::TraceConcreteDrawKeyHash> g_concrete_trace;
bool g_concrete_trace_capacity_exceeded = false;
bool g_trace_identity_capacity_exceeded = false;
std::uint64_t g_trace_incarnation_prunes = 0;
std::vector<ConcreteTraceRow> g_filtered_concrete_rows;
wuwa_tfr::TraceInvestigationView g_trace_investigation_view =
    wuwa_tfr::TraceInvestigationView::NormalPartialNoDiscard;
wuwa_tfr::TraceCandidateRange g_trace_candidate_range;
bool g_trace_candidate_range_initialized = false;
bool g_shader_family_investigation_mode = false;
std::atomic<std::uint64_t> g_trace_token{0};
std::atomic<std::uint32_t> g_trace_frames_remaining{0};
std::unordered_set<wuwa_tfr::TraceConcreteDrawKey,
    wuwa_tfr::TraceConcreteDrawKeyHash> g_manual_test_draws;
std::unordered_set<std::uint64_t> g_shader_family_skip_hashes;
std::size_t g_shader_family_skip_requested_rows = 0;
std::size_t g_shader_family_skip_requested_psos = 0;
std::optional<wuwa_tfr::TraceDrawRouteKey> g_pinned_draw_route;
std::atomic<std::uint64_t> g_manual_test_record_hits{0};
std::atomic<std::uint64_t> g_manual_test_suppressed_commands{0};
std::uint64_t g_trace_submission_serial = 0;
std::array<std::atomic<std::uint32_t>, kTraceWindowCount>
    g_trace_captured_frames{};
std::atomic<std::uint64_t> g_trace_frame_id{1};
std::uint64_t g_trace_generation = 0;
std::uintptr_t g_trace_swapchain = 0;
std::array<bool, kTraceWindowCount> g_trace_capture_complete{};
int g_trace_window_length = 120;
std::string g_trace_ui_status;

std::size_t TraceWindowIndex(TraceWindow window) noexcept {
  return static_cast<std::size_t>(window) - 1;
}

std::uint64_t MakeTraceToken(
    std::uint64_t generation,
    TraceWindow window) noexcept {
  return (generation << 2) | static_cast<std::uint64_t>(window);
}

TraceWindow TraceWindowFromToken(std::uint64_t token) noexcept {
  return static_cast<TraceWindow>(token & 0x3u);
}

void TraceHashAppend(
    std::uint64_t& hash,
    const void* data,
    std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
}

std::uint64_t TraceRootConstantEntryHash(
    const RootConstantKey& key,
    std::uint32_t value) noexcept {
  std::uint64_t hash = kTraceFnvOffset;
  TraceHashValue(hash, key.layout);
  TraceHashValue(hash, key.parameter);
  TraceHashValue(hash, key.word);
  TraceHashValue(hash, value);
  return hash;
}

void EnsureTraceLayout(
    CommandListTrace& trace,
    std::uint64_t layout) noexcept {
  if (trace.bound_layout == layout) return;
  trace.bound_layout = layout;
  trace.root_constant_fingerprint = 0;
  trace.pushed_cbv_fingerprint = kTraceFnvOffset;
  trace.descriptor_table_fingerprint = kTraceFnvOffset;
  trace.observed_bindings = 0;
  trace.root_constants.clear();
  trace.root_cbv_bindings.clear();
  trace.bound_descriptor_tables.clear();
}

bool HasPixelStage(shader_stage stages) noexcept {
  return (stages & shader_stage::pixel) == shader_stage::pixel;
}

ActiveTraceResource ActiveResourceIncarnationLocked(
    DeviceIdentity device,
    resource handle) {
  if (handle.handle == 0) return {};
  const auto active = FindActiveResourceLifecycle({device, handle.handle});
  return {active.incarnation_id, active.dynamic_contents};
}

std::uint64_t ActiveViewIncarnationLocked(
    DeviceIdentity device,
    resource_view handle) {
  if (handle.handle == 0) return 0;
  const auto* record = g_trace_view_incarnations.FindActive(
      {device, handle.handle});
  return record ? record->id : 0;
}

std::uint64_t PassFingerprint(
    const std::vector<std::uint64_t>& render_targets,
    std::uint64_t depth_stencil) {
  std::size_t hash = std::hash<std::uint64_t>{}(render_targets.size());
  for (const auto target : render_targets)
    wuwa_tfr::TraceHashCombine(hash, target);
  wuwa_tfr::TraceHashCombine(hash, depth_stencil);
  return static_cast<std::uint64_t>(hash);
}

wuwa_tfr::TraceGeometryKey MakeTraceGeometry(
    const CommandListTrace& trace,
    wuwa_tfr::TraceDrawKind kind,
    std::array<std::uint64_t, 5> arguments) {
  wuwa_tfr::TraceGeometryKey geometry;
  geometry.kind = kind;
  geometry.arguments = arguments;
  geometry.topology = trace.primitive_topology;
  if (trace.bound_pso_incarnation != 0)
    geometry.observations |= wuwa_tfr::TraceObservedPso;
  if (trace.topology_observed)
    geometry.observations |= wuwa_tfr::TraceObservedTopology;
  if (trace.vertex_buffers_observed)
    geometry.observations |= wuwa_tfr::TraceObservedVertexBuffers;
  if (trace.index_buffer_observed)
    geometry.observations |= wuwa_tfr::TraceObservedIndexBuffer;
  if (trace.pass_observed)
    geometry.observations |= wuwa_tfr::TraceObservedPass;
  geometry.vertex_buffers.reserve(trace.vertex_buffers.size());
  for (const auto& [slot, binding] : trace.vertex_buffers) {
    (void)slot;
    geometry.vertex_buffers.push_back(binding);
  }
  std::sort(geometry.vertex_buffers.begin(), geometry.vertex_buffers.end(),
      [](const auto& left, const auto& right) {
        return left.slot < right.slot;
      });
  geometry.index_buffer = trace.index_buffer;
  return geometry;
}

const char* TraceDrawKindName(wuwa_tfr::TraceDrawKind kind) noexcept {
  switch (kind) {
    case wuwa_tfr::TraceDrawKind::Direct: return "draw";
    case wuwa_tfr::TraceDrawKind::Indexed: return "indexed";
    case wuwa_tfr::TraceDrawKind::Mesh: return "mesh";
    case wuwa_tfr::TraceDrawKind::IndirectDraw: return "indirect-draw";
    case wuwa_tfr::TraceDrawKind::IndirectIndexed:
      return "indirect-indexed";
    case wuwa_tfr::TraceDrawKind::IndirectMesh: return "indirect-mesh";
  }
  return "unknown";
}

const char* RouteConclusionName(
    wuwa_tfr::TraceRouteConclusion conclusion) noexcept {
  switch (conclusion) {
    case wuwa_tfr::TraceRouteConclusion::SubmittedInFullWindow:
      return "ROUTE_SUBMITTED_IN_FULL_WINDOW";
    case wuwa_tfr::TraceRouteConclusion::NotObservedInFullWindow:
      return "ROUTE_NOT_OBSERVED_IN_FULL_WINDOW";
    case wuwa_tfr::TraceRouteConclusion::Unknown:
      return "ROUTE_WINDOW_COMPARISON_UNKNOWN";
  }
  return "ROUTE_WINDOW_COMPARISON_UNKNOWN";
}

std::string ConcreteFilterReasons(std::uint32_t reasons) {
  std::string text;
  const auto append = [&text](const char* value) {
    if (!text.empty()) text += ',';
    text += value;
  };
  if ((reasons & TracePresenceChanged) != 0) append("presence");
  if ((reasons & TracePsoChanged) != 0) append("pso");
  if ((reasons & TraceShaderChanged) != 0) append("shader");
  if ((reasons & TraceBindingStateChanged) != 0) append("binding-state");
  if ((reasons & TraceDiscardClue) != 0) append("discard");
  if ((reasons & TraceDitherClue) != 0) append("dither");
  if ((reasons & TraceBlendCoverageClue) != 0) append("blend/coverage");
  return text.empty() ? "none" : text;
}

std::string RouteUncertainty(const ConcreteTraceRow& row) {
  std::string text;
  const auto append = [&text](const char* value) {
    if (!text.empty()) text += ',';
    text += value;
  };
  if (!row.exact_pass_observed) append("exact-pass-unobserved");
  if (row.dynamic_contents_heuristic)
    append("dynamic-contents-heuristic");
  return text.empty() ? "none" : text;
}

std::string TraceIdentityText(const TracePsoIdentity& identity) {
  return Hex64(identity.creation_fingerprint) + ":" +
      Hex64(identity.shader_hash) + ":" + Hex64(identity.context_hash);
}

std::string TraceIdentityText(const TraceResourceIdentity& identity) {
  return Hex64(identity.fingerprint) + ":dynamic=" +
      (identity.dynamic_contents ? "1" : "0");
}

void ResetLifecycleAmbiguityDiagnosticsLocked() {
  g_trace_pso_lifecycle_ambiguities.Reset();
  g_trace_resource_lifecycle_ambiguities.Reset();
  g_trace_lifecycle_event_serial = 0;
}

wuwa_tfr::ExecutionPipelineIdentity DescribeTracePipeline(
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    std::uint64_t shader_hash) {
  blend_desc blend{};
  depth_stencil_desc depth{};
  wuwa_tfr::ExecutionPipelineIdentity info;
  info.shader_hash = shader_hash;
  std::uint64_t context_hash = kTraceFnvOffset;

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    const auto& subobject = subobjects[i];
    if (!subobject.data) continue;
    switch (subobject.type) {
      case pipeline_subobject_type::blend_state:
        blend = *static_cast<const blend_desc*>(subobject.data);
        break;
      case pipeline_subobject_type::depth_stencil_state:
        depth = *static_cast<const depth_stencil_desc*>(subobject.data);
        break;
      case pipeline_subobject_type::render_target_formats: {
        info.render_target_count = subobject.count;
        const auto* formats = static_cast<const format*>(subobject.data);
        for (std::uint32_t j = 0; j < subobject.count; ++j)
          TraceHashValue(context_hash, formats[j]);
        break;
      }
      case pipeline_subobject_type::sample_count:
        info.sample_count =
            *static_cast<const std::uint32_t*>(subobject.data);
        break;
      case pipeline_subobject_type::primitive_topology:
        info.primitive_topology = static_cast<std::uint32_t>(
            *static_cast<const primitive_topology*>(subobject.data));
        break;
      default:
        break;
    }
  }

  info.rt0_blend = blend.blend_enable[0];
  info.alpha_to_coverage = blend.alpha_to_coverage_enable;
  info.depth_test = depth.depth_enable;
  info.depth_write = depth.depth_write_mask;
  TraceHashValue(context_hash, info.render_target_count);
  TraceHashValue(context_hash, info.sample_count);
  TraceHashValue(context_hash, info.primitive_topology);
  TraceHashValue(context_hash, blend.alpha_to_coverage_enable);
  for (std::size_t i = 0; i < 8; ++i) {
    TraceHashValue(context_hash, blend.blend_enable[i]);
    TraceHashValue(context_hash, blend.source_color_blend_factor[i]);
    TraceHashValue(context_hash, blend.dest_color_blend_factor[i]);
    TraceHashValue(context_hash, blend.color_blend_op[i]);
    TraceHashValue(context_hash, blend.source_alpha_blend_factor[i]);
    TraceHashValue(context_hash, blend.dest_alpha_blend_factor[i]);
    TraceHashValue(context_hash, blend.alpha_blend_op[i]);
    TraceHashValue(context_hash, blend.render_target_write_mask[i]);
  }
  TraceHashValue(context_hash, depth.depth_enable);
  TraceHashValue(context_hash, depth.depth_write_mask);
  TraceHashValue(context_hash, depth.depth_func);
  TraceHashValue(context_hash, depth.stencil_enable);
  info.context_hash = context_hash;
  return info;
}

}  // namespace wuwa_tfr::dev

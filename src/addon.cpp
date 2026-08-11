// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#ifdef _WIN32
#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp>

#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"
#include "fade_primitive_runtime.hpp"
#if WUWA_TFR_DEVTOOLS
#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"
#include "pipeline_replacement_state.hpp"
#include "target_dither_bypass.hpp"
#include "trace_submission_identity.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WUWA_TFR_DEVTOOLS
#define WUWA_TFR_DEVTOOLS 0
#endif

using namespace reshade::api;

namespace {

using DeviceIdentity = std::uintptr_t;

struct InspectionRecord {
  bool success = false;
  bool dumped = false;
  std::size_t bytecode_size = 0;
#if WUWA_TFR_DEVTOOLS
  wuwa_tfr::SpatialDitherDiagnostic dither;
  wuwa_tfr::FadePrimitiveDiagnostic fade_primitive;
#endif
  std::string error;
};

#if WUWA_TFR_DEVTOOLS
enum class TraceWindow : std::uint8_t {
  None,
  Normal,
  PartialFade,
  FullFade,
};

constexpr std::size_t kTraceWindowCount = 3;
constexpr std::uint64_t kTraceFnvOffset = 14695981039346656037ull;
constexpr std::size_t kMaxConcreteTraceRecords = 16384;
constexpr std::size_t kMaxRecordedDrawsPerCommandList = 8192;
constexpr std::size_t kMaxTrackedPsoIncarnations = 16384;
constexpr std::size_t kMaxTrackedResourceIncarnations = 65536;
constexpr std::size_t kMaxTrackedViewIncarnations = 65536;

std::size_t TraceWindowIndex(TraceWindow window) noexcept {
  return static_cast<std::size_t>(window) - 1;
}

using TracePipelineKey = wuwa_tfr::TraceLiveHandleKey;
using TracePipelineKeyHash = wuwa_tfr::TraceLiveHandleKeyHash;

struct TracePsoIdentity {
  std::uint64_t creation_fingerprint = 0;
  std::uint64_t shader_hash = 0;
  std::uint64_t context_hash = 0;

  friend bool operator==(const TracePsoIdentity&, const TracePsoIdentity&) =
      default;
};

struct TraceResourceIdentity {
  std::uint64_t fingerprint = 0;
  bool dynamic_contents = false;

  friend bool operator==(
      const TraceResourceIdentity&,
      const TraceResourceIdentity&) = default;
};

struct TracePipelineInfo {
  std::uint64_t incarnation_id = 0;
  DeviceIdentity device = 0;
  std::uint64_t application_pipeline = 0;
  std::uint64_t pso_fingerprint = 0;
  std::uint64_t shader_hash = 0;
  std::uint64_t context_hash = 0;
  std::uint32_t primitive_topology = 0;
  bool live = true;
  bool rt0_blend = false;
  bool alpha_to_coverage = false;
  bool depth_test = false;
  bool depth_write = false;
  std::uint32_t render_target_count = 0;
  std::uint32_t sample_count = 1;
};

// ReShade only exposes a pipeline's creation description during init_pipeline.
// Keep a Dev-only, process-local deep copy for the small manually selected
// experiment set, so switching the target can recreate existing application
// PSOs instead of waiting for the game to create them again.
struct TargetBypassOwnedShader {
  std::vector<std::uint8_t> code;
  std::string entry_point;
  std::vector<std::uint32_t> spec_constant_ids;
  std::vector<std::uint32_t> spec_constant_values;
};

struct TargetBypassOwnedInputElement {
  input_element value;
  std::string semantic;
};

struct TargetBypassRecipeSubobject {
  pipeline_subobject_type type = pipeline_subobject_type::unknown;
  std::uint32_t count = 0;
  std::vector<std::byte> raw_data;
  std::vector<TargetBypassOwnedShader> shaders;
  std::vector<TargetBypassOwnedInputElement> input_elements;
};

struct TargetBypassPipelineRecipe {
  DeviceIdentity device = 0;
  pipeline_layout layout{};
  pipeline application_pipeline{};
  std::uint64_t shader_hash = 0;
  std::vector<TargetBypassRecipeSubobject> subobjects;
};

struct TargetBypassMaterializedRecipe {
  std::vector<pipeline_subobject> subobjects;
  std::vector<std::vector<shader_desc>> shaders;
  std::vector<std::vector<input_element>> input_layouts;
  std::vector<std::vector<std::max_align_t>> raw_storage;
  shader_desc* pixel_shader = nullptr;
};

using ConcreteWindowMetrics = wuwa_tfr::TraceSubmissionWindowMetrics;
using ConcreteSubmissionRecord =
    wuwa_tfr::TraceSubmissionRecord<kTraceWindowCount>;

struct ConcreteTraceRecord : ConcreteSubmissionRecord {
  TracePipelineInfo pipeline;
  std::uint64_t last_submission_serial = 0;
};

struct ConcreteTraceRow {
  wuwa_tfr::TraceConcreteDrawKey key;
  TracePipelineInfo pipeline;
  std::array<ConcreteWindowMetrics, kTraceWindowCount> windows;
  std::uint64_t last_submission_serial = 0;
  std::uint64_t geometry_fingerprint = 0;
  bool concrete = false;
  bool skip_eligible = false;
  bool investigation_has_discard = false;
  bool investigation_strict_spatial_dither = false;
  bool investigation_ambiguous_spatial_dither = false;
  std::uint32_t filter_reasons = 0;
  bool exact_pass_observed = false;
  bool dynamic_contents_heuristic = false;
  wuwa_tfr::TraceRouteConclusion conclusion =
      wuwa_tfr::TraceRouteConclusion::Unknown;
};

struct ShaderFamilyGroup {
  std::uint64_t shader_hash = 0;
  std::vector<std::size_t> row_indices;
  std::size_t concrete_row_count = 0;
  std::unordered_set<TracePipelineKey, TracePipelineKeyHash> application_psos;
  std::unordered_set<wuwa_tfr::TraceDrawRouteKey,
      wuwa_tfr::TraceDrawRouteKeyHash> routes;
  std::array<std::uint64_t, kTraceWindowCount> submissions{};
  bool all_fade_transition_candidates = true;
  bool any_discard = false;
  bool any_strict_spatial_dither = false;
  bool any_ambiguous_spatial_dither = false;
  bool any_blend = false;
  bool any_alpha_to_coverage = false;
};

enum ConcreteFilterReason : std::uint32_t {
  TracePresenceChanged = 1u << 0,
  TracePsoChanged = 1u << 1,
  TraceShaderChanged = 1u << 2,
  TraceBindingStateChanged = 1u << 3,
  TraceDiscardClue = 1u << 4,
  TraceDitherClue = 1u << 5,
  TraceBlendCoverageClue = 1u << 6,
  TraceStrictSpatialDitherClue = 1u << 7,
};

struct TraceWindowMetrics {
  std::uint64_t draws = 0;
  std::uint64_t active_frames = 0;
  std::uint64_t last_frame = 0;
  std::unordered_set<std::uint64_t> pso_contexts;
  std::unordered_set<std::uint64_t> root_constant_fingerprints;
  std::unordered_set<std::uint64_t> pushed_cbv_fingerprints;
  std::unordered_set<std::uint64_t> descriptor_table_fingerprints;
};

struct ShaderTraceRecord {
  std::array<TraceWindowMetrics, kTraceWindowCount> windows;
  bool any_rt0_blend = false;
  bool any_alpha_to_coverage = false;
  bool any_depth_test = false;
  bool any_depth_write = false;
  std::uint32_t max_render_target_count = 0;
  std::uint32_t max_sample_count = 1;
};

struct TraceSnapshotRow {
  std::uint64_t shader_hash = 0;
  std::array<std::uint64_t, kTraceWindowCount> draws{};
  std::array<std::uint64_t, kTraceWindowCount> active_frames{};
  std::array<std::uint64_t, kTraceWindowCount> pso_context_count{};
  std::array<std::uint64_t, kTraceWindowCount> root_constant_state_count{};
  std::array<std::uint64_t, kTraceWindowCount> pushed_cbv_state_count{};
  std::array<std::uint64_t, kTraceWindowCount> descriptor_table_state_count{};
  std::array<double, kTraceWindowCount> draws_per_frame{};
  double partial_minus_normal = 0.0;
  double full_minus_partial = 0.0;
  double comparison_score = 0.0;
  bool any_rt0_blend = false;
  bool any_alpha_to_coverage = false;
  bool any_depth_test = false;
  bool any_depth_write = false;
  std::uint32_t max_render_target_count = 0;
  std::uint32_t max_sample_count = 1;
  bool normal_partial_pso_changed = false;
  bool partial_full_pso_changed = false;
  bool normal_partial_root_constants_changed = false;
  bool partial_full_root_constants_changed = false;
  bool normal_partial_pushed_cbvs_changed = false;
  bool partial_full_pushed_cbvs_changed = false;
  bool normal_partial_descriptor_tables_changed = false;
  bool partial_full_descriptor_tables_changed = false;
  std::uint32_t state_change_count = 0;
  std::uint32_t discard_calls = 0;
  std::uint32_t strict_spatial_dither_discards = 0;
  bool strict_spatial_dither = false;
  bool ambiguous_spatial_dither = false;
};

struct RootConstantKey {
  std::uint64_t layout = 0;
  std::uint32_t parameter = 0;
  std::uint32_t word = 0;

  friend bool operator==(const RootConstantKey&, const RootConstantKey&) =
      default;
};

struct RootConstantKeyHash {
  std::size_t operator()(const RootConstantKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.layout);
    const auto combine = [&hash](std::uint64_t value) {
      const std::size_t next = std::hash<std::uint64_t>{}(value);
      hash ^= next + static_cast<std::size_t>(0x9E3779B9u) +
          (hash << 6) + (hash >> 2);
    };
    combine(key.parameter);
    combine(key.word);
    return hash;
  }
};

struct RecordedTraceDrawKey {
  wuwa_tfr::TraceConcreteDrawKey concrete;
  std::uint64_t root_constants = 0;
  std::uint64_t pushed_cbvs = 0;
  std::uint64_t descriptor_tables = 0;
  std::uint8_t observed_bindings = 0;

  friend bool operator==(const RecordedTraceDrawKey&,
      const RecordedTraceDrawKey&) = default;
};

struct RecordedTraceDrawKeyHash {
  std::size_t operator()(const RecordedTraceDrawKey& key) const noexcept {
    std::size_t hash = wuwa_tfr::TraceConcreteDrawKeyHash{}(key.concrete);
    wuwa_tfr::TraceHashCombine(hash, key.root_constants);
    wuwa_tfr::TraceHashCombine(hash, key.pushed_cbvs);
    wuwa_tfr::TraceHashCombine(hash, key.descriptor_tables);
    wuwa_tfr::TraceHashCombine(hash, key.observed_bindings);
    return hash;
  }
};

struct RecordedTraceDraw {
  std::uint64_t commands = 0;
  TracePipelineInfo pipeline;
};

struct __declspec(uuid("7928A6C2-22D4-4A56-879A-48E5DA2F8B91"))
    CommandListTrace {
  DeviceIdentity device = 0;
  std::uint64_t bound_pso_incarnation = 0;
  std::optional<TracePipelineInfo> bound_pipeline;
  std::uint64_t bound_layout = 0;
  std::uint32_t primitive_topology = 0;
  bool topology_observed = false;
  std::unordered_map<std::uint32_t, wuwa_tfr::TraceVertexBinding>
      vertex_buffers;
  bool vertex_buffers_observed = false;
  std::optional<wuwa_tfr::TraceIndexBinding> index_buffer;
  bool index_buffer_observed = false;
  std::uint64_t pass_fingerprint = 0;
  bool pass_observed = false;
  std::uint64_t root_constant_fingerprint = 0;
  std::uint64_t pushed_cbv_fingerprint = kTraceFnvOffset;
  std::uint64_t descriptor_table_fingerprint = kTraceFnvOffset;
  std::uint8_t observed_bindings = 0;
  std::unordered_map<RootConstantKey, std::uint32_t, RootConstantKeyHash>
      root_constants;
  std::unordered_map<RecordedTraceDrawKey, RecordedTraceDraw,
      RecordedTraceDrawKeyHash> recorded_draws;
  bool recorded_draw_capacity_exceeded = false;

  void Reset() {
    bound_pso_incarnation = 0;
    bound_pipeline.reset();
    bound_layout = 0;
    primitive_topology = 0;
    topology_observed = false;
    vertex_buffers.clear();
    vertex_buffers_observed = false;
    index_buffer.reset();
    index_buffer_observed = false;
    pass_fingerprint = 0;
    pass_observed = false;
    root_constant_fingerprint = 0;
    pushed_cbv_fingerprint = kTraceFnvOffset;
    descriptor_table_fingerprint = kTraceFnvOffset;
    observed_bindings = 0;
    root_constants.clear();
    recorded_draws.clear();
    recorded_draw_capacity_exceeded = false;
  }
};
#endif

DeviceIdentity DeviceKey(device* owner) noexcept {
  return reinterpret_cast<DeviceIdentity>(owner);
}

std::mutex g_inspection_mutex;
wuwa_tfr::DxcBridge* g_dxc = nullptr;
std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;
wuwa_tfr::DeviceActivityState<DeviceIdentity> g_device_activity;

std::atomic<std::uint32_t> g_d3d12_device_count{0};
std::atomic<std::uint64_t> g_seen_shader_callbacks{0};
std::atomic<std::uint64_t> g_unique_dxil_shaders{0};
std::atomic<std::uint64_t> g_disassembly_successes{0};
std::atomic<std::uint64_t> g_disassembly_failures{0};
std::atomic<std::uint64_t> g_dumped_shaders{0};
#if WUWA_TFR_DEVTOOLS
std::atomic<std::uint64_t> g_discard_shader_count{0};
std::atomic<std::uint64_t> g_strict_spatial_dither_count{0};
std::atomic<std::uint64_t> g_ambiguous_spatial_dither_count{0};
std::atomic<std::uint64_t> g_fade_primitive_shader_count{0};
std::atomic<std::uint64_t> g_fade_primitive_instance_count{0};
#endif
#if WUWA_TFR_DEVTOOLS
std::mutex g_trace_mutex;
wuwa_tfr::TraceIncarnationIndex<TracePsoIdentity> g_trace_pso_incarnations;
wuwa_tfr::TraceIncarnationIndex<TraceResourceIdentity>
    g_trace_resource_incarnations;
wuwa_tfr::TraceIncarnationIndex<std::uint64_t> g_trace_view_incarnations;
using TracePsoAmbiguityDiagnostics =
    wuwa_tfr::TraceLifecycleAmbiguityDiagnostics<TracePsoIdentity>;
using TraceResourceAmbiguityDiagnostics =
    wuwa_tfr::TraceLifecycleAmbiguityDiagnostics<TraceResourceIdentity>;
TracePsoAmbiguityDiagnostics g_trace_pso_lifecycle_ambiguities;
TraceResourceAmbiguityDiagnostics g_trace_resource_lifecycle_ambiguities;
std::uint64_t g_trace_lifecycle_event_serial = 0;
std::unordered_map<TracePipelineKey, TracePipelineInfo, TracePipelineKeyHash>
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

using TargetBypassReplacementState =
    wuwa_tfr::PipelineReplacementState<DeviceIdentity, pipeline>;

enum class TargetBypassMode : std::uint8_t {
  AllVerifiedV1,
  ManualShaderList,
  // Retained only so existing single-target helper code stays inert while the
  // Dev UI migrates to hash-group target modes. It is not selectable.
  LegacySelectedShader,
};

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

TargetBypassReplacementState g_target_bypass_replacements;
TargetBypassReplacementState g_fade_primitive_execution_replacements;
std::atomic<bool> g_target_bypass_enabled{false};
std::atomic<TargetBypassMode> g_target_bypass_mode{
    TargetBypassMode::AllVerifiedV1};
std::atomic<std::uint64_t> g_target_bypass_selected_hash{0};
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
std::mutex g_target_bypass_mutex;
std::shared_ptr<const std::vector<std::uint8_t>> g_target_bypass_bytecode;
std::uint64_t g_target_bypass_bytecode_hash = 0;
std::string g_target_bypass_status = "waiting for target shader PSO";
bool g_target_bypass_compile_attempted = false;
std::unordered_map<DeviceIdentity, device*> g_target_bypass_devices;
std::unordered_map<TracePipelineKey,
    std::shared_ptr<const TargetBypassPipelineRecipe>,
    TracePipelineKeyHash> g_target_bypass_recipes;
thread_local bool g_target_bypass_internal_create = false;
thread_local bool g_target_bypass_internal_bind = false;
thread_local bool g_target_bypass_internal_destroy = false;

std::mutex g_fade_primitive_execution_mutex;
// Persistent Dev preparation cache. Entries survive activation-policy changes
// and retain both the verified v1 metadata and successfully validated DXIL.
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
#endif

bool g_target_process = false;
bool g_diagnostic = false;
bool g_dump = false;
std::filesystem::path g_dump_path;
std::filesystem::path g_addon_directory;
#if !WUWA_TFR_DEVTOOLS
wuwa_tfr::FadePrimitiveRuntime g_public_antifade_runtime;
#endif

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

#if WUWA_TFR_DEVTOOLS
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

template <typename T>
void TraceHashValue(std::uint64_t& hash, const T& value) noexcept {
  TraceHashAppend(hash, &value, sizeof(value));
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
}

bool HasPixelStage(shader_stage stages) noexcept {
  return (stages & shader_stage::pixel) == shader_stage::pixel;
}

struct ActiveTraceResource {
  std::uint64_t incarnation = 0;
  bool dynamic_contents = false;
};

ActiveTraceResource ActiveResourceIncarnationLocked(
    DeviceIdentity device,
    resource handle) {
  if (handle.handle == 0) return {};
  const auto* record = g_trace_resource_incarnations.FindActive(
      {device, handle.handle});
  return record
      ? ActiveTraceResource{record->id, record->identity.dynamic_contents}
      : ActiveTraceResource{};
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
#endif

std::string Hex64(std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::uppercase << std::setw(16)
         << std::setfill('0') << value;
  return stream.str();
}

#if WUWA_TFR_DEVTOOLS
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
#endif

bool EnvFlag(const wchar_t* name) {
  wchar_t buffer[16]{};
  const DWORD length = GetEnvironmentVariableW(
      name, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return false;
  std::wstring value(buffer, length);
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return value == L"1" || value == L"true" || value == L"yes" ||
      value == L"on";
}

std::filesystem::path ConfigPath() {
  if (g_addon_directory.empty()) return {};
  return g_addon_directory / L"WuwaTFR.ini";
}

bool ResolveAddonDirectory(HMODULE module) {
  wchar_t module_path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(
      module, module_path, static_cast<DWORD>(std::size(module_path)));
  if (length == 0 || length >= std::size(module_path)) return false;

  const auto directory =
      std::filesystem::path(module_path, module_path + length).parent_path();
  if (directory.empty()) return false;
  g_addon_directory = directory;
  return true;
}

bool ConfigFlag(const wchar_t* key, bool fallback) {
  const auto path = ConfigPath();
  if (path.empty() ||
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return fallback;
  return GetPrivateProfileIntW(
      L"General", key, fallback ? 1 : 0, path.c_str()) != 0;
}

void SaveConfigFlag(const wchar_t* key, bool value) {
  const auto path = ConfigPath();
  if (path.empty()) return;
  WritePrivateProfileStringW(
      L"General", key, value ? L"1" : L"0", path.c_str());
}

std::filesystem::path ConfigPathValue(const wchar_t* key) {
  const auto config_path = ConfigPath();
  if (config_path.empty() ||
      GetFileAttributesW(config_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return {};

  constexpr DWORD kBufferChars = 32768;
  std::vector<wchar_t> raw(kBufferChars);
  const DWORD raw_length = GetPrivateProfileStringW(
      L"General", key, L"", raw.data(), kBufferChars,
      config_path.c_str());
  if (raw_length == 0 || raw_length >= kBufferChars - 1) return {};

  std::vector<wchar_t> expanded(kBufferChars);
  const DWORD expanded_length = ExpandEnvironmentStringsW(
      raw.data(), expanded.data(), kBufferChars);
  if (expanded_length == 0 || expanded_length > kBufferChars) return {};

  std::filesystem::path result(expanded.data());
  return result.is_absolute() ? result : std::filesystem::path{};
}

bool IsWuwaProcess() {
  wchar_t executable_path[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, executable_path, MAX_PATH)) return false;
  std::wstring name =
      std::filesystem::path(executable_path).filename().wstring();
  std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return name.find(L"client-win64-shipping") != std::wstring::npos ||
      name.find(L"wuthering") != std::wstring::npos;
}

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[WuwaTFR] " + message).c_str());
}

std::filesystem::path DumpDir() {
  if (g_dump_path.empty()) return {};
  std::error_code error;
  std::filesystem::create_directories(g_dump_path, error);
  if (error || !std::filesystem::is_directory(g_dump_path, error) || error)
    return {};
  return g_dump_path;
}

bool WriteCapture(
    std::uint64_t hash,
    std::size_t bytecode_size,
    const std::string& original_ir
#if WUWA_TFR_DEVTOOLS
    ,
    const wuwa_tfr::SpatialDitherDiagnostic& dither,
    const wuwa_tfr::FadePrimitiveDiagnostic& fade_primitive) {
#else
    ) {
#endif
  if (!g_dump || original_ir.empty()) return false;
  const auto directory = DumpDir();
  if (directory.empty()) return false;

  const std::string base = Hex64(hash);
  const auto ir_path = directory / (base + ".original.ll");
  const auto metadata_path = directory / (base + ".capture.meta.txt");

  std::ofstream ir_file(ir_path, std::ios::binary | std::ios::trunc);
  if (!ir_file) return false;
  ir_file.write(
      original_ir.data(), static_cast<std::streamsize>(original_ir.size()));
  if (!ir_file) return false;
  ir_file.close();

  std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
  if (!metadata) return false;
  metadata << "format=wuwa_tfr_capture_v1\n";
  metadata << "source=wuthering_waves_runtime_create_pipeline\n";
  metadata << "stage=original_pixel_shader\n";
  metadata << "selection=all_unique_dxil_when_dump_enabled\n";
#if WUWA_TFR_DEVTOOLS
  metadata << "analysis=independent_spatial_dither_diagnostic_v1\n";
#else
  metadata << "analysis=none\n";
#endif
  metadata << "mutation=none\n";
  metadata << "source_hash=" << base << "\n";
  metadata << "bytecode_size=" << bytecode_size << "\n";
#if WUWA_TFR_DEVTOOLS
  metadata << "discard_calls=" << dither.discard_calls << "\n";
  metadata << "strict_spatial_dither_discards="
           << dither.strict_spatial_dither_discards << "\n";
  metadata << "spatial_dither_classification="
           << wuwa_tfr::SpatialDitherClassificationName(
                  dither.classification)
           << "\n";
  metadata << "fade_primitive_detector=Fade Primitive v1\n";
  metadata << "fade_primitive_instances="
           << fade_primitive.instances.size() << "\n";
  for (std::size_t i = 0; i < fade_primitive.instances.size(); ++i) {
    metadata << "fade_primitive_instance_" << i << "_consumer="
             << wuwa_tfr::FadePrimitiveConsumerName(
                    fade_primitive.instances[i].consumer)
             << "\n";
  }
#endif
  metadata << "ir_file=" << base << ".original.ll\n";
  return static_cast<bool>(metadata);
}

bool HasDxilChunk(const std::uint8_t* data, std::size_t size) {
  if (!data || size < 32 || std::memcmp(data, "DXBC", 4) != 0) return false;

  std::uint32_t total_size = 0;
  std::uint32_t chunk_count = 0;
  std::memcpy(&total_size, data + 24, sizeof(total_size));
  std::memcpy(&chunk_count, data + 28, sizeof(chunk_count));
  if (chunk_count > 4096 || total_size != size) return false;

  const std::size_t table_end = 32ull + 4ull * chunk_count;
  if (total_size < table_end) return false;

  bool has_dxil = false;
  for (std::uint32_t i = 0; i < chunk_count; ++i) {
    std::uint32_t offset = 0;
    std::memcpy(&offset, data + 32 + 4ull * i, sizeof(offset));
    if (static_cast<std::size_t>(offset) + 8 > total_size) return false;

    std::uint32_t chunk_size = 0;
    std::memcpy(&chunk_size, data + offset + 4, sizeof(chunk_size));
    const std::size_t chunk_end =
        static_cast<std::size_t>(offset) + 8ull + chunk_size;
    if (chunk_end > total_size) return false;
    if (std::memcmp(data + offset, "DXIL", 4) == 0) has_dxil = true;
  }
  return has_dxil;
}

bool LooksLikeDxil(const shader_desc& descriptor) {
  if (!descriptor.code || descriptor.code_size < 64) return false;
  return HasDxilChunk(
      static_cast<const std::uint8_t*>(descriptor.code),
      descriptor.code_size);
}

void InspectPixelShader(const shader_desc& descriptor) {
  g_seen_shader_callbacks.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t hash = Fnv1a64(descriptor.code, descriptor.code_size);

  std::lock_guard lock(g_inspection_mutex);
  if (g_inspections.contains(hash)) return;

  g_unique_dxil_shaders.fetch_add(1, std::memory_order_relaxed);
  InspectionRecord record;
  record.bytecode_size = descriptor.code_size;

  if (!g_dxc) g_dxc = new wuwa_tfr::DxcBridge(g_addon_directory);
  if (!g_dxc->available()) {
    record.error = g_dxc->init_error();
    g_disassembly_failures.fetch_add(1, std::memory_order_relaxed);
    g_inspections.emplace(hash, std::move(record));
    return;
  }

  auto inspection =
      g_dxc->InspectShader(descriptor.code, descriptor.code_size);
  record.success = inspection.success;
  record.error = std::move(inspection.error);
  if (record.success) {
    g_disassembly_successes.fetch_add(1, std::memory_order_relaxed);
#if WUWA_TFR_DEVTOOLS
    record.dither =
        wuwa_tfr::AnalyzeSpatialDitherDiagnostic(inspection.original_ir);
    record.fade_primitive =
        wuwa_tfr::AnalyzeFadePrimitiveV1(inspection.original_ir);
    if (record.dither.discard_calls != 0)
      g_discard_shader_count.fetch_add(1, std::memory_order_relaxed);
    if (record.dither.classification ==
        wuwa_tfr::SpatialDitherClassification::StrictSpatialDither) {
      g_strict_spatial_dither_count.fetch_add(1, std::memory_order_relaxed);
    } else if (record.dither.classification ==
        wuwa_tfr::SpatialDitherClassification::
            AmbiguousStrictSpatialDither) {
      g_ambiguous_spatial_dither_count.fetch_add(
          1, std::memory_order_relaxed);
    }
    if (!record.fade_primitive.instances.empty()) {
      g_fade_primitive_shader_count.fetch_add(1, std::memory_order_relaxed);
      g_fade_primitive_instance_count.fetch_add(
          record.fade_primitive.instances.size(), std::memory_order_relaxed);
    }
#endif
    record.dumped = WriteCapture(
        hash, descriptor.code_size, inspection.original_ir
#if WUWA_TFR_DEVTOOLS
        ,
        record.dither,
        record.fade_primitive
#endif
        );
    if (record.dumped)
      g_dumped_shaders.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_disassembly_failures.fetch_add(1, std::memory_order_relaxed);
  }
  g_inspections.emplace(hash, std::move(record));
}

#if WUWA_TFR_DEVTOOLS
std::uint64_t MakeTraceToken(
    std::uint64_t generation,
    TraceWindow window) noexcept {
  return (generation << 2) | static_cast<std::uint64_t>(window);
}

TraceWindow TraceWindowFromToken(std::uint64_t token) noexcept {
  return static_cast<TraceWindow>(token & 0x3u);
}

bool FindDxilPixelShader(
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    const shader_desc*& descriptor,
    std::uint64_t& shader_hash) {
  descriptor = nullptr;
  shader_hash = 0;
  if (!subobjects) return false;

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::pixel_shader ||
        !subobjects[i].data)
      continue;
    const auto& candidate =
        *static_cast<const shader_desc*>(subobjects[i].data);
    if (!LooksLikeDxil(candidate)) continue;

    const std::uint64_t candidate_hash =
        Fnv1a64(candidate.code, candidate.code_size);
    if (descriptor && shader_hash != candidate_hash) return false;
    descriptor = &candidate;
    shader_hash = candidate_hash;
  }
  return descriptor != nullptr;
}

class ScopedThreadFlag {
 public:
  explicit ScopedThreadFlag(bool& flag) : flag_(flag), previous_(flag) {
    flag_ = true;
  }
  ~ScopedThreadFlag() { flag_ = previous_; }

 private:
  bool& flag_;
  bool previous_;
};

void DestroyTargetBypassReplacement(device* owner, pipeline replacement) {
  if (!owner || replacement.handle == 0) return;
  ScopedThreadFlag internal_destroy(g_target_bypass_internal_destroy);
  owner->destroy_pipeline(replacement);
}

std::uint64_t SelectedTargetBypassHash() noexcept {
  return g_target_bypass_selected_hash.load(std::memory_order_relaxed);
}

bool IsNormalOnlyRimSkipRow(const ConcreteTraceRow& row) noexcept {
  return wuwa_tfr::TraceGeometryIsConcrete(row.key.geometry) &&
      (row.key.geometry.kind == wuwa_tfr::TraceDrawKind::Direct ||
          row.key.geometry.kind == wuwa_tfr::TraceDrawKind::Indexed) &&
      wuwa_tfr::TraceNormalOnlySubmissionCandidate(row.windows);
}

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

bool IsTargetBypassShaderSubobject(pipeline_subobject_type type) noexcept {
  switch (type) {
    case pipeline_subobject_type::vertex_shader:
    case pipeline_subobject_type::hull_shader:
    case pipeline_subobject_type::domain_shader:
    case pipeline_subobject_type::geometry_shader:
    case pipeline_subobject_type::pixel_shader:
      return true;
    default:
      return false;
  }
}

std::size_t TargetBypassRawElementSize(pipeline_subobject_type type) noexcept {
  switch (type) {
    case pipeline_subobject_type::stream_output_state:
      return sizeof(stream_output_desc);
    case pipeline_subobject_type::blend_state:
      return sizeof(blend_desc);
    case pipeline_subobject_type::rasterizer_state:
      return sizeof(rasterizer_desc);
    case pipeline_subobject_type::depth_stencil_state:
      return sizeof(depth_stencil_desc);
    case pipeline_subobject_type::primitive_topology:
      return sizeof(primitive_topology);
    case pipeline_subobject_type::depth_stencil_format:
    case pipeline_subobject_type::render_target_formats:
      return sizeof(format);
    case pipeline_subobject_type::sample_mask:
    case pipeline_subobject_type::sample_count:
    case pipeline_subobject_type::viewport_count:
    case pipeline_subobject_type::max_vertex_count:
      return sizeof(std::uint32_t);
    case pipeline_subobject_type::dynamic_pipeline_states:
      return sizeof(dynamic_state);
    case pipeline_subobject_type::flags:
      return sizeof(pipeline_flags);
    default:
      return 0;
  }
}

std::shared_ptr<const TargetBypassPipelineRecipe> CopyTargetBypassRecipe(
    DeviceIdentity device_key,
    pipeline_layout layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    pipeline application_pipeline,
    std::uint64_t shader_hash,
    std::string& error) {
  if (!subobjects || application_pipeline.handle == 0) {
    error = "missing pipeline creation description";
    return {};
  }

  auto recipe = std::make_shared<TargetBypassPipelineRecipe>();
  recipe->device = device_key;
  recipe->layout = layout;
  recipe->application_pipeline = application_pipeline;
  recipe->shader_hash = shader_hash;
  recipe->subobjects.reserve(subobject_count);

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    const pipeline_subobject& source = subobjects[i];
    TargetBypassRecipeSubobject destination;
    destination.type = source.type;
    destination.count = source.count;
    if (source.count == 0) {
      recipe->subobjects.push_back(std::move(destination));
      continue;
    }
    if (!source.data) {
      error = "pipeline subobject has null data";
      return {};
    }

    if (IsTargetBypassShaderSubobject(source.type)) {
      const auto* shaders = static_cast<const shader_desc*>(source.data);
      destination.shaders.reserve(source.count);
      for (std::uint32_t j = 0; j < source.count; ++j) {
        const shader_desc& shader = shaders[j];
        if ((shader.code_size != 0 && !shader.code) ||
            (shader.spec_constants != 0 &&
                (!shader.spec_constant_ids || !shader.spec_constant_values))) {
          error = "shader subobject has incomplete data";
          return {};
        }
        TargetBypassOwnedShader owned;
        const auto* code = static_cast<const std::uint8_t*>(shader.code);
        if (shader.code_size != 0)
          owned.code.assign(code, code + shader.code_size);
        if (shader.entry_point) owned.entry_point = shader.entry_point;
        if (shader.spec_constants != 0) {
          owned.spec_constant_ids.assign(shader.spec_constant_ids,
              shader.spec_constant_ids + shader.spec_constants);
          owned.spec_constant_values.assign(shader.spec_constant_values,
              shader.spec_constant_values + shader.spec_constants);
        }
        destination.shaders.push_back(std::move(owned));
      }
    } else if (source.type == pipeline_subobject_type::input_layout) {
      const auto* elements = static_cast<const input_element*>(source.data);
      destination.input_elements.reserve(source.count);
      for (std::uint32_t j = 0; j < source.count; ++j) {
        TargetBypassOwnedInputElement owned;
        owned.value = elements[j];
        if (elements[j].semantic) owned.semantic = elements[j].semantic;
        owned.value.semantic = nullptr;
        destination.input_elements.push_back(std::move(owned));
      }
    } else {
      const std::size_t element_size = TargetBypassRawElementSize(source.type);
      if (element_size == 0) {
        error = "unsupported pipeline subobject in cached recipe";
        return {};
      }
      destination.raw_data.resize(element_size * source.count);
      std::memcpy(destination.raw_data.data(), source.data,
          destination.raw_data.size());
    }
    recipe->subobjects.push_back(std::move(destination));
  }
  return recipe;
}

bool MaterializeTargetBypassRecipe(
    const TargetBypassPipelineRecipe& recipe,
    TargetBypassMaterializedRecipe& materialized,
    std::string& error) {
  materialized.subobjects.reserve(recipe.subobjects.size());
  materialized.shaders.reserve(recipe.subobjects.size());
  materialized.input_layouts.reserve(recipe.subobjects.size());
  materialized.raw_storage.reserve(recipe.subobjects.size());

  for (const auto& source : recipe.subobjects) {
    pipeline_subobject destination{source.type, source.count, nullptr};
    if (source.count == 0) {
      materialized.subobjects.push_back(destination);
      continue;
    }
    if (IsTargetBypassShaderSubobject(source.type)) {
      if (source.shaders.size() != source.count) {
        error = "cached shader subobject count mismatch";
        return false;
      }
      auto& shaders = materialized.shaders.emplace_back();
      shaders.resize(source.count);
      for (std::uint32_t i = 0; i < source.count; ++i) {
        const auto& owned = source.shaders[i];
        shader_desc& shader = shaders[i];
        shader.code = owned.code.empty() ? nullptr : owned.code.data();
        shader.code_size = owned.code.size();
        shader.entry_point = owned.entry_point.empty()
            ? nullptr : owned.entry_point.c_str();
        shader.spec_constants = static_cast<std::uint32_t>(
            owned.spec_constant_ids.size());
        shader.spec_constant_ids = owned.spec_constant_ids.empty()
            ? nullptr : owned.spec_constant_ids.data();
        shader.spec_constant_values = owned.spec_constant_values.empty()
            ? nullptr : owned.spec_constant_values.data();
      }
      destination.data = shaders.data();
      if (source.type == pipeline_subobject_type::pixel_shader) {
        if (source.count != 1 || materialized.pixel_shader != nullptr) {
          error = "cached recipe has an ambiguous pixel shader descriptor";
          return false;
        }
        materialized.pixel_shader = &shaders.front();
      }
    } else if (source.type == pipeline_subobject_type::input_layout) {
      if (source.input_elements.size() != source.count) {
        error = "cached input-layout count mismatch";
        return false;
      }
      auto& elements = materialized.input_layouts.emplace_back();
      elements.resize(source.count);
      for (std::uint32_t i = 0; i < source.count; ++i) {
        elements[i] = source.input_elements[i].value;
        elements[i].semantic = source.input_elements[i].semantic.empty()
            ? nullptr : source.input_elements[i].semantic.c_str();
      }
      destination.data = elements.data();
    } else {
      const std::size_t words =
          (source.raw_data.size() + sizeof(std::max_align_t) - 1) /
          sizeof(std::max_align_t);
      if (words == 0) {
        error = "cached raw subobject has no data";
        return false;
      }
      auto& storage = materialized.raw_storage.emplace_back(words);
      std::memcpy(storage.data(), source.raw_data.data(),
          source.raw_data.size());
      destination.data = storage.data();
    }
    materialized.subobjects.push_back(destination);
  }
  if (!materialized.pixel_shader) {
    error = "cached recipe has no pixel shader descriptor";
    return false;
  }
  return true;
}

void CreateTargetBypassReplacement(
    device* owner,
    const std::shared_ptr<const TargetBypassPipelineRecipe>& recipe,
    std::uint64_t target_hash);

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

TracePipelineInfo DescribeTracePipeline(
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    std::uint64_t shader_hash) {
  blend_desc blend{};
  depth_stencil_desc depth{};
  TracePipelineInfo info;
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

void OnInitTracePipeline(
    device* owner,
    pipeline_layout layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects,
    pipeline handle) {
  if (g_target_bypass_internal_create) return;
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      handle.handle == 0)
    return;

  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return;

  const TracePipelineKey key{DeviceKey(owner), handle.handle};
  const shader_desc* descriptor = nullptr;
  std::uint64_t shader_hash = 0;
  if (!FindDxilPixelShader(
          subobject_count, subobjects, descriptor, shader_hash)) {
    std::lock_guard lock(g_trace_mutex);
    g_trace_pso_incarnations.Destroy(key);
    g_trace_pipelines.erase(key);
    g_target_bypass_recipes.erase(key);
    return;
  }

  // All-v1 Dev mode is driven by the frozen structural detector, so every
  // observed DXIL pixel shader is inspected even when capture dumping is off.
  InspectPixelShader(*descriptor);

  TracePipelineInfo pipeline_info = DescribeTracePipeline(
      subobject_count, subobjects, shader_hash);

  std::shared_ptr<const TargetBypassPipelineRecipe> target_recipe;
  std::string target_recipe_error;
  // The frozen execution-set is chosen after capture completes, so Dev keeps
  // a deep copy for every live DXIL PSO until that point. Only the frozen set
  // is ever patched or given a replacement.
  target_recipe = CopyTargetBypassRecipe(key.owner, layout, subobject_count,
      subobjects, handle, shader_hash, target_recipe_error);

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
    pipeline_info.live = true;
    g_trace_pipelines[key] = pipeline_info;
    g_trace_shaders.try_emplace(shader_hash);
    if (target_recipe) {
      g_target_bypass_recipes[key] = target_recipe;
    } else {
      g_target_bypass_recipes.erase(key);
    }
    const std::size_t pruned =
        g_trace_pso_incarnations.PruneTo(kMaxTrackedPsoIncarnations);
    if (pruned != 0) {
      g_trace_incarnation_prunes += pruned;
      g_trace_identity_capacity_exceeded = true;
      std::erase_if(g_trace_pipelines, [](const auto& entry) {
        return g_trace_pso_incarnations.FindActive(entry.first) == nullptr;
      });
      std::erase_if(g_target_bypass_recipes, [](const auto& entry) {
        return g_trace_pso_incarnations.FindActive(entry.first) == nullptr;
      });
    }
  }

  EnsureAllVerifiedV1Target(shader_hash);
  if (IsFadePrimitiveExecutionActive(shader_hash)) {
    if (target_recipe) {
      CreateFadePrimitiveExecutionReplacement(owner, target_recipe, shader_hash);
    } else {
      RecordFadePrimitiveExecutionFailure(shader_hash, "replacement PSO creation",
          "could not cache pipeline recipe: " + target_recipe_error);
    }
  }
}

void OnDestroyTracePipeline(device* owner, pipeline handle) {
  if (g_target_bypass_internal_destroy) return;
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      handle.handle == 0)
    return;
  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return;

  const TracePipelineKey key{DeviceKey(owner), handle.handle};
  std::uint64_t shader_hash = 0;
  {
    std::lock_guard lock(g_trace_mutex);
    if (const auto pipeline_it = g_trace_pipelines.find(key);
        pipeline_it != g_trace_pipelines.end())
      shader_hash = pipeline_it->second.shader_hash;
  }
  const auto removed = g_target_bypass_replacements.Remove(
      key.owner, key.handle);
  if (removed.final_antifade)
    DestroyTargetBypassReplacement(owner, *removed.final_antifade);
  const auto batch_removed = g_fade_primitive_execution_replacements.Remove(
      key.owner, key.handle);
  if (batch_removed.final_antifade) {
    DestroyTargetBypassReplacement(owner, *batch_removed.final_antifade);
    g_fade_primitive_execution_replacements_destroyed.fetch_add(1,
        std::memory_order_relaxed);
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    if (const auto target = g_fade_primitive_execution_targets.find(shader_hash);
        target != g_fade_primitive_execution_targets.end() &&
        target->second.live_replacements != 0)
      --target->second.live_replacements;
  }

  std::lock_guard lock(g_trace_mutex);
  g_trace_pso_incarnations.Destroy(key);
  g_trace_pipelines.erase(key);
  g_target_bypass_recipes.erase(key);
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
  if (g_target_bypass_internal_bind) return;
  if (!cmd_list ||
      (stages & pipeline_stage::pixel_shader) != pipeline_stage::pixel_shader)
    return;
  auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace) return;
  std::uint64_t shader_hash = 0;
  {
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
    shader_hash = pipeline_it->second.shader_hash;
  }

  if (!IsFadePrimitiveExecutionActive(shader_hash)) return;

  if (!g_target_bypass_enabled.load(std::memory_order_relaxed)) {
    g_fade_primitive_execution_original_bind_hits.fetch_add(1,
        std::memory_order_relaxed);
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    if (const auto target = g_fade_primitive_execution_targets.find(shader_hash);
        target != g_fade_primitive_execution_targets.end())
      ++target->second.original_bind_hits;
    return;
  }

  bool replacement_bound = false;
  g_fade_primitive_execution_replacements.WithSelected(
      trace->device, handle.handle, true, true,
      [&](pipeline replacement) {
        ScopedThreadFlag internal_bind(g_target_bypass_internal_bind);
        cmd_list->bind_pipeline(stages, replacement);
        g_fade_primitive_execution_bind_hits.fetch_add(1,
            std::memory_order_relaxed);
        replacement_bound = true;
      });
  if (!replacement_bound) return;
  std::lock_guard lock(g_fade_primitive_execution_mutex);
  if (const auto target = g_fade_primitive_execution_targets.find(shader_hash);
      target != g_fade_primitive_execution_targets.end())
    ++target->second.replacement_bind_hits;
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
    bool include_unobserved_dither = false) {
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
  row.pipeline.live = live != g_trace_pipelines.end() &&
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

bool WriteTraceReport() {
  std::array<std::uint32_t, kTraceWindowCount> captured_frames{};
  bool concrete_capacity_exceeded = false;
  std::uint64_t incarnation_prunes = 0;
  TracePsoAmbiguityDiagnostics::Snapshot pso_ambiguities;
  TraceResourceAmbiguityDiagnostics::Snapshot resource_ambiguities;
  {
    std::lock_guard lock(g_trace_mutex);
    if (g_trace_token.load(std::memory_order_relaxed) != 0 ||
        !std::all_of(g_trace_capture_complete.begin(),
            g_trace_capture_complete.end(), [](bool complete) {
              return complete;
            }))
      return false;
    for (std::size_t i = 0; i < kTraceWindowCount; ++i)
      captured_frames[i] =
          g_trace_captured_frames[i].load(std::memory_order_relaxed);
    concrete_capacity_exceeded = g_concrete_trace_capacity_exceeded ||
        g_trace_identity_capacity_exceeded;
    incarnation_prunes = g_trace_incarnation_prunes;
    pso_ambiguities = g_trace_pso_lifecycle_ambiguities.GetSnapshot();
    resource_ambiguities =
        g_trace_resource_lifecycle_ambiguities.GetSnapshot();
  }

  const auto write_ambiguity_metadata = [](auto& output,
                                            const char* event_class,
                                            const auto& snapshot) {
    output << event_class << "_lifecycle_ambiguity_events\t"
        << snapshot.total_events << "\n";
    output << event_class << "_lifecycle_ambiguity_unique_handles\t"
        << snapshot.unique_handles << "\n";
    output << event_class << "_lifecycle_ambiguity_max_per_handle\t"
        << snapshot.max_events_for_one_handle << "\n";
  };

  const auto directory = DumpDir();
  if (directory.empty()) return false;
  const auto rows = TraceSnapshot();

  std::ofstream report(
      directory / "runtime-trace.tsv",
      std::ios::binary | std::ios::trunc);
  if (!report) return false;
  report << "format\twuwa_tfr_runtime_trace_v5\n";
  report << "source\twuthering_waves_runtime\n";
  report << "capture_window_mutation\tnone\n";
  report << "evidence\tsearch_clues_only_not_camera_fade_positive\n";
  report << "window_order\tnormal_partial_fade_full_fade\n";
  report << "present_stream\tfirst_normal_window_swapchain\n";
  write_ambiguity_metadata(report, "pso", pso_ambiguities);
  write_ambiguity_metadata(report, "resource", resource_ambiguities);
  report << "resource_view_lifecycle_semantics\t"
      "descriptor_slot_version_replacement_not_lifecycle_ambiguity\n";
  report << "concrete_capacity_exceeded\t"
      << static_cast<int>(concrete_capacity_exceeded) << "\n";
  report << "incarnation_prunes\t" << incarnation_prunes << "\n";
  report << "normal_frames\t" << captured_frames[0] << "\n";
  report << "partial_fade_frames\t" << captured_frames[1] << "\n";
  report << "full_fade_frames\t" << captured_frames[2] << "\n";
  report << "binding_observation\troot_constant_state_and_binding_event_fingerprints_only\n";
  report << "shader_hash\tnormal_draws\tpartial_fade_draws\tfull_fade_draws"
            "\tnormal_active_frames\tpartial_fade_active_frames"
            "\tfull_fade_active_frames\tnormal_draws_per_frame"
            "\tpartial_fade_draws_per_frame\tfull_fade_draws_per_frame"
            "\tpartial_minus_normal\tfull_minus_partial\tcomparison_score"
            "\tnormal_pso_contexts\tpartial_fade_pso_contexts"
            "\tfull_fade_pso_contexts\tnormal_root_constant_states"
            "\tpartial_fade_root_constant_states"
            "\tfull_fade_root_constant_states"
            "\tnormal_pushed_cbv_event_fingerprints"
            "\tpartial_fade_pushed_cbv_event_fingerprints"
            "\tfull_fade_pushed_cbv_event_fingerprints"
            "\tnormal_descriptor_table_event_fingerprints"
            "\tpartial_fade_descriptor_table_event_fingerprints"
            "\tfull_fade_descriptor_table_event_fingerprints"
            "\tnormal_partial_pso_changed\tpartial_full_pso_changed"
            "\tnormal_partial_root_constants_changed"
            "\tpartial_full_root_constants_changed"
            "\tnormal_partial_pushed_cbv_events_changed"
            "\tpartial_full_pushed_cbv_events_changed"
            "\tnormal_partial_descriptor_table_events_changed"
            "\tpartial_full_descriptor_table_events_changed"
            "\tstate_change_count\tdiscard_calls"
            "\tstrict_spatial_dither_discards"
            "\tstrict_spatial_dither\tambiguous_spatial_dither"
            "\tany_rt0_blend"
            "\tany_alpha_to_coverage\tany_depth_test\tany_depth_write"
            "\tmax_render_targets\tmax_sample_count\n";
  report << std::fixed << std::setprecision(6);
  for (const auto& row : rows) {
    report << Hex64(row.shader_hash) << '\t'
           << row.draws[0] << '\t' << row.draws[1] << '\t' << row.draws[2]
           << '\t' << row.active_frames[0] << '\t' << row.active_frames[1]
           << '\t' << row.active_frames[2] << '\t' << row.draws_per_frame[0]
           << '\t' << row.draws_per_frame[1] << '\t' << row.draws_per_frame[2]
           << '\t' << row.partial_minus_normal << '\t'
           << row.full_minus_partial << '\t' << row.comparison_score
           << '\t' << row.pso_context_count[0] << '\t'
           << row.pso_context_count[1] << '\t' << row.pso_context_count[2]
           << '\t' << row.root_constant_state_count[0] << '\t'
           << row.root_constant_state_count[1] << '\t'
           << row.root_constant_state_count[2] << '\t'
           << row.pushed_cbv_state_count[0] << '\t'
           << row.pushed_cbv_state_count[1] << '\t'
           << row.pushed_cbv_state_count[2] << '\t'
           << row.descriptor_table_state_count[0] << '\t'
           << row.descriptor_table_state_count[1] << '\t'
           << row.descriptor_table_state_count[2] << '\t'
           << static_cast<int>(row.normal_partial_pso_changed) << '\t'
           << static_cast<int>(row.partial_full_pso_changed) << '\t'
           << static_cast<int>(
                  row.normal_partial_root_constants_changed) << '\t'
           << static_cast<int>(
                  row.partial_full_root_constants_changed) << '\t'
           << static_cast<int>(row.normal_partial_pushed_cbvs_changed) << '\t'
           << static_cast<int>(row.partial_full_pushed_cbvs_changed) << '\t'
           << static_cast<int>(
                  row.normal_partial_descriptor_tables_changed) << '\t'
           << static_cast<int>(
                  row.partial_full_descriptor_tables_changed) << '\t'
           << row.state_change_count << '\t'
           << row.discard_calls << '\t'
           << row.strict_spatial_dither_discards << '\t'
           << static_cast<int>(row.strict_spatial_dither) << '\t'
           << static_cast<int>(row.ambiguous_spatial_dither) << '\t'
           << static_cast<int>(row.any_rt0_blend) << '\t'
           << static_cast<int>(row.any_alpha_to_coverage) << '\t'
           << static_cast<int>(row.any_depth_test) << '\t'
           << static_cast<int>(row.any_depth_write) << '\t'
           << row.max_render_target_count << '\t' << row.max_sample_count
           << '\n';
  }
  report.flush();
  if (!report) return false;

  const auto concrete_rows = ConcreteTraceSnapshot();
  std::ofstream concrete_report(
      directory / "concrete-submission-trace.tsv",
      std::ios::binary | std::ios::trunc);
  if (!concrete_report) return false;
  concrete_report << "format\twuwa_tfr_concrete_submission_trace_v2\n";
  concrete_report << "source\twuthering_waves_runtime\n";
  concrete_report << "capture_window_mutation\tnone\n";
  concrete_report << "meaning\tgeometry_exact_pass_route_cpu_queue_submission_observation_only\n";
  concrete_report << "identity\tgeometry_pass_route_never_object_identity\n";
  concrete_report << "assessment\traw_capture_rows_only_generate_frozen_list_for_guarded_route_status\n";
  concrete_report << "unknown_policy\tmissing_exact_pass_or_dynamic_contents_heuristic_or_capacity_loss\n";
  concrete_report << "dynamic_contents_flag\theap_or_usage_heuristic_not_mutation_tracking\n";
  concrete_report << "limitation\tnot_gpu_completion_not_visible_pixels_not_fade_causality\n";
  concrete_report << "direct_args\tvertex_count_instance_count_first_vertex_first_instance_unused\n";
  concrete_report << "indexed_args\tindex_count_instance_count_first_index_vertex_offset_bits_first_instance\n";
  concrete_report << "mesh_args\tgroup_x_group_y_group_z_unused_unused\n";
  concrete_report << "indirect_args\tunknown_use_indirect_columns\n";
  write_ambiguity_metadata(concrete_report, "pso", pso_ambiguities);
  write_ambiguity_metadata(
      concrete_report, "resource", resource_ambiguities);
  concrete_report << "resource_view_lifecycle_semantics\t"
      "descriptor_slot_version_replacement_not_lifecycle_ambiguity\n";
  concrete_report << "capacity_exceeded\t"
      << static_cast<int>(concrete_capacity_exceeded) << "\n";
  concrete_report << "incarnation_prunes\t" << incarnation_prunes << "\n";
  concrete_report << "normal_frames\t" << captured_frames[0] << "\n";
  concrete_report << "partial_fade_frames\t" << captured_frames[1] << "\n";
  concrete_report << "full_fade_frames\t" << captured_frames[2] << "\n";
  concrete_report << "device\tapplication_pso\tpso_incarnation\tshader_hash"
      "\tdraw_kind\tgeometry_binding_hash\targ0\targ1\targ2\targ3\targ4"
      "\ttopology\tvertex_bindings\tindex_binding"
      "\tindirect_argument_resource\tindirect_offset"
      "\tindirect_declared_count\tindirect_stride"
      "\tpass_hash\tconcrete\tskip_eligible"
      "\tlive\tobservation_flags\tnormal_commands\tnormal_submissions"
      "\tnormal_active_frames\tpartial_commands\tpartial_submissions"
      "\tpartial_active_frames\tfull_commands\tfull_submissions"
      "\tfull_active_frames\n";
  for (const auto& row : concrete_rows) {
    std::ostringstream vertex_bindings;
    for (std::size_t i = 0; i < row.key.geometry.vertex_buffers.size(); ++i) {
      const auto& binding = row.key.geometry.vertex_buffers[i];
      if (i != 0) vertex_bindings << ';';
      vertex_bindings << binding.slot << ':' << binding.resource_incarnation
          << ':' << binding.offset << ':' << binding.stride << ':'
          << static_cast<int>(binding.dynamic_contents);
    }
    std::ostringstream index_binding;
    if (row.key.geometry.index_buffer) {
      index_binding << row.key.geometry.index_buffer->resource_incarnation
          << ':' << row.key.geometry.index_buffer->offset << ':'
          << row.key.geometry.index_buffer->index_size << ':'
          << static_cast<int>(
                 row.key.geometry.index_buffer->dynamic_contents);
    } else {
      index_binding << '-';
    }
    concrete_report << Hex64(row.pipeline.device) << '\t'
        << Hex64(row.pipeline.application_pipeline) << '\t'
        << row.key.pso_incarnation << '\t'
        << Hex64(row.pipeline.shader_hash) << '\t'
        << TraceDrawKindName(row.key.geometry.kind) << '\t'
        << Hex64(row.geometry_fingerprint);
    for (const auto argument : row.key.geometry.arguments)
      concrete_report << '\t' << argument;
    concrete_report << '\t' << row.key.geometry.topology << '\t'
        << vertex_bindings.str() << '\t' << index_binding.str() << '\t'
        << row.key.geometry.indirect_resource_incarnation << '\t'
        << row.key.geometry.indirect_offset << '\t'
        << row.key.geometry.indirect_declared_count << '\t'
        << row.key.geometry.indirect_stride << '\t'
        << Hex64(row.key.pass_fingerprint) << '\t'
        << static_cast<int>(row.concrete) << '\t'
        << static_cast<int>(row.skip_eligible) << '\t'
        << static_cast<int>(row.pipeline.live) << '\t'
        << row.key.geometry.observations;
    for (const auto& window : row.windows)
      concrete_report << '\t' << window.queue_submitted_commands << '\t'
          << window.command_list_submissions << '\t' << window.active_frames;
    concrete_report << '\n';
  }
  concrete_report.flush();
  if (!concrete_report) return false;

  std::ofstream ambiguity_report(
      directory / "lifecycle-ambiguities.tsv",
      std::ios::binary | std::ios::trunc);
  if (!ambiguity_report) return false;
  ambiguity_report << "format\twuwa_tfr_lifecycle_ambiguities_v2\n";
  ambiguity_report << "source\twuthering_waves_runtime\n";
  ambiguity_report << "capture_scope\tnormal_through_full_window_experiment\n";
  ambiguity_report <<
      "sample_policy\tfirst_32_unique_device_handle_per_ambiguity_class\n";
  ambiguity_report << "identity_policy\tfirst_ambiguity_for_sampled_handle\n";
  ambiguity_report << "resource_view_semantics\t"
      "descriptor_slot_version_replacement_not_lifecycle_ambiguity\n";
  ambiguity_report <<
      "class\ttotal_events\tunique_device_handles\tmax_events_for_one_handle\n";
  const auto write_summary_row = [&ambiguity_report](
                                     const char* event_class,
                                     const auto& snapshot) {
    ambiguity_report << event_class << '\t' << snapshot.total_events << '\t'
        << snapshot.unique_handles << '\t'
        << snapshot.max_events_for_one_handle << '\n';
  };
  write_summary_row("pso", pso_ambiguities);
  write_summary_row("resource", resource_ambiguities);
  ambiguity_report <<
      "samples\nclass\tdevice\thandle\tprevious_identity\tnew_identity"
      "\tframe\tevent_serial\thandle_event_count\n";
  const auto write_samples = [&ambiguity_report](
                                 const char* event_class,
                                 const auto& snapshot) {
    for (const auto& sample : snapshot.samples) {
      ambiguity_report << event_class << '\t' << Hex64(sample.key.owner)
          << '\t' << Hex64(sample.key.handle) << '\t'
          << TraceIdentityText(sample.previous_identity) << '\t'
          << TraceIdentityText(sample.new_identity) << '\t' << sample.frame
          << '\t' << sample.event_serial << '\t'
          << sample.handle_event_count << '\n';
    }
  };
  write_samples("pso", pso_ambiguities);
  write_samples("resource", resource_ambiguities);
  return static_cast<bool>(ambiguity_report);
}

std::string LocalExportTimestamp() {
  SYSTEMTIME time{};
  GetLocalTime(&time);
  std::ostringstream stream;
  stream << std::setfill('0') << std::setw(4) << time.wYear
         << std::setw(2) << time.wMonth << std::setw(2) << time.wDay << '-'
         << std::setw(2) << time.wHour << std::setw(2) << time.wMinute
         << std::setw(2) << time.wSecond;
  return stream.str();
}

bool WriteCurrentInvestigationRange(
    const std::vector<ConcreteTraceRow>& frozen_rows,
    const std::vector<std::size_t>& displayed_indices,
    wuwa_tfr::TraceCandidateRange range,
    wuwa_tfr::TraceInvestigationView investigation_view) {
  if (range.begin > range.end || range.end > displayed_indices.size())
    return false;
  const auto directory = DumpDir();
  if (directory.empty()) return false;

  const std::string timestamp = LocalExportTimestamp();
  std::ofstream report(directory / ("investigation-range-" + timestamp +
                                        ".tsv"),
      std::ios::binary | std::ios::trunc);
  if (!report) return false;

  report << "format\twuwa_tfr_investigation_range_v1\n";
  report << "source\twuthering_waves_runtime\n";
  report << "meaning\tfrozen_investigation_range_search_export_only\n";
  report << "investigation_view\t"
      << wuwa_tfr::TraceInvestigationViewName(investigation_view) << '\n';
  report << "frozen_population_size\t" << frozen_rows.size() << '\n';
  report << "view_population_size\t" << displayed_indices.size() << '\n';
  report << "current_range_begin\t" << range.begin << '\n';
  report << "current_range_end_exclusive\t" << range.end << '\n';
  report << "export_timestamp_local\t" << timestamp << '\n';
  report << "range_relative_index\tfrozen_row_index\tdevice_identity"
            "\tapplication_pso_handle\tpso_incarnation\tpixel_shader_hash"
            "\tpso_fingerprint\tgeometry_hash\texact_pass_hash\tdraw_kind"
            "\tnormal_submissions\tpartial_submissions\tfull_submissions"
            "\tfade_transition_candidate\tpartial_full_equal"
            "\tdiscard_clue\tstrict_spatial_dither_clue"
            "\tambiguous_spatial_dither_clue\tblend\talpha_to_coverage"
            "\tskip_eligible\n";

  for (std::size_t position = range.begin; position < range.end; ++position) {
    const std::size_t frozen_index = displayed_indices[position];
    if (frozen_index >= frozen_rows.size()) return false;
    const auto& row = frozen_rows[frozen_index];
    const auto& windows = row.windows;
    report << (position - range.begin) << '\t' << frozen_index << '\t'
           << Hex64(row.pipeline.device) << '\t'
           << Hex64(row.pipeline.application_pipeline) << '\t'
           << row.pipeline.incarnation_id << '\t'
           << Hex64(row.pipeline.shader_hash) << '\t';
    if (row.pipeline.pso_fingerprint != 0)
      report << Hex64(row.pipeline.pso_fingerprint);
    else
      report << "unavailable";
    report << '\t' << Hex64(row.geometry_fingerprint) << '\t'
           << Hex64(row.key.pass_fingerprint) << '\t'
           << TraceDrawKindName(row.key.geometry.kind) << '\t'
           << windows[0].command_list_submissions << '\t'
           << windows[1].command_list_submissions << '\t'
           << windows[2].command_list_submissions << '\t'
           << static_cast<int>(wuwa_tfr::TraceFadeTransitionCandidate(windows))
           << '\t' << static_cast<int>(wuwa_tfr::TracePartialFullEqual(windows))
           << '\t' << static_cast<int>(row.investigation_has_discard)
           << '\t'
           << static_cast<int>(row.investigation_strict_spatial_dither)
           << '\t'
           << static_cast<int>(row.investigation_ambiguous_spatial_dither)
           << '\t' << static_cast<int>(row.pipeline.rt0_blend) << '\t'
           << static_cast<int>(row.pipeline.alpha_to_coverage) << '\t'
           << static_cast<int>(row.skip_eligible) << '\n';
  }
  report.flush();
  return static_cast<bool>(report);
}
#endif

bool OnCreatePipeline(
    device* owner,
    pipeline_layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects) {
#if WUWA_TFR_DEVTOOLS
  if (g_target_bypass_internal_create) return false;
#endif
  // Dev capture observes the original descriptor only; it never mutates it.
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      (!g_diagnostic && !g_dump) || !subobjects)
    return false;

  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return false;

  for (std::uint32_t i = 0; i < subobject_count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::pixel_shader ||
        !subobjects[i].data)
      continue;
    const auto& descriptor =
        *static_cast<const shader_desc*>(subobjects[i].data);
    if (LooksLikeDxil(descriptor)) InspectPixelShader(descriptor);
  }
  return false;
}

void OnInitDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;
  if (g_device_activity.Activate(DeviceKey(owner)))
    g_d3d12_device_count.fetch_add(1, std::memory_order_relaxed);
#if WUWA_TFR_DEVTOOLS
  {
    std::lock_guard lock(g_target_bypass_mutex);
    g_target_bypass_devices[DeviceKey(owner)] = owner;
  }
#endif
}

void OnDestroyDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;

  auto teardown = g_device_activity.Deactivate(DeviceKey(owner));
  if (!teardown) return;

#if WUWA_TFR_DEVTOOLS
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
#endif

  std::uint32_t previous = g_d3d12_device_count.load(std::memory_order_acquire);
  while (previous != 0 &&
         !g_d3d12_device_count.compare_exchange_weak(
             previous, previous - 1,
             std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (previous != 1) return;

  std::lock_guard lock(g_inspection_mutex);
  delete g_dxc;
  g_dxc = nullptr;
}

#if WUWA_TFR_DEVTOOLS
void DrawFadePrimitiveTargetModes() {
  if (!ImGui::CollapsingHeader(
          "Dev transparency-filter target modes", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  bool enabled = g_target_bypass_enabled.load(std::memory_order_relaxed);
  if (ImGui::Checkbox("Remove transparency filter", &enabled))
    g_target_bypass_enabled.store(enabled, std::memory_order_relaxed);

  TargetBypassMode mode = g_target_bypass_mode.load(
      std::memory_order_relaxed);
  if (ImGui::RadioButton("All v1", mode == TargetBypassMode::AllVerifiedV1)) {
    g_target_bypass_mode.store(TargetBypassMode::AllVerifiedV1,
        std::memory_order_relaxed);
    RebuildFadePrimitiveExecutionTargets();
    mode = TargetBypassMode::AllVerifiedV1;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Manual shader list",
          mode == TargetBypassMode::ManualShaderList)) {
    g_target_bypass_mode.store(TargetBypassMode::ManualShaderList,
        std::memory_order_relaxed);
    RebuildFadePrimitiveExecutionTargets();
    mode = TargetBypassMode::ManualShaderList;
  }

  ImGui::TextDisabled(
      "All v1 follows only fully verified Fade Primitive v1 structure; it has no N/P/F, frozen-hash, or character-list filter.");
  ImGui::TextDisabled(
      "Manual selection is hash-group control only. Non-v1 rows remain ineligible and are never patched.");

  if (mode == TargetBypassMode::ManualShaderList) {
    if (ImGui::Button("Import current captured PSOs"))
      ImportCurrentCapturedPsosIntoManualList();
    ImGui::SameLine();
    if (ImGui::Button("Clear manual list"))
      ClearManualFadePrimitiveList();
  }

  struct DisplayRow {
    std::uint64_t shader_hash = 0;
    bool manual_enabled = false;
    std::size_t live_application_psos = 0;
    bool v1_match = false;
    std::uint32_t instances = 0;
    std::string consumers;
    FadePrimitiveExecutionTarget target;
    bool active_target = false;
  };
  std::unordered_map<std::uint64_t, std::unordered_set<TracePipelineKey,
      TracePipelineKeyHash>> live_psos;
  std::unordered_set<std::uint64_t> known_v1_hashes;
  {
    std::lock_guard inspection_lock(g_inspection_mutex);
    known_v1_hashes.reserve(g_inspections.size());
    for (const auto& [shader_hash, inspection] : g_inspections) {
      if (inspection.success && !inspection.fade_primitive.instances.empty())
        known_v1_hashes.insert(shader_hash);
    }
  }
  std::size_t live_matched_application_psos = 0;
  {
    std::lock_guard lock(g_trace_mutex);
    for (const auto& [key, pipeline] : g_trace_pipelines) {
      live_psos[pipeline.shader_hash].insert(key);
      if (known_v1_hashes.contains(pipeline.shader_hash))
        ++live_matched_application_psos;
    }
  }
  std::unordered_map<std::uint64_t, bool> manual_hashes;
  std::unordered_map<std::uint64_t, FadePrimitiveExecutionTarget> targets;
  std::string status;
  {
    std::lock_guard lock(g_fade_primitive_execution_mutex);
    manual_hashes = g_manual_fade_primitive_hashes;
    targets = g_fade_primitive_execution_targets;
    status = g_fade_primitive_execution_status;
  }
  std::unordered_set<std::uint64_t> display_hashes;
  if (mode == TargetBypassMode::ManualShaderList) {
    for (const auto& [shader_hash, enabled_hash] : manual_hashes) {
      (void)enabled_hash;
      display_hashes.insert(shader_hash);
    }
  } else {
    for (const auto& [shader_hash, target] : targets) {
      (void)target;
      display_hashes.insert(shader_hash);
    }
  }

  std::vector<DisplayRow> rows;
  rows.reserve(display_hashes.size());
  for (const std::uint64_t shader_hash : display_hashes) {
    DisplayRow row;
    row.shader_hash = shader_hash;
    row.manual_enabled = manual_hashes.contains(shader_hash) &&
        manual_hashes[shader_hash];
    row.live_application_psos = live_psos[shader_hash].size();
    {
      std::lock_guard inspection_lock(g_inspection_mutex);
      if (const auto inspection = g_inspections.find(shader_hash);
          inspection != g_inspections.end() && inspection->second.success &&
          !inspection->second.fade_primitive.instances.empty()) {
        row.v1_match = true;
        row.instances = static_cast<std::uint32_t>(
            inspection->second.fade_primitive.instances.size());
        row.consumers = FadePrimitiveConsumers(inspection->second.fade_primitive);
      }
    }
    if (const auto target = targets.find(shader_hash); target != targets.end()) {
      row.target = target->second;
      row.active_target = true;
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(), [](const DisplayRow& left,
                                         const DisplayRow& right) {
    return left.shader_hash < right.shader_hash;
  });

  const auto replacement_counts = g_fade_primitive_execution_replacements.Sizes();
  std::size_t active_shader_count = 0;
  std::size_t active_instance_count = 0;
  for (const auto& row : rows) {
    const bool active = mode == TargetBypassMode::AllVerifiedV1
        ? row.v1_match
        : row.manual_enabled && row.v1_match;
    if (!active) continue;
    ++active_shader_count;
    active_instance_count += row.instances;
  }
  ImGui::Text("Prepared shader cache=%zu | active shaders=%zu | active primitive instances=%zu | replacement PSOs live=%zu",
      targets.size(), active_shader_count, active_instance_count,
      replacement_counts.second);
  ImGui::Text("Soak: v1 matched=%zu | patched prepared=%llu | live matched application PSOs=%zu | replacements created=%llu destroyed=%llu failed=%llu",
      known_v1_hashes.size(), static_cast<unsigned long long>(
          g_fade_primitive_execution_shaders_prepared.load(
              std::memory_order_relaxed)), live_matched_application_psos,
      static_cast<unsigned long long>(
          g_fade_primitive_execution_replacements_created.load(
              std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_fade_primitive_execution_replacements_destroyed.load(
              std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_fade_primitive_execution_replacements_failed.load(
              std::memory_order_relaxed)));
  ImGui::Text("Bind hits: replacement=%llu | original while OFF=%llu",
      static_cast<unsigned long long>(g_fade_primitive_execution_bind_hits.load(
          std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_fade_primitive_execution_original_bind_hits.load(
              std::memory_order_relaxed)));
  ImGui::TextDisabled("%s", status.c_str());

  if (mode != TargetBypassMode::ManualShaderList) return;
  if (!ImGui::BeginTable("manual_v1_shader_groups", 10,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
          ImVec2(0.0f, 420.0f)))
    return;
  ImGui::TableSetupColumn("Enabled");
  ImGui::TableSetupColumn("Pixel shader hash");
  ImGui::TableSetupColumn("Live application PSOs");
  ImGui::TableSetupColumn("v1 match");
  ImGui::TableSetupColumn("Instances");
  ImGui::TableSetupColumn("Consumers");
  ImGui::TableSetupColumn("Replacement live");
  ImGui::TableSetupColumn("Created / failed");
  ImGui::TableSetupColumn("Replacement binds");
  ImGui::TableSetupColumn("Status");
  ImGui::TableHeadersRow();
  for (const auto& row : rows) {
    const std::string hash = Hex64(row.shader_hash);
    ImGui::PushID(hash.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool selected = row.manual_enabled;
    if (ImGui::Checkbox("##manual_enabled", &selected))
      SetManualFadePrimitiveEnabled(row.shader_hash, selected);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(hash.c_str());
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%zu", row.live_application_psos);
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(row.v1_match ? "yes" : "no (not eligible)");
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%u", row.instances);
    ImGui::TableSetColumnIndex(5);
    ImGui::TextUnformatted(row.v1_match ? row.consumers.c_str() : "-");
    ImGui::TableSetColumnIndex(6);
    ImGui::Text("%llu", static_cast<unsigned long long>(
        row.target.live_replacements));
    ImGui::TableSetColumnIndex(7);
    ImGui::Text("%llu / %llu", static_cast<unsigned long long>(
        row.target.replacements_created), static_cast<unsigned long long>(
        row.target.replacements_failed));
    ImGui::TableSetColumnIndex(8);
    ImGui::Text("%llu", static_cast<unsigned long long>(
        row.target.replacement_bind_hits));
    ImGui::TableSetColumnIndex(9);
    ImGui::TextUnformatted(row.target.failure.empty()
        ? (row.v1_match ? "eligible" : "not eligible")
        : row.target.failure.c_str());
    ImGui::PopID();
  }
  ImGui::EndTable();
}

#if !WUWA_TFR_DEVTOOLS
void OnInitPublicDevice(device* owner) {
  OnInitDevice(owner);
  if (g_target_process) g_public_antifade_runtime.OnInitDevice(owner);
}

void OnDestroyPublicDevice(device* owner) {
  if (g_target_process) g_public_antifade_runtime.OnDestroyDevice(owner);
  OnDestroyDevice(owner);
}

void OnInitPublicPipeline(device* owner, pipeline_layout layout,
    std::uint32_t count, const pipeline_subobject* subobjects,
    pipeline application_pipeline) {
  if (g_target_process) g_public_antifade_runtime.OnInitPipeline(
      owner, layout, count, subobjects, application_pipeline);
}

void OnDestroyPublicPipeline(device* owner, pipeline application_pipeline) {
  if (g_target_process)
    g_public_antifade_runtime.OnDestroyPipeline(owner, application_pipeline);
}

void OnBindPublicPipeline(command_list* list, pipeline_stage stages,
    pipeline application_pipeline) {
  if (g_target_process)
    g_public_antifade_runtime.OnBindPipeline(list, stages, application_pipeline);
}
#endif

void DrawTraceOverlay() {
  if (!ImGui::CollapsingHeader(
          "Runtime differential trace", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  ImGui::TextDisabled(
      "Observation only: recorded command lists are counted when executed.");
  ImGui::TextWrapped(
      "Capture the same idle scene at normal distance, with partial camera "
      "fade, then with full camera fade. Move only along the camera-character "
      "axis and stop before each capture.");
  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderInt("Window length (frames)",
      &g_trace_window_length, 30, 600);

  const std::uint64_t active_token =
      g_trace_token.load(std::memory_order_acquire);
  const TraceWindow active = active_token == 0
      ? TraceWindow::None
      : TraceWindowFromToken(active_token);
  const bool is_capturing = active_token != 0;
  std::array<std::uint32_t, kTraceWindowCount> captured_frames{};
  for (std::size_t i = 0; i < kTraceWindowCount; ++i)
    captured_frames[i] =
        g_trace_captured_frames[i].load(std::memory_order_relaxed);
  std::array<bool, kTraceWindowCount> capture_complete{};
  {
    std::lock_guard lock(g_trace_mutex);
    capture_complete = g_trace_capture_complete;
  }
  ImGui::BeginDisabled(is_capturing);
  if (ImGui::Button("Capture normal window"))
    StartTraceWindow(TraceWindow::Normal);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing || !capture_complete[0]);
  if (ImGui::Button("Capture partial fade"))
    StartTraceWindow(TraceWindow::PartialFade);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing || !capture_complete[1]);
  if (ImGui::Button("Capture full fade"))
    StartTraceWindow(TraceWindow::FullFade);
  ImGui::EndDisabled();

  if (is_capturing) {
    const char* label = active == TraceWindow::Normal
        ? "normal"
        : (active == TraceWindow::PartialFade ? "partial fade" : "full fade");
    ImGui::Text("Capturing %s: %u frames remaining", label,
        g_trace_frames_remaining.load(std::memory_order_relaxed));
  } else {
    ImGui::TextUnformatted("Capture state: idle");
  }

  if (ImGui::Button("Clear comparison")) ClearTraceComparison();
  ImGui::SameLine();
  ImGui::BeginDisabled(
      is_capturing ||
      !std::all_of(capture_complete.begin(), capture_complete.end(),
          [](bool complete) { return complete; }));
  if (ImGui::Button("Export trace reports")) {
    g_trace_ui_status = WriteTraceReport()
        ? "exported runtime, concrete-submission and lifecycle-ambiguities TSVs"
        : "export failed: finish all three windows and check DumpPath";
  }
  ImGui::EndDisabled();
  if (!g_trace_ui_status.empty())
    ImGui::TextDisabled("%s", g_trace_ui_status.c_str());

  std::size_t mapped_pipelines = 0;
  std::size_t observed_shader_rows = 0;
  std::size_t tracked_concrete_rows = 0;
  bool concrete_capacity_exceeded = false;
  bool identity_capacity_exceeded = false;
  std::uint64_t incarnation_prunes = 0;
  TracePsoAmbiguityDiagnostics::Snapshot pso_ambiguities;
  TraceResourceAmbiguityDiagnostics::Snapshot resource_ambiguities;
  {
    std::lock_guard lock(g_trace_mutex);
    mapped_pipelines = g_trace_pipelines.size();
    observed_shader_rows = static_cast<std::size_t>(std::count_if(
        g_trace_shaders.begin(), g_trace_shaders.end(), [](const auto& entry) {
          return std::any_of(entry.second.windows.begin(),
              entry.second.windows.end(), [](const auto& window) {
                return window.draws != 0;
              });
        }));
    tracked_concrete_rows = g_concrete_trace.size();
    concrete_capacity_exceeded = g_concrete_trace_capacity_exceeded;
    identity_capacity_exceeded = g_trace_identity_capacity_exceeded;
    incarnation_prunes = g_trace_incarnation_prunes;
    pso_ambiguities = g_trace_pso_lifecycle_ambiguities.GetSnapshot();
    resource_ambiguities =
        g_trace_resource_lifecycle_ambiguities.GetSnapshot();
  }
  ImGui::BeginDisabled(is_capturing ||
      !std::all_of(capture_complete.begin(), capture_complete.end(),
          [](bool complete) { return complete; }));
  if (ImGui::Button("Generate frozen Draw list"))
    GenerateFilteredConcreteRows();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Clear filtered list")) {
    std::lock_guard lock(g_trace_mutex);
    g_filtered_concrete_rows.clear();
    g_trace_investigation_view =
        wuwa_tfr::TraceInvestigationView::NormalPartialNoDiscard;
    g_trace_candidate_range_initialized = false;
    g_pinned_draw_route.reset();
    g_manual_test_draws.clear();
    ResetShaderFamilySkipAccountingLocked();
    g_manual_test_record_hits.store(0, std::memory_order_relaxed);
    g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
    g_trace_ui_status = "filtered list cleared";
  }
  const auto& concrete_rows = g_filtered_concrete_rows;
  auto investigation_view = g_trace_investigation_view;
  int investigation_view_index =
      static_cast<int>(investigation_view);
  constexpr const char* kInvestigationViews[] = {
      "N + P + no discard (default investigation population)",
      "Show All N + P (submission search clue only)",
      "N + P + discard (search clue only)",
      "N + P + strict spatial dither (search clue only)",
      "Normal-only rim shortlist (manual localization only)",
      "Show All raw investigation rows",
  };
  ImGui::SameLine();
  if (ImGui::Combo("Investigation view (search clues only)",
          &investigation_view_index, kInvestigationViews,
          static_cast<int>(std::size(kInvestigationViews)))) {
    investigation_view = static_cast<wuwa_tfr::TraceInvestigationView>(
        investigation_view_index);
    std::lock_guard lock(g_trace_mutex);
    g_trace_investigation_view = investigation_view;
    g_trace_candidate_range_initialized = false;
    g_manual_test_draws.clear();
    ResetShaderFamilySkipAccountingLocked();
    g_manual_test_record_hits.store(0, std::memory_order_relaxed);
    g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
  }
  std::vector<std::size_t> displayed_indices;
  displayed_indices.reserve(concrete_rows.size());
  for (std::size_t i = 0; i < concrete_rows.size(); ++i) {
    const bool visible =
        investigation_view == wuwa_tfr::TraceInvestigationView::
                NormalOnlyRimShortlist
            ? IsNormalOnlyRimSkipRow(concrete_rows[i])
            : wuwa_tfr::TraceInvestigationVisible(
                  concrete_rows[i].key, concrete_rows[i].windows,
                  investigation_view,
                  concrete_rows[i].investigation_has_discard,
                  concrete_rows[i].investigation_strict_spatial_dither);
    if (visible)
      displayed_indices.push_back(i);
  }
  std::unordered_set<wuwa_tfr::TraceConcreteDrawKey,
      wuwa_tfr::TraceConcreteDrawKeyHash> manual_selection;
  std::optional<wuwa_tfr::TraceDrawRouteKey> pinned_draw_route;
  wuwa_tfr::TraceCandidateRange candidate_range;
  bool shader_family_mode = false;
  std::unordered_set<std::uint64_t> shader_family_skip_hashes;
  std::size_t shader_family_skip_requested_rows = 0;
  std::size_t shader_family_skip_requested_psos = 0;
  {
    std::lock_guard lock(g_trace_mutex);
    if (!g_trace_candidate_range_initialized ||
        g_trace_candidate_range.begin > g_trace_candidate_range.end ||
        g_trace_candidate_range.end > displayed_indices.size()) {
      g_trace_candidate_range =
          wuwa_tfr::FullTraceCandidateRange(displayed_indices.size());
      g_trace_candidate_range_initialized = true;
      g_manual_test_draws.clear();
      ResetShaderFamilySkipAccountingLocked();
      g_manual_test_record_hits.store(0, std::memory_order_relaxed);
      g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
    }
    candidate_range = g_trace_candidate_range;
    manual_selection = g_manual_test_draws;
    pinned_draw_route = g_pinned_draw_route;
    shader_family_mode = g_shader_family_investigation_mode;
    shader_family_skip_hashes = g_shader_family_skip_hashes;
    shader_family_skip_requested_rows = g_shader_family_skip_requested_rows;
    shader_family_skip_requested_psos = g_shader_family_skip_requested_psos;
  }

  if (ImGui::Checkbox("Shader-family investigation mode (frozen rows only)",
          &shader_family_mode)) {
    std::lock_guard lock(g_trace_mutex);
    g_shader_family_investigation_mode = shader_family_mode;
  }

  if (shader_family_mode) {
    auto shader_groups = BuildShaderFamilyGroups(concrete_rows);
    if (investigation_view ==
        wuwa_tfr::TraceInvestigationView::NormalOnlyRimShortlist) {
      std::erase_if(shader_groups,
          [&concrete_rows](const ShaderFamilyGroup& group) {
            return !std::all_of(
                group.row_indices.begin(), group.row_indices.end(),
                [&concrete_rows](std::size_t row_index) {
                  return IsNormalOnlyRimSkipRow(concrete_rows[row_index]);
                });
          });
    }
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f),
        "Shader-family investigation: %zu groups from %zu frozen rows",
        shader_groups.size(), concrete_rows.size());
    ImGui::TextDisabled(
        "Group SKIP selects only live, skip-eligible exact Draw/PSO/pass rows already in this frozen population. It is an association shortcut, not shader or fade evidence.");
    constexpr ImGuiTableFlags kShaderFamilyTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("shader_family_groups", 15,
            kShaderFamilyTableFlags, ImVec2(0.0f, 500.0f))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("SKIP group");
      ImGui::TableSetupColumn("Pixel shader hash");
      ImGui::TableSetupColumn("Concrete rows");
      ImGui::TableSetupColumn("Application PSOs");
      ImGui::TableSetupColumn("Routes");
      ImGui::TableSetupColumn("Normal submit");
      ImGui::TableSetupColumn("Partial submit");
      ImGui::TableSetupColumn("Full submit");
      ImGui::TableSetupColumn("All N=0 P>0 F>0");
      ImGui::TableSetupColumn("Discard");
      ImGui::TableSetupColumn("Strict dither");
      ImGui::TableSetupColumn("Ambiguous dither");
      ImGui::TableSetupColumn("Blend");
      ImGui::TableSetupColumn("A2C");
      ImGui::TableSetupColumn("Expand rows");
      ImGui::TableHeadersRow();

      for (std::size_t group_index = 0; group_index < shader_groups.size();
           ++group_index) {
        const auto& group = shader_groups[group_index];
        ImGui::PushID(static_cast<int>(group_index));
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        bool group_selected =
            shader_family_skip_hashes.contains(group.shader_hash);
        ImGui::BeginDisabled(is_capturing);
        if (ImGui::Checkbox("##skip_shader_family", &group_selected)) {
          std::lock_guard lock(g_trace_mutex);
          SetShaderFamilySkipLocked(concrete_rows, group, group_selected);
          manual_selection = g_manual_test_draws;
          shader_family_skip_hashes = g_shader_family_skip_hashes;
          shader_family_skip_requested_rows =
              g_shader_family_skip_requested_rows;
          shader_family_skip_requested_psos =
              g_shader_family_skip_requested_psos;
        }
        ImGui::EndDisabled();
        ImGui::TableSetColumnIndex(1);
        const bool expanded = ImGui::TreeNodeEx(Hex64(group.shader_hash).c_str(),
            ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::TableSetColumnIndex(2);
        ImGui::Text("%zu", group.concrete_row_count);
        ImGui::TableSetColumnIndex(3);
        ImGui::Text("%zu", group.application_psos.size());
        ImGui::TableSetColumnIndex(4);
        ImGui::Text("%zu", group.routes.size());
        ImGui::TableSetColumnIndex(5);
        ImGui::Text("%llu", static_cast<unsigned long long>(group.submissions[0]));
        ImGui::TableSetColumnIndex(6);
        ImGui::Text("%llu", static_cast<unsigned long long>(group.submissions[1]));
        ImGui::TableSetColumnIndex(7);
        ImGui::Text("%llu", static_cast<unsigned long long>(group.submissions[2]));
        const auto yes_no = [](bool value) { return value ? "yes" : "no"; };
        ImGui::TableSetColumnIndex(8);
        ImGui::TextUnformatted(yes_no(group.all_fade_transition_candidates));
        ImGui::TableSetColumnIndex(9);
        ImGui::TextUnformatted(yes_no(group.any_discard));
        ImGui::TableSetColumnIndex(10);
        ImGui::TextUnformatted(yes_no(group.any_strict_spatial_dither));
        ImGui::TableSetColumnIndex(11);
        ImGui::TextUnformatted(yes_no(group.any_ambiguous_spatial_dither));
        ImGui::TableSetColumnIndex(12);
        ImGui::TextUnformatted(yes_no(group.any_blend));
        ImGui::TableSetColumnIndex(13);
        ImGui::TextUnformatted(yes_no(group.any_alpha_to_coverage));
        ImGui::TableSetColumnIndex(14);
        ImGui::TextUnformatted(expanded ? "open" : "click hash");

        if (expanded) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(1);
          if (ImGui::BeginTable("shader_family_rows", 10,
                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Frozen row");
            ImGui::TableSetupColumn("PSO handle");
            ImGui::TableSetupColumn("PSO id");
            ImGui::TableSetupColumn("Geometry");
            ImGui::TableSetupColumn("Exact pass");
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Normal");
            ImGui::TableSetupColumn("Partial");
            ImGui::TableSetupColumn("Full");
            ImGui::TableSetupColumn("SKIP eligible");
            ImGui::TableHeadersRow();
            for (const auto row_index : group.row_indices) {
              const auto& row = concrete_rows[row_index];
              ImGui::TableNextRow();
              ImGui::TableSetColumnIndex(0);
              ImGui::Text("%zu", row_index);
              ImGui::TableSetColumnIndex(1);
              ImGui::TextUnformatted(
                  Hex64(row.pipeline.application_pipeline).c_str());
              ImGui::TableSetColumnIndex(2);
              ImGui::Text("%llu",
                  static_cast<unsigned long long>(row.key.pso_incarnation));
              ImGui::TableSetColumnIndex(3);
              ImGui::TextUnformatted(Hex64(row.geometry_fingerprint).c_str());
              ImGui::TableSetColumnIndex(4);
              ImGui::TextUnformatted(Hex64(row.key.pass_fingerprint).c_str());
              ImGui::TableSetColumnIndex(5);
              ImGui::TextUnformatted(TraceDrawKindName(row.key.geometry.kind));
              ImGui::TableSetColumnIndex(6);
              ImGui::Text("%llu", static_cast<unsigned long long>(
                  row.windows[0].command_list_submissions));
              ImGui::TableSetColumnIndex(7);
              ImGui::Text("%llu", static_cast<unsigned long long>(
                  row.windows[1].command_list_submissions));
              ImGui::TableSetColumnIndex(8);
              ImGui::Text("%llu", static_cast<unsigned long long>(
                  row.windows[2].command_list_submissions));
              ImGui::TableSetColumnIndex(9);
              ImGui::TextUnformatted(yes_no(row.skip_eligible));
            }
            ImGui::EndTable();
          }
          ImGui::TreePop();
        }
        ImGui::PopID();
      }
      ImGui::EndTable();
    }
    if (!shader_family_skip_hashes.empty()) {
      ImGui::Text(
          "Shader-family SKIP: groups=%zu | requested live concrete PSOs=%zu | rows=%zu | actual suppressed command hits=%llu",
          shader_family_skip_hashes.size(),
          shader_family_skip_requested_psos,
          shader_family_skip_requested_rows,
          static_cast<unsigned long long>(
              g_manual_test_record_hits.load(std::memory_order_relaxed)));
    } else {
      ImGui::TextDisabled("Shader-family SKIP is off.");
    }
    return;
  }

  const auto replace_manual_range =
      [&](wuwa_tfr::TraceCandidateRange selected_range) {
        std::lock_guard lock(g_trace_mutex);
        g_manual_test_draws.clear();
        ResetShaderFamilySkipAccountingLocked();
        for (std::size_t position = selected_range.begin;
             position < selected_range.end; ++position) {
          const auto& row = concrete_rows[displayed_indices[position]];
          if (row.skip_eligible) g_manual_test_draws.insert(row.key);
        }
        g_manual_test_record_hits.store(0, std::memory_order_relaxed);
        g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
        manual_selection = g_manual_test_draws;
      };
  const auto narrow_to_range =
      [&](wuwa_tfr::TraceCandidateRange narrowed_range) {
        std::lock_guard lock(g_trace_mutex);
        g_trace_candidate_range = narrowed_range;
        g_trace_candidate_range_initialized = true;
        candidate_range = narrowed_range;
        g_manual_test_draws.clear();
        ResetShaderFamilySkipAccountingLocked();
        g_manual_test_record_hits.store(0, std::memory_order_relaxed);
        g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
        manual_selection.clear();
      };

  const auto first_half =
      wuwa_tfr::FirstTraceCandidateHalf(candidate_range);
  const auto second_half =
      wuwa_tfr::SecondTraceCandidateHalf(candidate_range);
  ImGui::BeginDisabled(is_capturing || first_half.size() == 0);
  if (ImGui::Button("SKIP first half")) replace_manual_range(first_half);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing || second_half.size() == 0);
  if (ImGui::Button("SKIP second half")) replace_manual_range(second_half);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing || candidate_range.size() <= 1);
  if (ImGui::Button("Narrow to first half")) narrow_to_range(first_half);
  ImGui::SameLine();
  if (ImGui::Button("Narrow to second half")) narrow_to_range(second_half);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing);
  if (ImGui::Button("Reset to full population")) {
    narrow_to_range(
        wuwa_tfr::FullTraceCandidateRange(displayed_indices.size()));
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled(
      "Test both halves independently. A positive half means only that at least one Draw in it contributes to recognizable character rendering.");

  std::vector<std::size_t> current_displayed_indices;
  current_displayed_indices.reserve(candidate_range.size());
  for (std::size_t position = candidate_range.begin;
       position < candidate_range.end; ++position)
    current_displayed_indices.push_back(displayed_indices[position]);

  ImGui::BeginDisabled(is_capturing || concrete_rows.empty());
  if (ImGui::Button("Export Current Investigation Range")) {
    g_trace_ui_status = WriteCurrentInvestigationRange(concrete_rows,
        displayed_indices, candidate_range, investigation_view)
        ? "exported current investigation range TSV"
        : "range export failed: check DumpPath";
  }
  ImGui::EndDisabled();

  std::vector<wuwa_tfr::TraceConcreteDrawKey> manual_candidates;
  manual_candidates.reserve(current_displayed_indices.size());
  for (const auto i : current_displayed_indices) {
    if (concrete_rows[i].skip_eligible)
      manual_candidates.push_back(concrete_rows[i].key);
  }

  const auto replace_manual_selection =
      [&](std::optional<wuwa_tfr::TraceConcreteDrawKey> selected) {
        std::lock_guard lock(g_trace_mutex);
        g_manual_test_draws.clear();
        ResetShaderFamilySkipAccountingLocked();
        if (selected) g_manual_test_draws.insert(*selected);
        g_manual_test_record_hits.store(0, std::memory_order_relaxed);
        g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
        manual_selection = g_manual_test_draws;
      };
  ImGui::BeginDisabled(is_capturing || manual_candidates.empty());
  if (ImGui::Button("Previous filtered Draw")) {
    std::size_t index = manual_candidates.size() - 1;
    if (manual_selection.size() == 1) {
      const auto it = std::find(
          manual_candidates.begin(), manual_candidates.end(),
          *manual_selection.begin());
      if (it != manual_candidates.end())
        index = (static_cast<std::size_t>(it - manual_candidates.begin()) +
                    manual_candidates.size() - 1) %
            manual_candidates.size();
    }
    replace_manual_selection(manual_candidates[index]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Next filtered Draw")) {
    std::size_t index = 0;
    if (manual_selection.size() == 1) {
      const auto it = std::find(
          manual_candidates.begin(), manual_candidates.end(),
          *manual_selection.begin());
      if (it != manual_candidates.end())
        index = (static_cast<std::size_t>(it - manual_candidates.begin()) + 1) %
            manual_candidates.size();
    }
    replace_manual_selection(manual_candidates[index]);
  }
  ImGui::SameLine();
  if (ImGui::Button("SKIP current range")) {
    std::lock_guard lock(g_trace_mutex);
    g_manual_test_draws.clear();
    ResetShaderFamilySkipAccountingLocked();
    g_manual_test_draws.insert(
        manual_candidates.begin(), manual_candidates.end());
    g_manual_test_record_hits.store(0, std::memory_order_relaxed);
    g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
    manual_selection = g_manual_test_draws;
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear all")) replace_manual_selection(std::nullopt);
  ImGui::EndDisabled();

  ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f),
      "Group investigation: full population=%zu | current range=%zu",
      displayed_indices.size(), candidate_range.size());
  ImGui::Text(
      "Requested SKIP row count: %zu | actual SKIP hit count: %llu",
      manual_selection.size(),
      static_cast<unsigned long long>(
          g_manual_test_record_hits.load(std::memory_order_relaxed)));

  if (!manual_selection.empty()) {
    ImGui::TextColored(
        ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
        "Association SKIP: %zu concrete Draw/PSO(s); record hits=%llu; skipped=%llu",
        manual_selection.size(),
        static_cast<unsigned long long>(
            g_manual_test_record_hits.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_manual_test_suppressed_commands.load(std::memory_order_relaxed)));
    ImGui::TextDisabled(
        "SKIP tests the selected concrete Draw/PSO/pass row. A visible change must be "
        "judged in-game; it does not turn route status into object identity.");
  } else {
    ImGui::TextDisabled("Association SKIP is off.");
  }

  ImGui::Text(
      "Mapped PSOs: %zu | normal: %u | partial: %u | full: %u",
      mapped_pipelines, captured_frames[0], captured_frames[1],
      captured_frames[2]);
  const auto show_ambiguities = [](const char* event_class,
                                    const auto& snapshot) {
    ImGui::Text(
        "%s lifecycle ambiguities: total=%llu | unique handles=%llu | max/handle=%llu",
        event_class,
        static_cast<unsigned long long>(snapshot.total_events),
        static_cast<unsigned long long>(snapshot.unique_handles),
        static_cast<unsigned long long>(snapshot.max_events_for_one_handle));
  };
  show_ambiguities("PSO", pso_ambiguities);
  show_ambiguities("Resource", resource_ambiguities);
  ImGui::TextDisabled(
      "ResourceView identity changes are descriptor-slot version replacements, not lifecycle ambiguities.");
  ImGui::Text("Incarnation prunes: %llu",
      static_cast<unsigned long long>(incarnation_prunes));
  ImGui::TextDisabled(
      "First 32 unique ambiguous PSO/Resource handles are written to lifecycle-ambiguities.tsv.");
  ImGui::Text(
      "Captured concrete records: %zu | shader search-clue rows: %zu",
      tracked_concrete_rows, observed_shader_rows);
  ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f),
      "Investigation rows shown: %zu | full population: %zu | raw frozen: %zu",
      current_displayed_indices.size(), displayed_indices.size(),
      concrete_rows.size());
  ImGui::TextDisabled(
      "These views are investigation/search clues only, not matcher or "
      "camera-fade evidence. Whole-Draw SKIP remains object/pass association only.");
  if (concrete_capacity_exceeded) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        "Concrete trace capacity reached; route comparison is UNKNOWN. "
        "Clear and repeat the three-window capture.");
  }
  if (identity_capacity_exceeded) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        "Live identity state was pruned; route comparison remains UNKNOWN for "
        "this process. Restart the Dev add-on before a research capture.");
  }
  ImGui::Text(
      "Static DXIL: discard=%llu | strict spatial dither=%llu | ambiguous=%llu",
      static_cast<unsigned long long>(
          g_discard_shader_count.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_strict_spatial_dither_count.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_ambiguous_spatial_dither_count.load(std::memory_order_relaxed)));
  ImGui::TextDisabled(
      "Fade Primitive v1 detector (read-only): shaders=%llu | instances=%llu",
      static_cast<unsigned long long>(
          g_fade_primitive_shader_count.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_fade_primitive_instance_count.load(std::memory_order_relaxed)));
  constexpr ImGuiTableFlags kTableFlags =
      ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
  if (pinned_draw_route) {
    std::array<std::uint64_t, kTraceWindowCount> pinned_submissions{};
    bool all_concrete = true;
    auto route_conclusion = wuwa_tfr::TraceRouteConclusion::Unknown;
    for (const auto& row : concrete_rows) {
      if (!(wuwa_tfr::MakeTraceDrawRoute(row.key) == *pinned_draw_route))
        continue;
      all_concrete &= row.concrete;
      route_conclusion = row.conclusion;
      for (std::size_t i = 0; i < kTraceWindowCount; ++i)
        pinned_submissions[i] += row.windows[i].command_list_submissions;
    }
    ImGui::Text("Pinned binding+pass route submissions: normal=%llu partial=%llu full=%llu",
        static_cast<unsigned long long>(pinned_submissions[0]),
        static_cast<unsigned long long>(pinned_submissions[1]),
        static_cast<unsigned long long>(pinned_submissions[2]));
    const char* route_status = RouteConclusionName(route_conclusion);
    ImGui::Text("Full-window geometry+pass route status: %s", route_status);
    ImGui::TextDisabled(all_concrete
        ? "This is a geometry+exact-pass route observation, never object identity or GPU pixels."
        : "Inconclusive identity: required IA/pass observation is missing or the command is indirect/mesh.");
  }

  if (ImGui::BeginTable("concrete_trace_rows", 14, kTableFlags,
          ImVec2(0.0f, 320.0f))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("SKIP");
    ImGui::TableSetupColumn("Pin");
    ImGui::TableSetupColumn("PSO id");
    ImGui::TableSetupColumn("PSO handle");
    ImGui::TableSetupColumn("Shader hash");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Geometry binding");
    ImGui::TableSetupColumn("Identity complete");
    ImGui::TableSetupColumn("Filter reasons");
    ImGui::TableSetupColumn("Uncertainty");
    ImGui::TableSetupColumn("Normal submit");
    ImGui::TableSetupColumn("Partial submit");
    ImGui::TableSetupColumn("Full submit");
    ImGui::TableSetupColumn("Route status");
    ImGui::TableHeadersRow();

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(current_displayed_indices.size()));
    while (clipper.Step()) {
      for (int display_index = clipper.DisplayStart;
           display_index < clipper.DisplayEnd; ++display_index) {
      const std::size_t i = current_displayed_indices[
          static_cast<std::size_t>(display_index)];
      const auto& row = concrete_rows[i];
      const std::string hash = Hex64(row.pipeline.shader_hash);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      bool selected = manual_selection.contains(row.key);
      ImGui::PushID(static_cast<int>(i));
      ImGui::BeginDisabled(is_capturing || !row.skip_eligible);
      if (ImGui::Checkbox("##skip_concrete", &selected)) {
        std::lock_guard lock(g_trace_mutex);
        if (selected) g_manual_test_draws.insert(row.key);
        else g_manual_test_draws.erase(row.key);
        ResetShaderFamilySkipAccountingLocked();
        g_manual_test_record_hits.store(0, std::memory_order_relaxed);
        g_manual_test_suppressed_commands.store(0, std::memory_order_relaxed);
        manual_selection = g_manual_test_draws;
      }
      ImGui::EndDisabled();
      ImGui::TableSetColumnIndex(1);
      const auto draw_route = wuwa_tfr::MakeTraceDrawRoute(row.key);
      bool pinned = pinned_draw_route && *pinned_draw_route == draw_route;
      ImGui::BeginDisabled(!row.concrete);
      if (ImGui::Checkbox("##pin_draw_route", &pinned)) {
        std::lock_guard lock(g_trace_mutex);
        SetPinnedDrawRouteLocked(pinned
            ? std::optional<wuwa_tfr::TraceDrawRouteKey>(draw_route)
            : std::nullopt);
        pinned_draw_route = g_pinned_draw_route;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%llu", static_cast<unsigned long long>(row.key.pso_incarnation));
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(Hex64(row.pipeline.application_pipeline).c_str());
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(hash.c_str());
      ImGui::TableSetColumnIndex(5);
      ImGui::TextUnformatted(TraceDrawKindName(row.key.geometry.kind));
      ImGui::TableSetColumnIndex(6);
      ImGui::TextUnformatted(Hex64(row.geometry_fingerprint).c_str());
      ImGui::TableSetColumnIndex(7);
      ImGui::TextUnformatted(row.concrete ? "yes" : "no");
      ImGui::TableSetColumnIndex(8);
      ImGui::TextUnformatted(ConcreteFilterReasons(row.filter_reasons).c_str());
      ImGui::TableSetColumnIndex(9);
      ImGui::TextUnformatted(RouteUncertainty(row).c_str());
      ImGui::TableSetColumnIndex(10);
      ImGui::Text("%llu", static_cast<unsigned long long>(row.windows[0].command_list_submissions));
      ImGui::TableSetColumnIndex(11);
      ImGui::Text("%llu", static_cast<unsigned long long>(row.windows[1].command_list_submissions));
      ImGui::TableSetColumnIndex(12);
      ImGui::Text("%llu", static_cast<unsigned long long>(row.windows[2].command_list_submissions));
      ImGui::TableSetColumnIndex(13);
      ImGui::TextUnformatted(RouteConclusionName(row.conclusion));
      }
    }
    ImGui::EndTable();
  }
  ImGui::TextDisabled(
      "The shader-level table and dither topology remain search clues only. "
      "Association SKIP never patches bytecode and is not camera-fade evidence.");
}
#endif

#if !WUWA_TFR_DEVTOOLS
void OnInitPublicDevice(device* owner) {
  OnInitDevice(owner);
  if (g_target_process) g_public_antifade_runtime.OnInitDevice(owner);
}

void OnDestroyPublicDevice(device* owner) {
  if (g_target_process) g_public_antifade_runtime.OnDestroyDevice(owner);
  OnDestroyDevice(owner);
}

void OnInitPublicPipeline(device* owner, pipeline_layout layout,
    std::uint32_t count, const pipeline_subobject* subobjects,
    pipeline application_pipeline) {
  if (g_target_process) g_public_antifade_runtime.OnInitPipeline(
      owner, layout, count, subobjects, application_pipeline);
}

void OnDestroyPublicPipeline(device* owner, pipeline application_pipeline) {
  if (g_target_process)
    g_public_antifade_runtime.OnDestroyPipeline(owner, application_pipeline);
}

void OnBindPublicPipeline(command_list* list, pipeline_stage stages,
    pipeline application_pipeline) {
  if (g_target_process)
    g_public_antifade_runtime.OnBindPipeline(list, stages, application_pipeline);
}
#endif

void DrawOverlay(effect_runtime*) {
  ImGui::TextUnformatted("WuwaTFR");
#if WUWA_TFR_DEVTOOLS
  ImGui::TextUnformatted("Dev: isolated causal bypass experiments available");
  ImGui::TextDisabled(
      "Dev tools are isolated from the Production automatic Transparency Filter runtime.");
#else
  ImGui::TextUnformatted("Automatic verified transparency-filter matching");
  bool antifade_enabled = g_public_antifade_runtime.enabled();
  if (ImGui::Checkbox("Remove transparency filter", &antifade_enabled)) {
    g_public_antifade_runtime.set_enabled(antifade_enabled);
    SaveConfigFlag(L"EnableTFR", antifade_enabled);
  }
#endif

#if WUWA_TFR_DEVTOOLS
  ImGui::Separator();
  ImGui::Text("DXIL callbacks: %llu",
      static_cast<unsigned long long>(
          g_seen_shader_callbacks.load(std::memory_order_relaxed)));
  ImGui::Text("Unique DXIL shaders: %llu",
      static_cast<unsigned long long>(
          g_unique_dxil_shaders.load(std::memory_order_relaxed)));
  ImGui::Text("Disassembly: success=%llu failure=%llu",
      static_cast<unsigned long long>(
          g_disassembly_successes.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          g_disassembly_failures.load(std::memory_order_relaxed)));
  ImGui::Text("Original IR dumps: %llu",
      static_cast<unsigned long long>(
          g_dumped_shaders.load(std::memory_order_relaxed)));

#if WUWA_TFR_DEVTOOLS
  ImGui::Separator();
  DrawFadePrimitiveTargetModes();
  ImGui::Separator();
  DrawTraceOverlay();
#endif
#endif
}

} // namespace

#if WUWA_TFR_DEVTOOLS
extern "C" __declspec(dllexport) const char* NAME =
    "WuwaTFR Dev (isolated bypass experiments)";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Dev-only isolated dither-stage bypass experiments; no Production matcher.";
#else
extern "C" __declspec(dllexport) const char* NAME =
    "WuwaTFR";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Automatic verified camera-proximity transparency removal for Wuthering Waves DX12.";
#endif

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      if (!ResolveAddonDirectory(module)) return FALSE;
      g_target_process = IsWuwaProcess();
#if WUWA_TFR_DEVTOOLS
      g_diagnostic = ConfigFlag(
          L"Diagnostic", EnvFlag(L"WUWA_TFR_DIAGNOSTIC"));
      g_dump = ConfigFlag(L"Dump", EnvFlag(L"WUWA_TFR_DUMP"));
      g_dump_path = ConfigPathValue(L"DumpPath");
#else
      g_public_antifade_runtime.set_dxc_runtime_directory(g_addon_directory);
      g_public_antifade_runtime.set_enabled(ConfigFlag(L"EnableTFR", true));
#endif

      if (!reshade::register_addon(module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_device>(
#if WUWA_TFR_DEVTOOLS
          OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
      reshade::register_event<reshade::addon_event::create_pipeline>(OnCreatePipeline);
#else
          OnInitPublicDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyPublicDevice);
      reshade::register_event<reshade::addon_event::init_pipeline>(OnInitPublicPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnDestroyPublicPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnBindPublicPipeline);
#endif
#if WUWA_TFR_DEVTOOLS
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
#endif
      reshade::register_overlay("WuwaTFR", DrawOverlay);

      if (g_target_process) {
        Log(reshade::log::level::info,
#if WUWA_TFR_DEVTOOLS
            std::string("loaded Dev research build") +
            (g_diagnostic ? "; diagnostic=on" : "; diagnostic=off") +
            (g_dump ? "; dump=all-unique-dxil" : "; dump=off") +
            (g_dump ? "; dump-path=" + g_dump_path.string() : "") +
            "; devtools=compiled" +
            "; config=" + ConfigPath().string());
#else
            std::string("loaded automatic transparency-removal runtime") +
            "; devtools=not-compiled" +
            "; config=" + ConfigPath().string());
#endif
      }
      break;
    }
    case DLL_PROCESS_DETACH:
      // Avoid COM and FreeLibrary cleanup under the loader lock during process
      // termination. A normal explicit unload still unregisters the add-on.
      if (reserved == nullptr) reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
#endif

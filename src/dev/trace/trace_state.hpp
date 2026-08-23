// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only runtime differential trace: data structures and process-wide
// state for the three-window (normal / partial-fade / full-fade) PSO,
// resource, and draw-submission capture used for manual investigation. See
// dev/trace/trace_events.* for the ReShade event handlers that populate this
// state and dev/trace/trace_report.* for the TSV exporters that read it.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <reshade.hpp>

#include "addon_shared.hpp"
#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

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

std::size_t TraceWindowIndex(TraceWindow window) noexcept;
std::uint64_t MakeTraceToken(
    std::uint64_t generation, TraceWindow window) noexcept;
TraceWindow TraceWindowFromToken(std::uint64_t token) noexcept;

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

using TracePsoAmbiguityDiagnostics =
    wuwa_tfr::TraceLifecycleAmbiguityDiagnostics<TracePsoIdentity>;
using TraceResourceAmbiguityDiagnostics =
    wuwa_tfr::TraceLifecycleAmbiguityDiagnostics<TraceResourceIdentity>;

extern std::mutex g_trace_mutex;
extern wuwa_tfr::TraceIncarnationIndex<TracePsoIdentity>
    g_trace_pso_incarnations;
extern wuwa_tfr::TraceIncarnationIndex<TraceResourceIdentity>
    g_trace_resource_incarnations;
extern wuwa_tfr::TraceIncarnationIndex<std::uint64_t>
    g_trace_view_incarnations;
extern TracePsoAmbiguityDiagnostics g_trace_pso_lifecycle_ambiguities;
extern TraceResourceAmbiguityDiagnostics g_trace_resource_lifecycle_ambiguities;
extern std::uint64_t g_trace_lifecycle_event_serial;
extern std::unordered_map<TracePipelineKey, TracePipelineInfo,
    TracePipelineKeyHash> g_trace_pipelines;
extern std::unordered_map<std::uint64_t, ShaderTraceRecord> g_trace_shaders;
extern std::unordered_map<wuwa_tfr::TraceConcreteDrawKey, ConcreteTraceRecord,
    wuwa_tfr::TraceConcreteDrawKeyHash> g_concrete_trace;
extern bool g_concrete_trace_capacity_exceeded;
extern bool g_trace_identity_capacity_exceeded;
extern std::uint64_t g_trace_incarnation_prunes;
extern std::vector<ConcreteTraceRow> g_filtered_concrete_rows;
extern wuwa_tfr::TraceInvestigationView g_trace_investigation_view;
extern wuwa_tfr::TraceCandidateRange g_trace_candidate_range;
extern bool g_trace_candidate_range_initialized;
extern bool g_shader_family_investigation_mode;
extern std::atomic<std::uint64_t> g_trace_token;
extern std::atomic<std::uint32_t> g_trace_frames_remaining;
extern std::unordered_set<wuwa_tfr::TraceConcreteDrawKey,
    wuwa_tfr::TraceConcreteDrawKeyHash> g_manual_test_draws;
extern std::unordered_set<std::uint64_t> g_shader_family_skip_hashes;
extern std::size_t g_shader_family_skip_requested_rows;
extern std::size_t g_shader_family_skip_requested_psos;
extern std::optional<wuwa_tfr::TraceDrawRouteKey> g_pinned_draw_route;
extern std::atomic<std::uint64_t> g_manual_test_record_hits;
extern std::atomic<std::uint64_t> g_manual_test_suppressed_commands;
extern std::uint64_t g_trace_submission_serial;
extern std::array<std::atomic<std::uint32_t>, kTraceWindowCount>
    g_trace_captured_frames;
extern std::atomic<std::uint64_t> g_trace_frame_id;
extern std::uint64_t g_trace_generation;
extern std::uintptr_t g_trace_swapchain;
extern std::array<bool, kTraceWindowCount> g_trace_capture_complete;
extern int g_trace_window_length;
extern std::string g_trace_ui_status;

// --- Small shared helpers used by both trace_events.cpp and trace_report.cpp
// (and, for the hash helpers, by the recipe/experiments modules' fingerprint
// computations) ---

void TraceHashAppend(
    std::uint64_t& hash, const void* data, std::size_t size) noexcept;

template <typename T>
void TraceHashValue(std::uint64_t& hash, const T& value) noexcept {
  TraceHashAppend(hash, &value, sizeof(value));
}

std::uint64_t TraceRootConstantEntryHash(
    const RootConstantKey& key, std::uint32_t value) noexcept;
void EnsureTraceLayout(CommandListTrace& trace, std::uint64_t layout) noexcept;
bool HasPixelStage(reshade::api::shader_stage stages) noexcept;

struct ActiveTraceResource {
  std::uint64_t incarnation = 0;
  bool dynamic_contents = false;
};

ActiveTraceResource ActiveResourceIncarnationLocked(
    DeviceIdentity device, reshade::api::resource handle);
std::uint64_t ActiveViewIncarnationLocked(
    DeviceIdentity device, reshade::api::resource_view handle);
std::uint64_t PassFingerprint(
    const std::vector<std::uint64_t>& render_targets,
    std::uint64_t depth_stencil);
wuwa_tfr::TraceGeometryKey MakeTraceGeometry(
    const CommandListTrace& trace,
    wuwa_tfr::TraceDrawKind kind,
    std::array<std::uint64_t, 5> arguments);
const char* TraceDrawKindName(wuwa_tfr::TraceDrawKind kind) noexcept;
const char* RouteConclusionName(
    wuwa_tfr::TraceRouteConclusion conclusion) noexcept;
std::string ConcreteFilterReasons(std::uint32_t reasons);
std::string RouteUncertainty(const ConcreteTraceRow& row);
std::string TraceIdentityText(const TracePsoIdentity& identity);
std::string TraceIdentityText(const TraceResourceIdentity& identity);
void ResetLifecycleAmbiguityDiagnosticsLocked();
TracePipelineInfo DescribeTracePipeline(
    std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    std::uint64_t shader_hash);

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/manual_capture.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

#include <imgui.h>

#include "dev/capture/fade_control_runtime.hpp"
#include "dev/dev_inspection.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"
#include "dev/trace/trace_report.hpp"
#include "dev/trace/trace_state.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

namespace {

// All state below is either protected by g_trace_mutex (see
// dev/trace/trace_state.hpp, reused here per the task's concurrency
// guidance) or only ever touched from the ImGui render thread, matching how
// dev/trace/trace_report.cpp's g_trace_ui_status is handled.
ManualCaptureAccumulator g_manual_capture;
std::uint64_t g_manual_capture_generation = 0;
std::uint64_t g_manual_capture_frame_counter = 0;
std::uintptr_t g_manual_capture_swapchain = 0;

// Fast unlocked check so an idle manual capture costs nothing on the hot
// execute_command_list/present path; re-checked under the lock before any
// state mutation.
std::atomic<bool> g_manual_capture_active{false};

std::string g_manual_capture_status = "idle";
std::string g_manual_capture_last_export;

// UI-thread-only (the checkbox and StartManualCapture() both run on the
// ImGui render thread, same as g_dev_antifade_runtime's toggle elsewhere in
// Dev): the pending choice for the *next* session. Start() snapshots this
// into the session itself, which is the value that actually governs
// filtering -- this variable only ever feeds a new Start().
bool g_manual_capture_filter_pending_verified_only = true;

const char* ShaderFilterName(ManualCaptureShaderFilter filter) {
  return filter == ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly
      ? "verified_fade_primitive_only"
      : "all_observed_pixel_shaders";
}

std::string SerializeVertexBindings(
    const std::vector<wuwa_tfr::TraceVertexBinding>& bindings) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const auto& binding = bindings[i];
    if (i != 0) stream << ';';
    stream << binding.slot << ':' << binding.resource_incarnation << ':'
           << binding.offset << ':' << binding.stride << ':'
           << static_cast<int>(binding.dynamic_contents);
  }
  return stream.str();
}

std::string SerializeIndexBinding(
    const std::optional<wuwa_tfr::TraceIndexBinding>& index_buffer) {
  if (!index_buffer) return "-";
  std::ostringstream stream;
  stream << index_buffer->resource_incarnation << ':' << index_buffer->offset
         << ':' << index_buffer->index_size << ':'
         << static_cast<int>(index_buffer->dynamic_contents);
  return stream.str();
}

bool WriteManualCaptureExport(const ManualCaptureSnapshot& snapshot,
    const std::string& timestamp, std::filesystem::path& out_path) {
  const auto directory = DumpDir();
  if (directory.empty()) return false;
  // LocalExportTimestamp() only has second precision, so two captures
  // stopped within the same second need a numbered suffix to avoid
  // silently overwriting each other via std::ios::trunc below.
  const std::string filename = AllocateExportFilename(
      "manual-capture-" + timestamp, ".tsv",
      [&directory](const std::string& candidate) {
        return std::filesystem::exists(directory / candidate);
      });
  out_path = directory / filename;

  std::ofstream report(out_path, std::ios::binary | std::ios::trunc);
  if (!report) return false;

  report << "format\twuwa_tfr_manual_execution_capture_v2\n";
  report << "capture_type\tmanual_user_delimited_session\n";
  report << "session_id\t" << snapshot.session_id << '\n';
  report << "start_frame\t" << snapshot.start_frame << '\n';
  report << "end_frame\t" << snapshot.end_frame << '\n';
  report << "captured_presents\t" << snapshot.captured_presents << '\n';
  report << "record_count\t" << snapshot.records.size() << '\n';
  report << "capacity_exceeded\t"
         << static_cast<int>(snapshot.capacity_exceeded) << '\n';
  report << "shader_filter\t" << ShaderFilterName(snapshot.shader_filter)
         << '\n';
  report << "export_timestamp_local\t" << timestamp << '\n';
  report << "membership_boundary\t"
      "command_list_submitted_while_capturing_not_recording_time\n";
  report << "record_identity\t"
      "pso_incarnation_plus_geometry_plus_pass_fingerprint_same_identity_as_"
      "the_differential_trace_concrete_draw_key_never_shader_hash_alone\n";
  report << "swapchain_policy\t"
      "first_present_observed_after_start_capture_all_other_swapchains_"
      "ignored\n";
  report << "pso_identity_semantics\t"
      "application_pso_is_the_application_pipeline_handle_pso_incarnation_"
      "disambiguates_handle_reuse_pso_fingerprint_is_observed_pipeline_"
      "creation_identity_pixel_shader_hash_is_the_original_dxil_pixel_"
      "shader\n";
  report << "root_constant_fingerprint_meaning\t"
      "observed_root_constant_state_fingerprint\n";
  report << "pushed_cbv_fingerprint_meaning\t"
      "accumulated_binding_event_fingerprint_not_stable_state_not_"
      "constant_buffer_contents\n";
  report << "descriptor_table_fingerprint_meaning\t"
      "accumulated_binding_event_fingerprint_not_stable_state_not_"
      "constant_buffer_contents\n";
  report << "binding_summary_semantics\t"
      "first_last_fingerprint_and_changed_flag_across_every_draw_"
      "aggregated_into_this_route_not_a_single_draws_binding_state\n";
  report << "pass_observation\t"
      "pass_observed_marks_exact_render_pass_proof_per_row_an_unobserved_"
      "pass_is_never_shown_as_proven_identity\n";
  report << "direct_args\tvertex_count_instance_count_first_vertex_"
      "first_instance_unused\n";
  report << "indexed_args\tindex_count_instance_count_first_index_"
      "vertex_offset_bits_first_instance\n";
  report << "mesh_args\tgroup_x_group_y_group_z_unused_unused\n";
  report << "indirect_args\tunknown_use_indirect_columns\n";
  report << "limitation\t"
      "cpu_queue_submission_observation_only_not_gpu_completion_evidence_"
      "not_proof_of_visible_pixels_not_proof_of_fade_causality\n";

  report << "device\tapplication_pso\tpso_incarnation\tpso_fingerprint"
      "\tpso_context_hash\tpixel_shader_hash"
      "\tdraw_kind\tgeometry_fingerprint\targ0\targ1\targ2\targ3\targ4"
      "\ttopology\tvertex_bindings\tindex_binding"
      "\tindirect_argument_resource\tindirect_offset"
      "\tindirect_declared_count\tindirect_stride"
      "\tpass_fingerprint\tpass_observed"
      "\tobserved_bindings"
      "\troot_constant_first_fingerprint\troot_constant_last_fingerprint"
      "\troot_constant_changed"
      "\tpushed_cbv_first_fingerprint\tpushed_cbv_last_fingerprint"
      "\tpushed_cbv_changed"
      "\tdescriptor_table_first_fingerprint"
      "\tdescriptor_table_last_fingerprint\tdescriptor_table_changed"
      "\trt0_blend\talpha_to_coverage\tdepth_test\tdepth_write"
      "\trender_target_count\tsample_count"
      "\tcommands\tsubmissions\tfirst_frame\tlast_frame\n";

  for (const auto& [key, record] : snapshot.records) {
    report << Hex64(record.pipeline.device) << '\t'
           << Hex64(record.pipeline.application_pso) << '\t'
           << record.pipeline.pso_incarnation << '\t'
           << Hex64(record.pipeline.pso_fingerprint) << '\t'
           << Hex64(record.pipeline.pso_context_hash) << '\t'
           << Hex64(record.pipeline.pixel_shader_hash) << '\t'
           << TraceDrawKindName(key.geometry.kind) << '\t'
           << Hex64(static_cast<std::uint64_t>(
                  wuwa_tfr::TraceGeometryKeyHash{}(key.geometry)));
    for (const auto argument : key.geometry.arguments)
      report << '\t' << argument;
    report << '\t' << key.geometry.topology << '\t'
           << SerializeVertexBindings(key.geometry.vertex_buffers) << '\t'
           << SerializeIndexBinding(key.geometry.index_buffer) << '\t'
           << key.geometry.indirect_resource_incarnation << '\t'
           << key.geometry.indirect_offset << '\t'
           << key.geometry.indirect_declared_count << '\t'
           << key.geometry.indirect_stride << '\t'
           << Hex64(key.pass_fingerprint) << '\t'
           << static_cast<int>(
                  (key.geometry.observations & wuwa_tfr::TraceObservedPass) !=
                  0)
           << '\t' << static_cast<int>(record.observed_bindings) << '\t'
           << Hex64(record.root_constants.first_fingerprint) << '\t'
           << Hex64(record.root_constants.last_fingerprint) << '\t'
           << static_cast<int>(record.root_constants.changed) << '\t'
           << Hex64(record.pushed_cbvs.first_fingerprint) << '\t'
           << Hex64(record.pushed_cbvs.last_fingerprint) << '\t'
           << static_cast<int>(record.pushed_cbvs.changed) << '\t'
           << Hex64(record.descriptor_tables.first_fingerprint) << '\t'
           << Hex64(record.descriptor_tables.last_fingerprint) << '\t'
           << static_cast<int>(record.descriptor_tables.changed) << '\t'
           << static_cast<int>(record.pipeline.rt0_blend) << '\t'
           << static_cast<int>(record.pipeline.alpha_to_coverage) << '\t'
           << static_cast<int>(record.pipeline.depth_test) << '\t'
           << static_cast<int>(record.pipeline.depth_write) << '\t'
           << record.pipeline.render_target_count << '\t'
           << record.pipeline.sample_count << '\t' << record.commands << '\t'
           << record.submissions << '\t' << record.first_frame << '\t'
           << record.last_frame << '\n';
  }
  report.flush();
  return static_cast<bool>(report);
}

// Fully verified: the shader has a successful inspection carrying at least
// one fully verified Fade Primitive v1 instance. Reuses the existing Dev
// inspection cache as the source of truth (dev/dev_inspection.hpp) instead
// of running the matcher again here -- same success criterion the read-only
// diagnostics panel already uses (dev/dev_overlay.cpp's
// DrawFadePrimitiveTargetModes).
bool IsVerifiedFadePrimitiveShaderLocked(std::uint64_t shader_hash) {
  const auto it = g_inspections.find(shader_hash);
  return it != g_inspections.end() && it->second.inspection_succeeded &&
      !it->second.fade_instances.empty();
}

}  // namespace

void OnManualCaptureExecute(command_queue*, command_list* cmd_list) {
  if (!g_manual_capture_active.load(std::memory_order_acquire) || !cmd_list)
    return;
  const auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace ||
      (trace->recorded_draws.empty() &&
          !trace->recorded_draw_capacity_exceeded))
    return;

  // Snapshot the session's fixed filter mode without holding g_trace_mutex
  // across the g_inspection_mutex lookups below -- lock order must never
  // nest g_inspection_mutex inside g_trace_mutex.
  ManualCaptureShaderFilter filter_mode;
  {
    std::lock_guard lock(g_trace_mutex);
    if (g_manual_capture.state() != ManualCaptureState::Capturing) return;
    filter_mode = g_manual_capture.active().shader_filter;
  }

  std::unordered_set<std::uint64_t> eligible_shader_hashes;
  const bool filter_active =
      filter_mode == ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly;
  if (filter_active) {
    std::unordered_set<std::uint64_t> observed_shader_hashes;
    observed_shader_hashes.reserve(trace->recorded_draws.size());
    for (const auto& [draw_key, draw] : trace->recorded_draws)
      observed_shader_hashes.insert(draw.pipeline.shader_hash);

    std::lock_guard inspection_lock(g_inspection_mutex);
    for (const auto shader_hash : observed_shader_hashes) {
      if (IsVerifiedFadePrimitiveShaderLocked(shader_hash))
        eligible_shader_hashes.insert(shader_hash);
    }
  }

  std::lock_guard lock(g_trace_mutex);
  if (g_manual_capture.state() != ManualCaptureState::Capturing) return;
  if (trace->recorded_draw_capacity_exceeded)
    g_manual_capture.MarkCapacityExceeded();
  const std::uint64_t frame = g_manual_capture_frame_counter;
  for (const auto& [draw_key, draw] : trace->recorded_draws) {
    if (filter_active &&
        !eligible_shader_hashes.contains(draw.pipeline.shader_hash))
      continue;
    const ManualCaptureRecordKey key{draw_key.concrete.pso_incarnation,
        draw_key.concrete.geometry, draw_key.concrete.pass_fingerprint};
    const ManualCapturePipelineInfo pipeline{draw.pipeline.device,
        draw.pipeline.application_pipeline, draw.pipeline.incarnation_id,
        draw.pipeline.pso_fingerprint, draw.pipeline.context_hash,
        draw.pipeline.shader_hash, draw.pipeline.primitive_topology,
        draw.pipeline.rt0_blend, draw.pipeline.alpha_to_coverage,
        draw.pipeline.depth_test, draw.pipeline.depth_write,
        draw.pipeline.render_target_count, draw.pipeline.sample_count};
    const ManualCaptureBindingObservation binding{draw_key.root_constants,
        draw_key.pushed_cbvs, draw_key.descriptor_tables,
        draw_key.observed_bindings};
    g_manual_capture.Accumulate(key, pipeline, draw.commands, frame, binding);
  }
}

void OnManualCapturePresent(command_queue*, swapchain* presented_swapchain,
    const rect*, const rect*, std::uint32_t, const rect*) {
  if (!g_manual_capture_active.load(std::memory_order_acquire)) return;

  std::lock_guard lock(g_trace_mutex);
  if (g_manual_capture.state() != ManualCaptureState::Capturing) return;
  const std::uintptr_t swapchain_id =
      reinterpret_cast<std::uintptr_t>(presented_swapchain);
  if (g_manual_capture_swapchain == 0)
    g_manual_capture_swapchain = swapchain_id;
  else if (g_manual_capture_swapchain != swapchain_id)
    return;
  ++g_manual_capture_frame_counter;
  g_manual_capture.ObservePresent(g_manual_capture_frame_counter);
}

void RegisterManualCaptureEvents() {
  reshade::register_event<reshade::addon_event::execute_command_list>(
      OnManualCaptureExecute);
  reshade::register_event<reshade::addon_event::present>(
      OnManualCapturePresent);
}

void StartManualCapture() {
  // Read on the ImGui render thread, same as the checkbox that sets it;
  // Start() below copies the value into the session, which is what actually
  // governs filtering from this point on.
  const auto filter_mode = g_manual_capture_filter_pending_verified_only
      ? ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly
      : ManualCaptureShaderFilter::AllObservedPixelShaders;

  const bool fade_control_enabled = FadeControlCapturePending();

  std::lock_guard lock(g_trace_mutex);
  ++g_manual_capture_generation;
  g_manual_capture_frame_counter = 0;
  g_manual_capture_swapchain = 0;
  g_manual_capture.Start(g_manual_capture_generation, 0, filter_mode);
  g_manual_capture_active.store(true, std::memory_order_release);
  g_manual_capture_status = "capturing";
  // Independent lifecycle/mutex from the route accumulation above (see
  // fade_control_runtime.hpp); tied to the same session id purely for
  // human correlation across the two exported TSVs.
  StartFadeControlCapture(g_manual_capture_generation, fade_control_enabled);
}

bool StopAndExportManualCapture() {
  ManualCaptureSnapshot snapshot;
  {
    std::lock_guard lock(g_trace_mutex);
    if (g_manual_capture.state() != ManualCaptureState::Capturing)
      return false;
    snapshot = g_manual_capture.Stop(g_manual_capture_frame_counter);
    g_manual_capture_active.store(false, std::memory_order_release);
  }
  const bool fade_control_was_enabled = StopFadeControlCapture();

  // File I/O deliberately happens after the locks above are released.
  const std::string timestamp = LocalExportTimestamp();
  std::filesystem::path export_path;
  const bool exported =
      WriteManualCaptureExport(snapshot, timestamp, export_path);
  bool fade_control_exported = false;
  bool fade_snapshot_exported = false;
  if (fade_control_was_enabled) {
    std::filesystem::path fade_control_path;
    fade_control_exported =
        WriteFadeControlExport(timestamp, fade_control_path);
    std::filesystem::path fade_snapshot_path;
    fade_snapshot_exported =
        WriteFadeControlSnapshotExport(timestamp, fade_snapshot_path);
  }
  g_manual_capture_status = exported
      ? (fade_control_was_enabled
                ? ((fade_control_exported && fade_snapshot_exported)
                          ? "exported manual capture + fade control values "
                            "+ snapshots"
                          : "exported manual capture; fade control/"
                            "snapshot export failed: check DumpPath")
                : "exported manual capture")
      : "export failed: check DumpPath";
  if (exported) g_manual_capture_last_export = export_path.filename().string();
  return exported;
}

void ClearManualCapture() {
  std::lock_guard lock(g_trace_mutex);
  if (g_manual_capture.state() == ManualCaptureState::Capturing) return;
  g_manual_capture.Clear();
  g_manual_capture_status = "cleared";
  g_manual_capture_last_export.clear();
}

void DrawManualCaptureOverlay() {
  if (!ImGui::CollapsingHeader("Manual execution capture"))
    return;

  ImGui::TextDisabled(
      "Independent of the differential trace above: Start, reproduce one "
      "state, Stop + export, then repeat for a second state without "
      "restarting the game.");

  ManualCaptureSummary summary;
  {
    std::lock_guard lock(g_trace_mutex);
    summary = g_manual_capture.Summary();
  }
  const bool is_capturing = summary.state == ManualCaptureState::Capturing;

  ImGui::BeginDisabled(is_capturing);
  ImGui::Checkbox("Verified Fade Primitive only",
      &g_manual_capture_filter_pending_verified_only);
  ImGui::EndDisabled();
  ImGui::TextDisabled(
      "Snapshotted when Start capture is pressed and fixed for that "
      "session. Admits a Draw only when its Pixel Shader Hash already has a "
      "successful inspection with a fully verified Fade Primitive v1 "
      "instance; the matcher is not re-run here.");

  bool fade_control_pending = FadeControlCapturePending();
  ImGui::BeginDisabled(is_capturing);
  if (ImGui::Checkbox("Capture Fade control values", &fade_control_pending))
    SetFadeControlCapturePending(fade_control_pending);
  ImGui::EndDisabled();
  ImGui::TextDisabled(
      "Snapshotted at Start capture. Only meaningful for verified Fade "
      "Primitive Draws -- CPU command-recording-time observation of mapped "
      "constant-buffer memory, never GPU completion evidence. See "
      "manual-fade-controls-*.tsv for the full disclosure.");

  ImGui::BeginDisabled(is_capturing);
  if (ImGui::Button("Start capture")) StartManualCapture();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!is_capturing);
  if (ImGui::Button("Stop + export")) StopAndExportManualCapture();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(is_capturing);
  if (ImGui::Button("Clear")) ClearManualCapture();
  ImGui::EndDisabled();

  const char* state_name = summary.state == ManualCaptureState::Idle
      ? "Idle"
      : (summary.state == ManualCaptureState::Capturing ? "Capturing"
                                                          : "Captured");
  ImGui::Text("State: %s", state_name);
  ImGui::Text("Filter: %s", ShaderFilterName(summary.shader_filter));
  ImGui::Text("Session: %llu",
      static_cast<unsigned long long>(summary.session_id));
  ImGui::Text("Presents: %llu",
      static_cast<unsigned long long>(summary.captured_presents));
  ImGui::Text("Records: %zu", summary.record_count);
  ImGui::Text(
      "Capacity exceeded: %s", summary.capacity_exceeded ? "yes" : "no");
  ImGui::Text("Last export: %s",
      g_manual_capture_last_export.empty() ? "none"
                                            : g_manual_capture_last_export.c_str());
  ImGui::TextDisabled("%s", g_manual_capture_status.c_str());
  ImGui::TextDisabled(
      "Pixel Shader Hash, Application PSO, PSO Incarnation, PSO Fingerprint "
      "and PSO Context are separate identities -- see the exported TSV's "
      "columns, never a single \"Hash\".");
  if (summary.capacity_exceeded) {
    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
        "Unique-record capacity reached; this capture is incomplete.");
  }

  const auto fade_control = GetFadeControlDiagnosticCounters();
  if (fade_control.enabled) {
    ImGui::Separator();
    ImGui::Text(
        "Fade control values: sources=%zu | resolved bindings=%zu | "
        "sampled=%llu | unavailable=%llu",
        fade_control.control_sources, fade_control.resolved_bindings,
        static_cast<unsigned long long>(fade_control.sampled_values),
        static_cast<unsigned long long>(fade_control.unavailable_values));
    if (fade_control.capacity_exceeded) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
          "Fade control value capacity reached; this trace is incomplete.");
    }
    ImGui::Text("Predicate CBV snapshots: %zu", fade_control.snapshot_count);
    if (fade_control.snapshot_capacity_exceeded) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
          "Snapshot capacity reached; some predicate bindings were not "
          "snapshotted.");
    }
  }
}

}  // namespace wuwa_tfr::dev

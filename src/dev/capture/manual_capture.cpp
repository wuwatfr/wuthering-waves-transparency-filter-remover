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

#include <imgui.h>

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
  out_path = directory / ("manual-capture-" + timestamp + ".tsv");

  std::ofstream report(out_path, std::ios::binary | std::ios::trunc);
  if (!report) return false;

  report << "format\twuwa_tfr_manual_execution_capture_v1\n";
  report << "capture_type\tmanual_user_delimited_session\n";
  report << "session_id\t" << snapshot.session_id << '\n';
  report << "start_frame\t" << snapshot.start_frame << '\n';
  report << "end_frame\t" << snapshot.end_frame << '\n';
  report << "captured_presents\t" << snapshot.captured_presents << '\n';
  report << "record_count\t" << snapshot.records.size() << '\n';
  report << "capacity_exceeded\t"
         << static_cast<int>(snapshot.capacity_exceeded) << '\n';
  report << "export_timestamp_local\t" << timestamp << '\n';
  report << "membership_boundary\t"
      "command_list_submitted_while_capturing_not_recording_time\n";
  report << "swapchain_policy\t"
      "first_present_observed_after_start_capture_all_other_swapchains_"
      "ignored\n";
  report << "pso_identity_semantics\t"
      "application_pso_is_the_application_pipeline_handle_pso_incarnation_"
      "disambiguates_handle_reuse_pso_fingerprint_is_observed_pipeline_"
      "creation_identity_pixel_shader_hash_is_the_original_dxil_pixel_"
      "shader\n";
  report << "binding_observation\t"
      "fingerprints_of_observed_binding_state_and_events_not_captured_"
      "constant_buffer_contents\n";
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
      "\troot_constant_fingerprint\tpushed_cbv_fingerprint"
      "\tdescriptor_table_fingerprint\tobserved_bindings"
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
           << '\t' << Hex64(key.root_constant_fingerprint) << '\t'
           << Hex64(key.pushed_cbv_fingerprint) << '\t'
           << Hex64(key.descriptor_table_fingerprint) << '\t'
           << static_cast<int>(key.observed_bindings) << '\t'
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

}  // namespace

void OnManualCaptureExecute(command_queue*, command_list* cmd_list) {
  if (!g_manual_capture_active.load(std::memory_order_acquire) || !cmd_list)
    return;
  const auto* trace = cmd_list->get_private_data<CommandListTrace>();
  if (!trace ||
      (trace->recorded_draws.empty() &&
          !trace->recorded_draw_capacity_exceeded))
    return;

  std::lock_guard lock(g_trace_mutex);
  if (g_manual_capture.state() != ManualCaptureState::Capturing) return;
  if (trace->recorded_draw_capacity_exceeded)
    g_manual_capture.MarkCapacityExceeded();
  const std::uint64_t frame = g_manual_capture_frame_counter;
  for (const auto& [draw_key, draw] : trace->recorded_draws) {
    const ManualCaptureRecordKey key{
        draw_key.concrete.pso_incarnation, draw_key.concrete.geometry,
        draw_key.concrete.pass_fingerprint, draw_key.root_constants,
        draw_key.pushed_cbvs, draw_key.descriptor_tables,
        draw_key.observed_bindings};
    const ManualCapturePipelineInfo pipeline{draw.pipeline.device,
        draw.pipeline.application_pipeline, draw.pipeline.incarnation_id,
        draw.pipeline.pso_fingerprint, draw.pipeline.context_hash,
        draw.pipeline.shader_hash, draw.pipeline.primitive_topology,
        draw.pipeline.rt0_blend, draw.pipeline.alpha_to_coverage,
        draw.pipeline.depth_test, draw.pipeline.depth_write,
        draw.pipeline.render_target_count, draw.pipeline.sample_count};
    g_manual_capture.Accumulate(key, pipeline, draw.commands, frame);
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
  std::lock_guard lock(g_trace_mutex);
  ++g_manual_capture_generation;
  g_manual_capture_frame_counter = 0;
  g_manual_capture_swapchain = 0;
  g_manual_capture.Start(g_manual_capture_generation, 0);
  g_manual_capture_active.store(true, std::memory_order_release);
  g_manual_capture_status = "capturing";
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

  // File I/O deliberately happens after the lock above is released.
  const std::string timestamp = LocalExportTimestamp();
  std::filesystem::path export_path;
  const bool exported =
      WriteManualCaptureExport(snapshot, timestamp, export_path);
  g_manual_capture_status = exported ? "exported manual capture"
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
}

}  // namespace wuwa_tfr::dev

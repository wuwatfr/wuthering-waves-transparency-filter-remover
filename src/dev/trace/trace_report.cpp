// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/trace/trace_report.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <Windows.h>

#include "dev/capture/manual_capture_state.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"
#include "dev/trace/trace_events.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

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

  const std::string timestamp = LocalExportTimestamp();
  const auto filenames = AllocateExportFilenameGroup(
      {"runtime-trace-" + timestamp, "concrete-submission-trace-" + timestamp,
          "lifecycle-ambiguities-" + timestamp},
      ".tsv", [&directory](const std::string& candidate) {
        return std::filesystem::exists(directory / candidate);
      });

  std::ofstream report(
      directory / filenames[0],
      std::ios::binary | std::ios::trunc);
  if (!report) return false;
  report << "format\twuwa_tfr_runtime_trace_v5\n";
  report << "export_timestamp_local\t" << timestamp << "\n";
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
      directory / filenames[1],
      std::ios::binary | std::ios::trunc);
  if (!concrete_report) return false;
  concrete_report << "format\twuwa_tfr_concrete_submission_trace_v2\n";
  concrete_report << "export_timestamp_local\t" << timestamp << "\n";
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
        << static_cast<int>(row.pipeline_live) << '\t'
        << row.key.geometry.observations;
    for (const auto& window : row.windows)
      concrete_report << '\t' << window.queue_submitted_commands << '\t'
          << window.command_list_submissions << '\t' << window.active_frames;
    concrete_report << '\n';
  }
  concrete_report.flush();
  if (!concrete_report) return false;

  std::ofstream ambiguity_report(
      directory / filenames[2],
      std::ios::binary | std::ios::trunc);
  if (!ambiguity_report) return false;
  ambiguity_report << "format\twuwa_tfr_lifecycle_ambiguities_v2\n";
  ambiguity_report << "export_timestamp_local\t" << timestamp << "\n";
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

}  // namespace wuwa_tfr::dev

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_overlay.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <imgui.h>

#include "dev/dev_inspection.hpp"
#include "dev/dev_runtime.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"
#include "pre_fade_fmin_analysis.hpp"
#include "dev/trace/trace_events.hpp"
#include "dev/trace/trace_report.hpp"
#include "dev/trace/trace_state.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

void DrawFadePrimitiveTargetModes() {
  if (!ImGui::CollapsingHeader(
          "Dev transparency-filter target modes", ImGuiTreeNodeFlags_DefaultOpen))
    return;

  bool enabled = g_dev_antifade_runtime.enabled();
  if (ImGui::Checkbox("Remove Transparency Filter", &enabled))
    g_dev_antifade_runtime.set_enabled(enabled);
  ImGui::TextDisabled(
      "Replacement is owned entirely by the shared FadePrimitiveRuntime: every "
      "fully verified Fade Primitive v1 shader is matched and prepared; there "
      "is no per-hash selection.");

  const auto telemetry = g_dev_antifade_runtime.memory_telemetry_snapshot();
  ImGui::Text(
      "Shader cache: entries=%llu bytes=%llu | preparations in flight=%llu | replacement PSOs live=%llu | active devices=%llu",
      static_cast<unsigned long long>(telemetry.shader_cache_entries),
      static_cast<unsigned long long>(telemetry.shader_cache_bytecode_bytes),
      static_cast<unsigned long long>(telemetry.preparations_in_flight),
      static_cast<unsigned long long>(telemetry.live_replacement_pipelines),
      static_cast<unsigned long long>(telemetry.active_devices));
  ImGui::Text(
      "Matched=%llu | prepared=%llu | replacements created=%llu failed=%llu | replacement binds=%llu",
      static_cast<unsigned long long>(telemetry.matched_shaders_total),
      static_cast<unsigned long long>(telemetry.prepared_shaders_total),
      static_cast<unsigned long long>(telemetry.replacements_created_total),
      static_cast<unsigned long long>(telemetry.replacements_failed_total),
      static_cast<unsigned long long>(telemetry.replacement_binds_total));

  ImGui::Separator();

  struct DisplayRow {
    std::uint64_t shader_hash = 0;
    std::size_t live_application_psos = 0;
    std::uint32_t instances = 0;
    std::string consumers;
    std::uint32_t patch_evidence = 0;
    std::string adjacency;
    std::string fail_reasons;
    bool patch_succeeded = false;
    std::string patch_failure;
    bool prepared_succeeded = false;
    std::string prepared_failure;
  };
  std::unordered_map<std::uint64_t, std::size_t> live_pso_counts;
  {
    std::lock_guard lock(g_trace_mutex);
    for (const auto& [key, pipeline] : g_trace_pipelines)
      ++live_pso_counts[pipeline.shader_hash];
  }

  std::vector<DisplayRow> rows;
  {
    std::lock_guard inspection_lock(g_inspection_mutex);
    rows.reserve(g_inspections.size());
    for (const auto& [shader_hash, inspection] : g_inspections) {
      if (!inspection.inspection_succeeded || inspection.fade_instances.empty())
        continue;
      DisplayRow row;
      row.shader_hash = shader_hash;
      if (const auto live = live_pso_counts.find(shader_hash);
          live != live_pso_counts.end())
        row.live_application_psos = live->second;
      row.instances = static_cast<std::uint32_t>(
          inspection.fade_instances.size());
      row.patch_succeeded = inspection.patch_succeeded;
      row.patch_failure = inspection.patch_failure;
      row.prepared_succeeded = inspection.prepared_succeeded;
      row.prepared_failure = inspection.prepared_failure;

      std::vector<wuwa_tfr::FadePrimitiveInstance> instances_only;
      instances_only.reserve(inspection.fade_instances.size());
      for (const auto& fade_instance : inspection.fade_instances)
        instances_only.push_back(fade_instance.instance);
      row.consumers = FadePrimitiveConsumers(instances_only);

      for (const auto& fade_instance : inspection.fade_instances) {
        if (!fade_instance.pre_fade) {
          if (!row.fail_reasons.empty()) row.fail_reasons += "; ";
          row.fail_reasons += "no patch evidence (not reached before "
              "fail-closed rejection)";
          continue;
        }
        ++row.patch_evidence;
        const auto& analysis = *fade_instance.pre_fade;
        const char* name = "unknown";
        switch (analysis.adjacency) {
          case wuwa_tfr::PreFadeAdjacency::SameRow: name = "same-row"; break;
          case wuwa_tfr::PreFadeAdjacency::CrossRow: name = "cross-row"; break;
          case wuwa_tfr::PreFadeAdjacency::NonAdjacent: name = "non-adjacent"; break;
          case wuwa_tfr::PreFadeAdjacency::Unknown: name = "unknown"; break;
        }
        if (!row.adjacency.empty()) row.adjacency += ",";
        row.adjacency += name;
      }
      rows.push_back(std::move(row));
    }
  }
  std::sort(rows.begin(), rows.end(), [](const DisplayRow& left,
                                         const DisplayRow& right) {
    return left.shader_hash < right.shader_hash;
  });

  ImGui::Text("Fully verified Fade Primitive v1 shaders observed: %zu",
      rows.size());

  if (!ImGui::BeginTable("fade_primitive_diagnostics", 9,
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
          ImVec2(0.0f, 420.0f)))
    return;
  ImGui::TableSetupColumn("Pixel shader hash");
  ImGui::TableSetupColumn("Live application PSOs");
  ImGui::TableSetupColumn("Instances");
  ImGui::TableSetupColumn("Consumers");
  ImGui::TableSetupColumn("Patch evidence");
  ImGui::TableSetupColumn("Adjacency (diagnostic only)");
  ImGui::TableSetupColumn("Patch");
  ImGui::TableSetupColumn("Prepared");
  ImGui::TableSetupColumn("Fail-closed reason");
  ImGui::TableHeadersRow();
  for (const auto& row : rows) {
    const std::string hash = Hex64(row.shader_hash);
    ImGui::PushID(hash.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(hash.c_str());
    ImGui::TableSetColumnIndex(1);
    ImGui::Text("%zu", row.live_application_psos);
    ImGui::TableSetColumnIndex(2);
    ImGui::Text("%u", row.instances);
    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(row.consumers.c_str());
    ImGui::TableSetColumnIndex(4);
    ImGui::Text("%u/%u", row.patch_evidence, row.instances);
    ImGui::TableSetColumnIndex(5);
    ImGui::TextUnformatted(row.adjacency.c_str());
    ImGui::TableSetColumnIndex(6);
    if (row.patch_succeeded) ImGui::TextUnformatted("ok");
    else ImGui::TextUnformatted(row.patch_failure.c_str());
    ImGui::TableSetColumnIndex(7);
    if (row.prepared_succeeded) ImGui::TextUnformatted("ok");
    else ImGui::TextUnformatted(row.prepared_failure.c_str());
    ImGui::TableSetColumnIndex(8);
    ImGui::TextUnformatted(row.fail_reasons.c_str());
    ImGui::PopID();
  }
  ImGui::EndTable();
}

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

}  // namespace wuwa_tfr::dev

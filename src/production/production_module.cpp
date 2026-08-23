// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The Production variant implementation of addon_variant.hpp. Owns
// g_public_antifade_runtime (the sole Production fade-primitive replacement
// owner) and memory telemetry; addon.cpp knows nothing about either beyond
// calling these five functions.

#include "addon_variant.hpp"

#include <Windows.h>
#include <Psapi.h>
#include <imgui.h>
#include <reshade.hpp>

#include <chrono>
#include <string>
#include <string_view>

#include "addon_shared.hpp"
#include "fade_primitive_runtime.hpp"
#include "memory_telemetry.hpp"

using namespace reshade::api;

extern "C" __declspec(dllexport) const char* NAME =
    "WuwaTFR";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Automatic verified camera-proximity transparency removal for Wuthering Waves DX12.";

namespace wuwa_tfr::variant {

namespace {

wuwa_tfr::FadePrimitiveRuntime g_public_antifade_runtime;
wuwa_tfr::MemoryTelemetryController g_memory_telemetry;

struct ProcessMemoryTelemetryMetrics {
  std::uint64_t working_set_bytes = 0;
  std::uint64_t private_commit_bytes = 0;
  std::uint64_t handle_count = 0;
  bool memory_query_succeeded = false;
  bool handle_query_succeeded = false;
};

ProcessMemoryTelemetryMetrics QueryCurrentProcessMemoryTelemetry() noexcept {
  ProcessMemoryTelemetryMetrics metrics;
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(),
          reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
          sizeof(counters))) {
    metrics.working_set_bytes =
        static_cast<std::uint64_t>(counters.WorkingSetSize);
    metrics.private_commit_bytes =
        static_cast<std::uint64_t>(counters.PrivateUsage);
    metrics.memory_query_succeeded = true;
  }
  DWORD handle_count = 0;
  if (GetProcessHandleCount(GetCurrentProcess(), &handle_count)) {
    metrics.handle_count = handle_count;
    metrics.handle_query_succeeded = true;
  }
  return metrics;
}

void LogMemoryTelemetry(reshade::log::level level, std::string_view message) {
  std::string line = "[WuwaTFR][memory] ";
  line.append(message);
  reshade::log::message(level, line.c_str());
}

void OnInitPublicDevice(device* owner) {
  if (g_target_process) g_public_antifade_runtime.OnInitDevice(owner);
}

void OnDestroyPublicDevice(device* owner) {
  if (g_target_process) g_public_antifade_runtime.OnDestroyDevice(owner);
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

void EmitPublicMemoryTelemetryPresent() {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
  const auto ticket = g_memory_telemetry.TryAcquireSample(now);
  if (!ticket || !g_memory_telemetry.IsCurrent(*ticket)) return;

  const auto process = QueryCurrentProcessMemoryTelemetry();
  const auto runtime = g_public_antifade_runtime.memory_telemetry_snapshot();
  if (!g_memory_telemetry.IsCurrent(*ticket)) return;

  wuwa_tfr::MemoryTelemetrySnapshot snapshot;
  snapshot.working_set_bytes = process.working_set_bytes;
  snapshot.private_commit_bytes = process.private_commit_bytes;
  snapshot.handle_count = process.handle_count;
  snapshot.shader_cache_entries = runtime.shader_cache_entries;
  snapshot.shader_cache_bytecode_bytes = runtime.shader_cache_bytecode_bytes;
  snapshot.preparations_in_flight = runtime.preparations_in_flight;
  snapshot.live_replacement_pipelines = runtime.live_replacement_pipelines;
  snapshot.active_devices = runtime.active_devices;
  snapshot.matched_shaders_total = runtime.matched_shaders_total;
  snapshot.prepared_shaders_total = runtime.prepared_shaders_total;
  snapshot.replacements_created_total = runtime.replacements_created_total;
  snapshot.replacements_failed_total = runtime.replacements_failed_total;
  snapshot.replacement_binds_total = runtime.replacement_binds_total;

  const bool process_query_failed = !process.memory_query_succeeded ||
      !process.handle_query_succeeded;
  const std::string start_line = ticket->schema_start
      ? wuwa_tfr::FormatMemoryTelemetryStart(*ticket) : std::string{};
  const std::string sample_line =
      wuwa_tfr::FormatMemoryTelemetrySample(*ticket, snapshot);
  g_memory_telemetry.EmitIfCurrent(*ticket, [&] {
    if (!start_line.empty())
      LogMemoryTelemetry(reshade::log::level::info, start_line);
    // At most one warning every ten minutes (and at session start) if a
    // Windows process query is unavailable. The sample still reports all
    // available WuwaTFR retention and activity counters.
    if (process_query_failed &&
        (ticket->sample == 0 || ticket->sample %
            wuwa_tfr::kMemoryTelemetryWarningIntervalSamples == 0)) {
      LogMemoryTelemetry(reshade::log::level::warning,
          "process_query_failed=1 session=" + std::to_string(ticket->session) +
          " sample=" + std::to_string(ticket->sample) +
          " unavailable_process_fields_are_zero=1");
    }
    LogMemoryTelemetry(reshade::log::level::info, sample_line);
  });
}

void OnPublicMemoryTelemetryPresent(
    command_queue*, swapchain*, const rect*, const rect*, std::uint32_t,
    const rect*) noexcept {
  // Keep the disabled production present path to this atomic check only.
  if (!g_memory_telemetry.enabled()) return;
  (void)wuwa_tfr::InvokeMemoryTelemetryNoThrow(
      [] { EmitPublicMemoryTelemetryPresent(); });
}

}  // namespace

void InitializeVariant() {
  g_public_antifade_runtime.set_dxc_runtime_directory(g_addon_directory);
  g_public_antifade_runtime.set_enabled(wuwa_tfr::ConfigFlag(L"EnableTFR", true));
}

void RegisterVariantEvents() {
  reshade::register_event<reshade::addon_event::init_device>(
      OnInitPublicDevice);
  reshade::register_event<reshade::addon_event::destroy_device>(
      OnDestroyPublicDevice);
  reshade::register_event<reshade::addon_event::init_pipeline>(
      OnInitPublicPipeline);
  reshade::register_event<reshade::addon_event::destroy_pipeline>(
      OnDestroyPublicPipeline);
  reshade::register_event<reshade::addon_event::bind_pipeline>(
      OnBindPublicPipeline);
  reshade::register_event<reshade::addon_event::present>(
      OnPublicMemoryTelemetryPresent);
}

void DrawVariantOverlay() {
  ImGui::TextUnformatted("Automatic verified transparency-filter matching");
  bool antifade_enabled = g_public_antifade_runtime.enabled();
  if (ImGui::Checkbox("Remove Transparency Filter", &antifade_enabled)) {
    g_public_antifade_runtime.set_enabled(antifade_enabled);
    wuwa_tfr::SaveConfigFlag(L"EnableTFR", antifade_enabled);
  }
  bool memory_telemetry_enabled = g_memory_telemetry.enabled();
  if (ImGui::Checkbox("Log memory telemetry (10 s)", &memory_telemetry_enabled))
    g_memory_telemetry.SetEnabled(memory_telemetry_enabled);
  ImGui::TextDisabled(
      "Session-only; writes one sample every 10 seconds to ReShade.log.");
}

void LogVariantStartup() {
  Log(reshade::log::level::info,
      std::string("loaded automatic transparency-removal runtime") +
      "; devtools=not-compiled" +
      "; config=" + ConfigPath().string());
}

void OnLastDeviceDestroyed() {}

}  // namespace wuwa_tfr::variant

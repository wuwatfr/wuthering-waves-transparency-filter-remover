// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <imgui.h>
#include <reshade.hpp>

#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"
#include "fade_primitive_runtime.hpp"
#include "memory_telemetry.hpp"
#include "wuwa_process.hpp"
#if WUWA_TFR_DEVTOOLS
#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"
#include "target_dither_bypass.hpp"
#include "trace_submission_identity.hpp"

#include "dev/diagnostics/dev_diagnostics.hpp"
#include "dev/dev_runtime.hpp"
#include "dev/trace/trace_events.hpp"
#include "dev/trace/trace_report.hpp"
#include "dev/trace/trace_state.hpp"
#include "dev/dev_events.hpp"
#include "dev/dev_overlay.hpp"
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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifndef WUWA_TFR_DEVTOOLS
#define WUWA_TFR_DEVTOOLS 0
#endif

// Bridge for the small set of always-compiled shader-inspection/DXC/device-
// activity infrastructure shared between this file (compiled into both
// WuwaTFR and WuwaTFRDev) and the Dev-only modules under src/dev/. See the
// header for why these specific entities need external, not anonymous-
// namespace, linkage.
#include "production/addon_shared.hpp"

using namespace reshade::api;

namespace {

using wuwa_tfr::DeviceIdentity;
using wuwa_tfr::DeviceKey;
using wuwa_tfr::InspectionRecord;
using wuwa_tfr::g_inspection_mutex;
using wuwa_tfr::g_dxc;
using wuwa_tfr::g_inspections;
using wuwa_tfr::g_device_activity;
using wuwa_tfr::g_target_process;
using wuwa_tfr::g_addon_directory;
using wuwa_tfr::Hex64;
using wuwa_tfr::Log;
using wuwa_tfr::DumpDir;
using wuwa_tfr::InspectPixelShader;
using wuwa_tfr::LooksLikeDxil;
using wuwa_tfr::Fnv1a64;

// DeviceKey, g_inspection_mutex, g_dxc, g_inspections, and g_device_activity
// are declared (with external linkage) in production/addon_shared.hpp and
// defined further below, in the out-of-line `namespace wuwa_tfr { ... }`
// block near the end of this file, so that the Dev-only translation units
// under src/dev/ can share these exact instances.

std::atomic<std::uint32_t> g_d3d12_device_count{0};
std::atomic<std::uint64_t> g_seen_shader_callbacks{0};
std::atomic<std::uint64_t> g_unique_dxil_shaders{0};
std::atomic<std::uint64_t> g_disassembly_successes{0};
std::atomic<std::uint64_t> g_disassembly_failures{0};
std::atomic<std::uint64_t> g_dumped_shaders{0};
// g_discard_shader_count, g_strict_spatial_dither_count,
// g_ambiguous_spatial_dither_count, g_fade_primitive_shader_count, and
// g_fade_primitive_instance_count moved to
// dev/diagnostics/dev_diagnostics.hpp (wuwa_tfr::dev namespace); their only
// writer (InspectPixelShader, below) and reader (DrawTraceOverlay, moved to
// dev/dev_overlay.cpp) are both compiled only under WUWA_TFR_DEVTOOLS.

// g_target_process and g_addon_directory are declared in
// production/addon_shared.hpp and defined further below so Dev-only modules
// can share the same instances.
bool g_diagnostic = false;
bool g_dump = false;
std::filesystem::path g_dump_path;
#if !WUWA_TFR_DEVTOOLS
wuwa_tfr::FadePrimitiveRuntime g_public_antifade_runtime;
wuwa_tfr::MemoryTelemetryController g_memory_telemetry;
#endif

// Fnv1a64 is declared in production/addon_shared.hpp and defined further
// below so the Dev-only diagnostics module can share it.

// Hex64 is declared in production/addon_shared.hpp and defined further below
// so the Dev-only modules can share it.

#if !WUWA_TFR_DEVTOOLS
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
  std::array<wchar_t, 32768> executable_path{};
  const DWORD length = GetModuleFileNameW(
      nullptr, executable_path.data(),
      static_cast<DWORD>(executable_path.size()));
  if (length == 0 || length >= executable_path.size()) return false;
  return wuwa_tfr::IsWuwaExecutable(
      std::filesystem::path(executable_path.data(),
                            executable_path.data() + length));
}

// Log and DumpDir are declared in production/addon_shared.hpp and defined
// further below so the Dev-only modules can share them; DumpDir still reads
// this file's own g_dump_path.

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

// LooksLikeDxil and InspectPixelShader are declared in
// production/addon_shared.hpp and defined further below (after the counters
// and WriteCapture they use) so the Dev-only modules can share them.

#if WUWA_TFR_DEVTOOLS
// Only registered as an event in the Dev build (see DllMain); Production
// never wires this up, so it must not be compiled into that binary.
bool OnCreatePipeline(
    device* owner,
    pipeline_layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects) {
  if (wuwa_tfr::dev::g_dev_runtime_internal_pipeline_event) return false;
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
#endif

void OnInitDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;
  if (g_device_activity.Activate(DeviceKey(owner)))
    g_d3d12_device_count.fetch_add(1, std::memory_order_relaxed);
}

void OnDestroyDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;

  auto teardown = g_device_activity.Deactivate(DeviceKey(owner));
  if (!teardown) return;

#if WUWA_TFR_DEVTOOLS
  wuwa_tfr::dev::OnDestroyDeviceHook(owner);
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
#endif

void DrawOverlay(effect_runtime*) {
  ImGui::TextUnformatted("WuwaTFR");
  ImGui::TextDisabled("Build: %s", WUWA_TFR_BUILD_COMMIT);
#if WUWA_TFR_DEVTOOLS
  ImGui::TextUnformatted("Dev: FadePrimitiveRuntime + diagnostics/trace tools");
  ImGui::TextDisabled(
      "Dev runs its own instance of the same replacement runtime as Production, "
      "plus read-only diagnostics and trace/capture tools.");
#else
  ImGui::TextUnformatted("Automatic verified transparency-filter matching");
  bool antifade_enabled = g_public_antifade_runtime.enabled();
  if (ImGui::Checkbox("Remove Transparency Filter", &antifade_enabled)) {
    g_public_antifade_runtime.set_enabled(antifade_enabled);
    SaveConfigFlag(L"EnableTFR", antifade_enabled);
  }
  bool memory_telemetry_enabled = g_memory_telemetry.enabled();
  if (ImGui::Checkbox("Log memory telemetry (10 s)", &memory_telemetry_enabled))
    g_memory_telemetry.SetEnabled(memory_telemetry_enabled);
  ImGui::TextDisabled(
      "Session-only; writes one sample every 10 seconds to ReShade.log.");
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

  ImGui::Separator();
  wuwa_tfr::dev::DrawFadePrimitiveTargetModes();
  ImGui::Separator();
  wuwa_tfr::dev::DrawTraceOverlay();
#endif
}

} // namespace

// Out-of-line definitions for the shared, externally-linked bridge declared
// in production/addon_shared.hpp (see the comments left at each entity's
// original position above). This block is placed after the anonymous
// namespace closes so it can still reach that namespace's members (which,
// per the language rules for unnamed namespaces, are visible via the
// enclosing scope) -- HasDxilChunk, WriteCapture, g_dump_path, and the
// always-compiled shader-inspection counters, all of which stay internal to
// this file since only InspectPixelShader itself needs external linkage.
namespace wuwa_tfr {

std::mutex g_inspection_mutex;
DxcBridge* g_dxc = nullptr;
std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;
DeviceActivityState<DeviceIdentity> g_device_activity;
bool g_target_process = false;
std::filesystem::path g_addon_directory;

std::string Hex64(std::uint64_t value) {
  std::ostringstream stream;
  stream << std::hex << std::uppercase << std::setw(16)
         << std::setfill('0') << value;
  return stream.str();
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

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool LooksLikeDxil(const reshade::api::shader_desc& descriptor) {
  if (!descriptor.code || descriptor.code_size < 64) return false;
  return HasDxilChunk(
      static_cast<const std::uint8_t*>(descriptor.code),
      descriptor.code_size);
}

void InspectPixelShader(const reshade::api::shader_desc& descriptor) {
  g_seen_shader_callbacks.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t hash = Fnv1a64(descriptor.code, descriptor.code_size);

  std::lock_guard lock(g_inspection_mutex);
  if (g_inspections.contains(hash)) return;

  g_unique_dxil_shaders.fetch_add(1, std::memory_order_relaxed);
  InspectionRecord record;
  record.bytecode_size = descriptor.code_size;

  if (!g_dxc) g_dxc = new DxcBridge(g_addon_directory);
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
    record.dither = AnalyzeSpatialDitherDiagnostic(inspection.original_ir);
    record.fade_primitive = AnalyzeFadePrimitiveV1(inspection.original_ir);
    if (record.dither.discard_calls != 0)
      dev::g_discard_shader_count.fetch_add(1, std::memory_order_relaxed);
    if (record.dither.classification ==
        SpatialDitherClassification::StrictSpatialDither) {
      dev::g_strict_spatial_dither_count.fetch_add(1,
          std::memory_order_relaxed);
    } else if (record.dither.classification ==
        SpatialDitherClassification::AmbiguousStrictSpatialDither) {
      dev::g_ambiguous_spatial_dither_count.fetch_add(
          1, std::memory_order_relaxed);
    }
    if (!record.fade_primitive.instances.empty()) {
      dev::g_fade_primitive_shader_count.fetch_add(1,
          std::memory_order_relaxed);
      dev::g_fade_primitive_instance_count.fetch_add(
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

}  // namespace wuwa_tfr

#if WUWA_TFR_DEVTOOLS
extern "C" __declspec(dllexport) const char* NAME =
    "WuwaTFR Dev (diagnostics build)";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Developer diagnostics build: runs the same replacement runtime as "
    "Production plus read-only trace/capture tools.";
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
      reshade::register_event<reshade::addon_event::present>(
          OnPublicMemoryTelemetryPresent);
#endif
#if WUWA_TFR_DEVTOOLS
      wuwa_tfr::dev::RegisterDevEvents();
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

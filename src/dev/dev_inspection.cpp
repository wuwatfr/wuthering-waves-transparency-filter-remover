// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_inspection.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <string>
#include <utility>

#include "dev/dev_runtime.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"
#include "addon_shared.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

namespace {

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

bool WriteCapture(
    std::uint64_t hash,
    std::size_t bytecode_size,
    const std::string& original_ir,
    const wuwa_tfr::SpatialDitherDiagnostic& dither,
    const wuwa_tfr::FadePrimitiveDiagnostic& fade_primitive) {
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
  metadata << "analysis=independent_spatial_dither_diagnostic_v1\n";
  metadata << "mutation=none\n";
  metadata << "source_hash=" << base << "\n";
  metadata << "bytecode_size=" << bytecode_size << "\n";
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
  metadata << "ir_file=" << base << ".original.ll\n";
  return static_cast<bool>(metadata);
}

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

}  // namespace

std::mutex g_inspection_mutex;
DxcBridge* g_dxc = nullptr;
std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;

std::atomic<std::uint64_t> g_seen_shader_callbacks{0};
std::atomic<std::uint64_t> g_unique_dxil_shaders{0};
std::atomic<std::uint64_t> g_disassembly_successes{0};
std::atomic<std::uint64_t> g_disassembly_failures{0};
std::atomic<std::uint64_t> g_dumped_shaders{0};

bool g_diagnostic = false;
bool g_dump = false;

bool LooksLikeDxil(const reshade::api::shader_desc& descriptor) {
  if (!descriptor.code || descriptor.code_size < 64) return false;
  return HasDxilChunk(
      static_cast<const std::uint8_t*>(descriptor.code),
      descriptor.code_size);
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
    record.dither = AnalyzeSpatialDitherDiagnostic(inspection.original_ir);
    record.fade_primitive = AnalyzeFadePrimitiveV1(inspection.original_ir);
    // Diagnostic-only; never influences record.fade_primitive or patch
    // eligibility above.
    record.fade_control = AnalyzeFadeControlSources(
        inspection.original_ir, record.fade_primitive);
    if (record.dither.discard_calls != 0)
      g_discard_shader_count.fetch_add(1, std::memory_order_relaxed);
    if (record.dither.classification ==
        SpatialDitherClassification::StrictSpatialDither) {
      g_strict_spatial_dither_count.fetch_add(1, std::memory_order_relaxed);
    } else if (record.dither.classification ==
        SpatialDitherClassification::AmbiguousStrictSpatialDither) {
      g_ambiguous_spatial_dither_count.fetch_add(
          1, std::memory_order_relaxed);
    }
    if (!record.fade_primitive.instances.empty()) {
      g_fade_primitive_shader_count.fetch_add(1, std::memory_order_relaxed);
      g_fade_primitive_instance_count.fetch_add(
          record.fade_primitive.instances.size(), std::memory_order_relaxed);
    }
    record.dumped = WriteCapture(
        hash, descriptor.code_size, inspection.original_ir,
        record.dither, record.fade_primitive);
    if (record.dumped)
      g_dumped_shaders.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_disassembly_failures.fetch_add(1, std::memory_order_relaxed);
  }
  g_inspections.emplace(hash, std::move(record));
}

bool OnCreatePipeline(
    device* owner,
    pipeline_layout,
    std::uint32_t subobject_count,
    const pipeline_subobject* subobjects) {
  if (g_dev_runtime_internal_pipeline_event) return false;
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

void InitializeInspectionConfig() {
  g_diagnostic = ConfigFlag(L"Diagnostic", EnvFlag(L"WUWA_TFR_DIAGNOSTIC"));
  g_dump = ConfigFlag(L"Dump", EnvFlag(L"WUWA_TFR_DUMP"));
  g_dump_path = ConfigPathValue(L"DumpPath");
}

void TeardownInspectionOnLastDevice() {
  std::lock_guard lock(g_inspection_mutex);
  delete g_dxc;
  g_dxc = nullptr;
}

}  // namespace wuwa_tfr::dev

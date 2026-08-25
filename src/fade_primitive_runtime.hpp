// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstdint>
#include <filesystem>

namespace wuwa_tfr {

class FadePrimitiveRuntimeObserver;

struct FadePrimitiveRuntimeTelemetrySnapshot {
  std::uint64_t shader_cache_entries = 0;
  std::uint64_t shader_cache_bytecode_bytes = 0;
  std::uint64_t preparations_in_flight = 0;
  std::uint64_t live_replacement_pipelines = 0;
  std::uint64_t active_devices = 0;
  std::uint64_t matched_shaders_total = 0;
  std::uint64_t prepared_shaders_total = 0;
  std::uint64_t replacements_created_total = 0;
  std::uint64_t replacements_failed_total = 0;
  std::uint64_t replacement_binds_total = 0;
};

class FadePrimitiveRuntime {
 public:
  FadePrimitiveRuntime();
  ~FadePrimitiveRuntime();
  FadePrimitiveRuntime(const FadePrimitiveRuntime&) = delete;
  FadePrimitiveRuntime& operator=(const FadePrimitiveRuntime&) = delete;

  void OnInitDevice(reshade::api::device* device);
  void OnDestroyDevice(reshade::api::device* device);
  void OnInitPipeline(reshade::api::device* device,
      reshade::api::pipeline_layout layout, std::uint32_t subobject_count,
      const reshade::api::pipeline_subobject* subobjects,
      reshade::api::pipeline pipeline);
  void OnDestroyPipeline(reshade::api::device* device,
      reshade::api::pipeline pipeline);
  void OnBindPipeline(reshade::api::command_list* command_list,
      reshade::api::pipeline_stage stages, reshade::api::pipeline pipeline);
  void set_dxc_runtime_directory(std::filesystem::path addon_directory);
  void set_observer(FadePrimitiveRuntimeObserver* observer);
  bool enabled() const;
  void set_enabled(bool enabled);
  FadePrimitiveRuntimeTelemetrySnapshot memory_telemetry_snapshot() const;

 private:
  struct Impl;
  Impl* impl_;
};

}
#endif

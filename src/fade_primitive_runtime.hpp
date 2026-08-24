// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

#include <cstdint>
#include <filesystem>

namespace wuwa_tfr {

class FadePrimitiveRuntimeObserver;

// A point-in-time view of only the Production runtime objects explicitly
// retained by WuwaTFR. Cumulative totals retain their existing meanings:
// matched/prepared count successful structural match and patch preparation;
// replacements_created counts successful replacement PSO creation;
// replacements_failed counts fail-closed preparation/replacement outcomes;
// replacement_binds counts actual replacement bind calls.
struct FadePrimitiveRuntimeTelemetrySnapshot {
  // Completed cache entries include cached fail-closed outcomes. Payload bytes
  // include only completed entries that retain patched bytecode vectors.
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

// Production runtime for the fully verified fade primitive. It intentionally
// owns no capture, trace, or manual-target state.
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
  // Set during add-on initialization, before ReShade registers callbacks.
  void set_dxc_runtime_directory(std::filesystem::path addon_directory);
  // Installs an optional, read-only observer of already-computed shader-
  // preparation and application-pipeline facts; see
  // fade_primitive_runtime_observer.hpp. Not owned: the caller retains
  // responsibility for the observer's lifetime, which must outlive every
  // subsequent callback into this runtime. Like set_dxc_runtime_directory,
  // this is initialization-time configuration -- set once, before any
  // device activates (i.e. before OnInitDevice can first be called) -- and
  // is never guarded by a lock, so it must not be changed afterward.
  // Production never calls this setter; its observer stays null and its
  // behavior is unaffected.
  void set_observer(FadePrimitiveRuntimeObserver* observer);
  bool enabled() const;
  void set_enabled(bool enabled);
  FadePrimitiveRuntimeTelemetrySnapshot memory_telemetry_snapshot() const;

 private:
  struct Impl;
  Impl* impl_;
};

} // namespace wuwa_tfr
#endif

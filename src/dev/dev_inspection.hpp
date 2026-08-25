// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <reshade.hpp>

#include "dev/capture/fade_control_state.hpp"
#include "dxil_dither_diagnostic.hpp"
#include "fade_primitive_detector.hpp"
#include "fade_primitive_runtime_observer.hpp"
#include "pre_fade_fmin_analysis.hpp"

namespace wuwa_tfr::dev {

struct FadeInstanceObservation {
  wuwa_tfr::FadePrimitiveInstance instance;
  std::optional<wuwa_tfr::PreFadeFMinAnalysis> pre_fade;
};

struct InspectionRecord {
  bool inspection_succeeded = false;
  std::string inspection_error;
  std::size_t bytecode_size = 0;
  std::vector<FadeInstanceObservation> fade_instances;
  std::shared_ptr<const std::vector<FadeControlSamplingSource>>
      fade_control_sampling_sources;
  bool patch_succeeded = false;
  std::string patch_failure;
  bool prepared_succeeded = false;
  std::string prepared_failure;
  wuwa_tfr::SpatialDitherDiagnostic dither;
  bool dumped = false;
};

extern std::mutex g_inspection_mutex;
extern std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;

extern std::atomic<std::uint64_t> g_seen_shader_callbacks;
extern std::atomic<std::uint64_t> g_unique_dxil_shaders;
extern std::atomic<std::uint64_t> g_disassembly_successes;
extern std::atomic<std::uint64_t> g_disassembly_failures;
extern std::atomic<std::uint64_t> g_dumped_shaders;

extern bool g_dump;

wuwa_tfr::FadePrimitiveRuntimeObserver* InspectionObserver();

void InitializeInspectionConfig();

}

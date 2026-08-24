// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "dev/capture/fade_control_state.hpp"
#include "dev/trace/trace_state.hpp"

namespace wuwa_tfr::dev {

void RegisterFadeControlRuntimeEvents();

bool FadeControlCapturePending();
void SetFadeControlCapturePending(bool enabled);

void StartFadeControlCapture(std::uint64_t session_id, bool enabled);

bool StopFadeControlCapture();

struct FadeControlDiagnosticCounters {
  bool enabled = false;
  std::size_t control_sources = 0;
  std::size_t resolved_bindings = 0;
  std::uint64_t sampled_values = 0;
  std::uint64_t unavailable_values = 0;
  bool capacity_exceeded = false;
  std::size_t snapshot_count = 0;
  bool snapshot_capacity_exceeded = false;
};
FadeControlDiagnosticCounters GetFadeControlDiagnosticCounters();

bool WriteFadeControlExport(
    const std::string& timestamp, std::filesystem::path& out_path);

bool WriteFadeControlSnapshotExport(
    const std::string& timestamp, std::filesystem::path& out_path);

void SampleFadeControlValuesOnDraw(const CommandListTrace& trace,
    const wuwa_tfr::TraceConcreteDrawKey& route,
    const wuwa_tfr::ExecutionPipelineIdentity& pipeline);

}

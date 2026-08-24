// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/manual_capture_state.hpp"

namespace wuwa_tfr::dev {

ManualCaptureSessionToken g_manual_capture_session_token;

void ManualCaptureAccumulator::Start(std::uint64_t session_id,
    std::uint64_t start_frame, ManualCaptureShaderFilter shader_filter) {
  active_ = ManualCaptureSnapshot{};
  active_.session_id = session_id;
  active_.start_frame = start_frame;
  active_.end_frame = start_frame;
  active_.shader_filter = shader_filter;
  index_.clear();
  last_result_ = ManualCaptureSnapshot{};
  state_ = ManualCaptureState::Capturing;
}

namespace {

void UpdateBindingSummary(ManualCaptureBindingSummary& summary,
    bool already_observed, std::uint64_t fingerprint) {
  if (!already_observed) {
    summary.first_fingerprint = fingerprint;
    summary.last_fingerprint = fingerprint;
    summary.changed = false;
    return;
  }
  if (summary.last_fingerprint != fingerprint) summary.changed = true;
  summary.last_fingerprint = fingerprint;
}

}

void ManualCaptureAccumulator::Accumulate(const ManualCaptureRecordKey& key,
    const wuwa_tfr::ExecutionPipelineIdentity& pipeline, std::uint64_t commands,
    std::uint64_t frame, const ManualCaptureBindingObservation& binding) {
  if (state_ != ManualCaptureState::Capturing) return;

  std::size_t position;
  if (const auto existing = index_.find(key); existing != index_.end()) {
    position = existing->second;
  } else {
    if (active_.records.size() >= kMaxManualCaptureRecords) {
      active_.capacity_exceeded = true;
      return;
    }
    position = active_.records.size();
    active_.records.push_back({key, ManualCaptureRecord{}});
    index_.emplace(key, position);
    active_.records[position].second.pipeline = pipeline;
    active_.records[position].second.first_frame = frame;
  }

  auto& record = active_.records[position].second;
  record.commands += commands;
  ++record.submissions;
  record.last_frame = frame;

  if ((binding.observed_bindings & 0x1) != 0) {
    UpdateBindingSummary(record.root_constants,
        (record.observed_bindings & 0x1) != 0,
        binding.root_constant_fingerprint);
  }
  if ((binding.observed_bindings & 0x2) != 0) {
    UpdateBindingSummary(record.pushed_cbvs,
        (record.observed_bindings & 0x2) != 0, binding.pushed_cbv_fingerprint);
  }
  if ((binding.observed_bindings & 0x4) != 0) {
    UpdateBindingSummary(record.descriptor_tables,
        (record.observed_bindings & 0x4) != 0,
        binding.descriptor_table_fingerprint);
  }
  record.observed_bindings |= binding.observed_bindings;
}

void ManualCaptureAccumulator::MarkCapacityExceeded() {
  if (state_ != ManualCaptureState::Capturing) return;
  active_.capacity_exceeded = true;
}

void ManualCaptureAccumulator::ObservePresent(std::uint64_t frame) {
  if (state_ != ManualCaptureState::Capturing) return;
  active_.end_frame = frame;
  ++active_.captured_presents;
}

ManualCaptureSnapshot ManualCaptureAccumulator::Stop(std::uint64_t end_frame) {
  if (state_ != ManualCaptureState::Capturing) return {};
  active_.end_frame = end_frame;
  last_result_ = active_;
  state_ = ManualCaptureState::Captured;
  active_ = ManualCaptureSnapshot{};
  index_.clear();
  return last_result_;
}

void ManualCaptureAccumulator::Clear() {
  if (state_ == ManualCaptureState::Capturing) return;
  last_result_ = ManualCaptureSnapshot{};
  state_ = ManualCaptureState::Idle;
}

ManualCaptureSummary ManualCaptureAccumulator::Summary() const noexcept {
  const ManualCaptureSnapshot& current =
      state_ == ManualCaptureState::Capturing ? active_ : last_result_;
  return ManualCaptureSummary{
      state_,
      current.session_id,
      current.start_frame,
      current.end_frame,
      current.captured_presents,
      current.records.size(),
      current.capacity_exceeded,
      current.shader_filter,
  };
}

std::string AllocateExportFilename(const std::string& stem,
    const std::string& extension,
    const std::function<bool(const std::string&)>& exists) {
  std::string candidate = stem + extension;
  if (!exists(candidate)) return candidate;
  for (int suffix = 1; suffix <= kMaxExportFilenameAttempts; ++suffix) {
    candidate = stem + "-" + std::to_string(suffix) + extension;
    if (!exists(candidate)) return candidate;
  }
  return candidate;
}

}

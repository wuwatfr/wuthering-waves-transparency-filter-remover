// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

enum class ManualCaptureState : std::uint8_t {
  Idle,
  Capturing,
  Captured,
};

class ManualCaptureSessionToken {
 public:
  std::uint64_t value() const noexcept {
    return value_.load(std::memory_order_acquire);
  }
  void Start(std::uint64_t session_id) noexcept {
    value_.store(session_id, std::memory_order_release);
  }
  void Stop() noexcept { value_.store(0, std::memory_order_release); }

  bool StillLive(std::uint64_t session_id) const noexcept {
    return session_id != 0 && value() == session_id;
  }

 private:
  std::atomic<std::uint64_t> value_{0};
};

extern ManualCaptureSessionToken g_manual_capture_session_token;

constexpr std::size_t kMaxManualCaptureRecords = 16384;

using ManualCaptureRecordKey = wuwa_tfr::TraceConcreteDrawKey;
using ManualCaptureRecordKeyHash = wuwa_tfr::TraceConcreteDrawKeyHash;

enum class ManualCaptureShaderFilter : std::uint8_t {
  AllObservedPixelShaders,
  VerifiedFadePrimitiveOnly,
};

struct ManualCaptureBindingSummary {
  std::uint64_t first_fingerprint = 0;
  std::uint64_t last_fingerprint = 0;
  bool changed = false;
};

struct ManualCaptureBindingObservation {
  std::uint64_t root_constant_fingerprint = 0;
  std::uint64_t pushed_cbv_fingerprint = 0;
  std::uint64_t descriptor_table_fingerprint = 0;
  std::uint8_t observed_bindings = 0;
};

struct ManualCaptureRecord {
  wuwa_tfr::ExecutionPipelineIdentity pipeline;
  std::uint64_t commands = 0;
  std::uint64_t submissions = 0;
  std::uint64_t first_frame = 0;
  std::uint64_t last_frame = 0;
  std::uint8_t observed_bindings = 0;
  ManualCaptureBindingSummary root_constants;
  ManualCaptureBindingSummary pushed_cbvs;
  ManualCaptureBindingSummary descriptor_tables;
};

struct ManualCaptureSnapshot {
  std::uint64_t session_id = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint64_t captured_presents = 0;
  bool capacity_exceeded = false;
  ManualCaptureShaderFilter shader_filter =
      ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly;
  std::vector<std::pair<ManualCaptureRecordKey, ManualCaptureRecord>> records;
};

struct ManualCaptureSummary {
  ManualCaptureState state = ManualCaptureState::Idle;
  std::uint64_t session_id = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint64_t captured_presents = 0;
  std::size_t record_count = 0;
  bool capacity_exceeded = false;
  ManualCaptureShaderFilter shader_filter =
      ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly;
};

class ManualCaptureAccumulator {
 public:
  void Start(std::uint64_t session_id, std::uint64_t start_frame,
      ManualCaptureShaderFilter shader_filter);

  void Accumulate(const ManualCaptureRecordKey& key,
      const wuwa_tfr::ExecutionPipelineIdentity& pipeline, std::uint64_t commands,
      std::uint64_t frame, const ManualCaptureBindingObservation& binding);

  void MarkCapacityExceeded();
  void ObservePresent(std::uint64_t frame);
  ManualCaptureSnapshot Stop(std::uint64_t end_frame);
  void Clear();

  ManualCaptureState state() const noexcept { return state_; }

  bool IsLiveSession(std::uint64_t session_id) const noexcept {
    return state_ == ManualCaptureState::Capturing &&
        active_.session_id == session_id;
  }

  ManualCaptureSummary Summary() const noexcept;

  const ManualCaptureSnapshot& active() const noexcept { return active_; }

  const ManualCaptureSnapshot& last_result() const noexcept {
    return last_result_;
  }

 private:
  ManualCaptureState state_ = ManualCaptureState::Idle;
  ManualCaptureSnapshot active_;
  ManualCaptureSnapshot last_result_;
  std::unordered_map<ManualCaptureRecordKey, std::size_t,
      ManualCaptureRecordKeyHash>
      index_;
};

constexpr int kMaxExportFilenameAttempts = 1000;

std::string AllocateExportFilename(const std::string& stem,
    const std::string& extension,
    const std::function<bool(const std::string&)>& exists);

}

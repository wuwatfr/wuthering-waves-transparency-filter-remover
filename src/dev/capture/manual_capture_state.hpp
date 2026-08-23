// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only manual execution capture: pure session lifecycle and per-record
// accumulation state, independent of ReShade/D3D12 so it can be unit tested
// without the runtime. See dev/capture/manual_capture.* for the ReShade
// event handlers, TSV export, and ImGui panel that drive this state from
// the existing runtime differential trace's own observation
// (CommandListTrace::recorded_draws, dev/trace/trace_state.hpp).
//
// Record identity intentionally mirrors dev/trace/trace_state.hpp's
// RecordedTraceDrawKey (PSO incarnation + exact geometry/pass + observed
// binding-state fingerprints) so this never aggregates by shader hash alone:
// one shader observed under several distinct application PSOs, passes, or
// binding routes stays as several distinct records.

#pragma once

#include <cstddef>
#include <cstdint>
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

// Same order of magnitude as dev/trace/trace_state.hpp's
// kMaxConcreteTraceRecords; a manual session is a single bounded
// investigation window, not the full differential trace.
constexpr std::size_t kMaxManualCaptureRecords = 16384;

struct ManualCaptureRecordKey {
  std::uint64_t pso_incarnation = 0;
  wuwa_tfr::TraceGeometryKey geometry;
  std::uint64_t pass_fingerprint = 0;
  std::uint64_t root_constant_fingerprint = 0;
  std::uint64_t pushed_cbv_fingerprint = 0;
  std::uint64_t descriptor_table_fingerprint = 0;
  std::uint8_t observed_bindings = 0;

  friend bool operator==(
      const ManualCaptureRecordKey&, const ManualCaptureRecordKey&) = default;
};

struct ManualCaptureRecordKeyHash {
  std::size_t operator()(const ManualCaptureRecordKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.pso_incarnation);
    wuwa_tfr::TraceHashCombine(
        hash, wuwa_tfr::TraceGeometryKeyHash{}(key.geometry));
    wuwa_tfr::TraceHashCombine(hash, key.pass_fingerprint);
    wuwa_tfr::TraceHashCombine(hash, key.root_constant_fingerprint);
    wuwa_tfr::TraceHashCombine(hash, key.pushed_cbv_fingerprint);
    wuwa_tfr::TraceHashCombine(hash, key.descriptor_table_fingerprint);
    wuwa_tfr::TraceHashCombine(hash, key.observed_bindings);
    return hash;
  }
};

// Pipeline/shader identity for one accumulated record, copied verbatim from
// the existing trace's TracePipelineInfo (dev/trace/trace_state.hpp) at
// accumulation time. Declared separately here only so this header stays
// ReShade-independent; the field *values* are never recomputed -- see
// dev/capture/manual_capture.cpp's OnManualCaptureExecute.
struct ManualCapturePipelineInfo {
  std::uint64_t device = 0;
  std::uint64_t application_pso = 0;
  std::uint64_t pso_incarnation = 0;
  std::uint64_t pso_fingerprint = 0;
  std::uint64_t pso_context_hash = 0;
  std::uint64_t pixel_shader_hash = 0;
  std::uint32_t primitive_topology = 0;
  bool rt0_blend = false;
  bool alpha_to_coverage = false;
  bool depth_test = false;
  bool depth_write = false;
  std::uint32_t render_target_count = 0;
  std::uint32_t sample_count = 1;
};

struct ManualCaptureRecord {
  ManualCapturePipelineInfo pipeline;
  std::uint64_t commands = 0;
  std::uint64_t submissions = 0;
  std::uint64_t first_frame = 0;
  std::uint64_t last_frame = 0;
};

struct ManualCaptureSnapshot {
  std::uint64_t session_id = 0;
  std::uint64_t start_frame = 0;
  std::uint64_t end_frame = 0;
  std::uint64_t captured_presents = 0;
  bool capacity_exceeded = false;
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
};

// Not internally synchronized: the caller (dev/capture/manual_capture.cpp)
// serializes all access under the existing trace's g_trace_mutex, the same
// pattern the rest of dev/trace/trace_state.hpp's globals use. This keeps
// start/stop/accumulate from ever racing into a partially attributed
// session.
class ManualCaptureAccumulator {
 public:
  // Begins a new session: clears the previous manual-capture result (if
  // any), resets accumulation, and starts accepting records. Never touches
  // any other Dev investigation state.
  void Start(std::uint64_t session_id, std::uint64_t start_frame);

  // Accumulates one submitted key/pipeline pair. No-op outside the
  // Capturing state. `frame` is the session-relative frame this submission
  // belongs to (see ObservePresent).
  void Accumulate(const ManualCaptureRecordKey& key,
      const ManualCapturePipelineInfo& pipeline, std::uint64_t commands,
      std::uint64_t frame);

  // Marks the active session incomplete without discarding what was already
  // captured, e.g. when a source command list hit its own per-list
  // recording cap upstream.
  void MarkCapacityExceeded();

  // Records one observed Present on the session's selected swapchain.
  void ObservePresent(std::uint64_t frame);

  // Freezes the active session as the new manual-capture result and returns
  // a copy of it. No-op outside the Capturing state (returns a default,
  // empty snapshot).
  ManualCaptureSnapshot Stop(std::uint64_t end_frame);

  // Clears only the frozen result. Never affects an in-progress session or
  // any other Dev investigation state; call sites must not invoke this
  // while Capturing.
  void Clear();

  ManualCaptureState state() const noexcept { return state_; }

  // The session currently worth displaying: the in-progress session while
  // Capturing, otherwise the last frozen result (or an empty summary if
  // none exists yet). Cheap: never copies the record vector.
  ManualCaptureSummary Summary() const noexcept;

  // The in-progress session (only meaningful while Capturing).
  const ManualCaptureSnapshot& active() const noexcept { return active_; }

  // The most recently frozen session, if any.
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

}  // namespace wuwa_tfr::dev

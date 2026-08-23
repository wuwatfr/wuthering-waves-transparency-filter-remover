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
// Record identity is the same stable execution-route identity the existing
// concrete trace already uses -- wuwa_tfr::TraceConcreteDrawKey (PSO
// incarnation + exact geometry + pass fingerprint) -- so this never
// aggregates by shader hash alone: one shader observed under several
// distinct application PSOs, passes, or geometry stays as several distinct
// records.
//
// Binding-state fingerprints (root constants, pushed CBVs, descriptor
// tables) are deliberately NOT part of record identity: the first real
// in-game captures showed that including them there fragments a single
// stable Draw route into thousands of near-duplicate records, because the
// pushed-CBV and descriptor-table fingerprints in particular are
// *accumulated binding-event* fingerprints, not stable per-Draw state (see
// dev/trace/trace_state.hpp's CommandListTrace::pushed_cbv_fingerprint /
// descriptor_table_fingerprint). They remain valuable investigation clues,
// so each record instead keeps a compact first/last/changed summary per
// binding kind -- bounded storage, not an unbounded fingerprint set.

#pragma once

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

// Same order of magnitude as dev/trace/trace_state.hpp's
// kMaxConcreteTraceRecords; a manual session is a single bounded
// investigation window, not the full differential trace.
constexpr std::size_t kMaxManualCaptureRecords = 16384;

// Deliberately the same type the existing concrete trace uses for route
// identity (dev/trace/trace_state.hpp's g_concrete_trace is keyed by this
// too) -- reusing it directly instead of a look-alike local struct keeps the
// two identities from silently drifting apart.
using ManualCaptureRecordKey = wuwa_tfr::TraceConcreteDrawKey;
using ManualCaptureRecordKeyHash = wuwa_tfr::TraceConcreteDrawKeyHash;

// Which pixel shaders a manual-capture session accumulates. Snapshotted once
// at Start() and fixed for the session's lifetime -- see
// ManualCaptureAccumulator::Start.
enum class ManualCaptureShaderFilter : std::uint8_t {
  AllObservedPixelShaders,
  VerifiedFadePrimitiveOnly,
};

// A compact summary of one binding kind's fingerprints across every Draw
// aggregated into a record, instead of storing every distinct fingerprint
// (which would just move the unbounded-storage problem into each record).
// `changed` is sticky: once two observations disagree it stays true even if
// a later observation happens to match again.
struct ManualCaptureBindingSummary {
  std::uint64_t first_fingerprint = 0;
  std::uint64_t last_fingerprint = 0;
  bool changed = false;
};

// One Draw's observed binding-state fingerprints, exactly as computed by
// the existing trace's CommandListTrace (dev/trace/trace_state.hpp) at the
// moment of that Draw -- copied in, never recomputed here. `observed_bindings`
// bit 0x1 = root constants, 0x2 = pushed CBVs, 0x4 = descriptor tables,
// matching dev/trace/trace_events.cpp's own bit usage.
struct ManualCaptureBindingObservation {
  std::uint64_t root_constant_fingerprint = 0;
  std::uint64_t pushed_cbv_fingerprint = 0;
  std::uint64_t descriptor_table_fingerprint = 0;
  std::uint8_t observed_bindings = 0;
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
  // OR of every accumulated Draw's observed_bindings -- which binding kinds
  // were ever observed for this route, not any single Draw's state.
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

// Not internally synchronized: the caller (dev/capture/manual_capture.cpp)
// serializes all access under the existing trace's g_trace_mutex, the same
// pattern the rest of dev/trace/trace_state.hpp's globals use. This keeps
// start/stop/accumulate from ever racing into a partially attributed
// session.
class ManualCaptureAccumulator {
 public:
  // Begins a new session: clears the previous manual-capture result (if
  // any), resets accumulation, and starts accepting records. Never touches
  // any other Dev investigation state. `shader_filter` is snapshotted for
  // the whole session -- see ManualCaptureShaderFilter.
  void Start(std::uint64_t session_id, std::uint64_t start_frame,
      ManualCaptureShaderFilter shader_filter);

  // Accumulates one submitted key/pipeline pair, plus that Draw's observed
  // binding-state fingerprints (folded into the record's compact
  // first/last/changed summaries, never stored per-observation). No-op
  // outside the Capturing state. `frame` is the session-relative frame this
  // submission belongs to (see ObservePresent).
  void Accumulate(const ManualCaptureRecordKey& key,
      const ManualCapturePipelineInfo& pipeline, std::uint64_t commands,
      std::uint64_t frame, const ManualCaptureBindingObservation& binding);

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

// Upper bound on numbered-suffix attempts before AllocateExportFilename
// gives up and returns the last candidate anyway (a residual, deliberately
// unresolved collision at that point is astronomically unlikely for a Dev
// diagnostic export and preferable to spinning forever).
constexpr int kMaxExportFilenameAttempts = 1000;

// Returns the first of "<stem><extension>", "<stem>-1<extension>",
// "<stem>-2<extension>", ... for which `exists` reports false. Pure/
// portable: takes an injected existence check instead of touching the
// filesystem itself, so it is unit-testable without ReShade or real files.
// Exists to make manual-capture export filenames collision-free even when
// two sessions are stopped within the same LocalExportTimestamp() second
// (LocalExportTimestamp() has only second precision, and the exporter opens
// with std::ios::trunc).
std::string AllocateExportFilename(const std::string& stem,
    const std::string& extension,
    const std::function<bool(const std::string&)>& exists);

}  // namespace wuwa_tfr::dev

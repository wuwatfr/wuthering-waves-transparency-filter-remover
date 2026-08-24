// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, investigation-only: the ReShade-facing half of the targeted
// Fade control-value tracer. Samples runtime CBV values for the three roles
// dev/dev_inspection.cpp already resolved statically, once, at pipeline
// inspection time -- the verified instance's gate-predicate evidence
// (fade_primitive_detector.hpp) and, when the instance also reached a
// Matched pre-Fade FMin, that FMin's two direct-scalar-CBV operands
// (pre_fade_fmin_analysis.hpp). This module performs no DXIL analysis of
// its own; layered on dev/capture/fade_control_state.* (bounded aggregation)
// to answer one question -- does the same verified Fade Primitive receive
// distinguishable runtime control values under camera-proximity fade vs.
// scripted/animation invisibility? It does not implement, and must never be
// treated as evidence for, any Production runtime switching, conditional
// patching, or matcher/heuristic change.
//
// value_observation semantics (repeated verbatim in every export's
// metadata; read this before trusting any sampled value):
//   CPU command-recording-time observation of already-mapped constant-
//   buffer memory. NOT GPU completion evidence. NOT proof of the value the
//   GPU ultimately consumed, if the application violates normal upload-
//   buffer synchronization. A value is sampled once per recorded Draw, at
//   the moment that Draw is recorded (not when its command list is later
//   submitted) -- see dev/trace/trace_events.cpp's RecordOrSuppressTraceDraw
//   for the exact call site.
//
// Concurrency: this module owns one dedicated mutex, g_fade_control_mutex,
// deliberately independent of dev/trace/trace_state.hpp's g_trace_mutex and
// dev/dev_inspection.hpp's g_inspection_mutex. SampleFadeControlValuesOnDraw
// (the hot Draw-time path) takes g_inspection_mutex only after releasing
// any other lock, and never nests it inside g_fade_control_mutex or
// g_trace_mutex -- see that function's definition for the exact ordering.
//
// Session lifecycle: this module owns no independent "is a session active"
// state of its own. It participates in the one authoritative manual-capture
// session (dev/capture/manual_capture_state.hpp's ManualCaptureAccumulator,
// driven by dev/capture/manual_capture.cpp's StartManualCapture/
// StopAndExportManualCapture) via that same file's shared, lock-free
// g_manual_capture_session_token -- checked first on the Draw-time path,
// before this module's own g_fade_control_enabled_for_session (whether this
// particular session opted into tracing at all). StartFadeControlCapture/
// StopFadeControlCapture below do not decide when a session starts or
// stops; they are only ever called by manual_capture.cpp in lockstep with
// its own Start/Stop, and record this module's per-session participation
// (accumulator resets, the enabled choice), not session identity itself.
//
// Performance boundary: no GPU readback, no fence waits, no DXIL analysis
// or matcher rerun on Draw, no file I/O on Draw, no unbounded allocation on
// Draw. All shader/control-source analysis happens once, at pipeline
// inspection time (dev/dev_inspection.cpp). The Draw-time hook is gated by
// two relaxed-acquire atomic loads (the shared session token, then this
// module's own enabled flag) and is a no-op whenever no manual capture is
// sampling control values.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "dev/capture/fade_control_state.hpp"
#include "dev/trace/trace_state.hpp"

namespace wuwa_tfr::dev {

// Registers this module's own ReShade event handlers (init/destroy
// pipeline_layout for static register/space resolution, map/unmap_buffer_
// region and destroy_resource for mapped-buffer lifecycle). Independent of
// RegisterDevEvents()/RegisterManualCaptureEvents(); registers no Draw
// event of its own.
void RegisterFadeControlRuntimeEvents();

// UI-thread-only pending choice for the *next* manual-capture session,
// mirroring manual_capture.cpp's own pending-checkbox pattern. The caller
// (the "Capture Fade control values" checkbox) must only allow editing this
// while not capturing; the value actually governing an active session is
// the one snapshotted by StartFadeControlCapture below.
bool FadeControlCapturePending();
void SetFadeControlCapturePending(bool enabled);

// Called by manual_capture.cpp's StartManualCapture, immediately after it
// starts the one shared session -- never an independent entry point of its
// own. Resets this module's accumulators for the new session and records
// `enabled`, the already-snapshotted (session-fixed) choice of whether this
// particular session participates at all.
void StartFadeControlCapture(std::uint64_t session_id, bool enabled);

// Called by manual_capture.cpp's StopAndExportManualCapture, alongside its
// own stop of the shared session. Freezes this module's accumulated state.
// Returns false (and touches nothing further) if value tracing was never
// enabled for this session.
bool StopFadeControlCapture();

struct FadeControlDiagnosticCounters {
  bool enabled = false;
  std::size_t control_sources = 0;
  std::size_t resolved_bindings = 0;
  std::uint64_t sampled_values = 0;
  std::uint64_t unavailable_values = 0;
  bool capacity_exceeded = false;
  // Distinct predicate CBV byte-window snapshots captured this session (see
  // dev/capture/fade_control_snapshot.hpp) -- at most one per unique
  // resolved predicate binding, independent of sampled_values above.
  std::size_t snapshot_count = 0;
  bool snapshot_capacity_exceeded = false;
};
FadeControlDiagnosticCounters GetFadeControlDiagnosticCounters();

// Writes the bounded aggregate for the most recently stopped session to a
// separate, timestamped, collision-free TSV (manual-fade-controls-*.tsv),
// alongside (never in place of) the route TSV manual_capture.cpp already
// writes. `timestamp` should be the same LocalExportTimestamp() the caller
// used for that route TSV, so the two files are self-evidently paired.
// Returns false (out_path left untouched) if value tracing was never
// enabled for the session, or if the export itself fails.
bool WriteFadeControlExport(
    const std::string& timestamp, std::filesystem::path& out_path);

// Writes the session's captured predicate CBV byte-window snapshots (see
// dev/capture/fade_control_snapshot.hpp) to a separate, timestamped,
// collision-free TSV (manual-fade-snapshots-*.tsv), alongside (never in
// place of) the route and per-value-stats TSVs. Same enable/timestamp
// contract as WriteFadeControlExport: returns false (out_path untouched) if
// value tracing was never enabled for the session, or the export itself
// fails. A session with tracing enabled but zero verified predicates ever
// resolving to mapped memory still succeeds, producing a header-only file.
bool WriteFadeControlSnapshotExport(
    const std::string& timestamp, std::filesystem::path& out_path);

// The Draw-time sampling hook: called once per recorded Draw from
// dev/trace/trace_events.cpp's RecordOrSuppressTraceDraw, reusing that
// single source of Draw/PSO/pass provenance instead of a second
// instrumentation chain. `route` and `pipeline` are exactly what that
// existing Draw path already computed for its own recorded_draws entry.
void SampleFadeControlValuesOnDraw(const CommandListTrace& trace,
    const wuwa_tfr::TraceConcreteDrawKey& route,
    const TracePipelineInfo& pipeline);

}  // namespace wuwa_tfr::dev

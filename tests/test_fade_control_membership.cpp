// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include <cstring>
#include <vector>

#include "dev/capture/fade_control_snapshot.hpp"
#include "dev/capture/manual_capture_state.hpp"
#include "test_check.hpp"

// Command-list submission is the single authoritative session boundary for
// both Manual Capture and Fade-control. These tests model the interleaving
// between Draw-record time (where Fade values are sampled and staged as
// pending observations, unconditionally of session state) and
// execute_command_list time (where OnManualCaptureExecute decides session
// membership once and commits both channels together) deterministically,
// the same way test_manual_capture_state.cpp already models
// ManualCaptureSessionToken/IsLiveSession staleness -- no threads, no
// sleeps, no ReShade command-list objects required.

namespace {

using wuwa_tfr::ExecutionPipelineIdentity;
using wuwa_tfr::dev::CommitPendingFadeControlObservations;
using wuwa_tfr::dev::FadeControlAccumulator;
using wuwa_tfr::dev::FadeControlRecordKey;
using wuwa_tfr::dev::FadeControlRole;
using wuwa_tfr::dev::FadeControlSnapshotAccumulator;
using wuwa_tfr::dev::FadeControlTrackerCapacityAccumulator;
using wuwa_tfr::dev::FadeControlTrackerCapacityDiagnostics;
using wuwa_tfr::dev::FadeControlTrackerCapacityHasLoss;
using wuwa_tfr::dev::FadeControlValueSample;
using wuwa_tfr::dev::MergeFadeControlTrackerCapacity;
using wuwa_tfr::dev::ManualCaptureAccumulator;
using wuwa_tfr::dev::ManualCaptureBindingObservation;
using wuwa_tfr::dev::ManualCaptureRecordKey;
using wuwa_tfr::dev::ManualCaptureSessionToken;
using wuwa_tfr::dev::ManualCaptureShaderFilter;
using wuwa_tfr::dev::PendingFadeControlObservation;

constexpr auto kAllShaders = ManualCaptureShaderFilter::AllObservedPixelShaders;

wuwa_tfr::TraceGeometryKey Geometry(std::uint64_t vertex_resource) {
  wuwa_tfr::TraceGeometryKey key;
  key.kind = wuwa_tfr::TraceDrawKind::Indexed;
  key.arguments = {10, 1, 0, 0, 0};
  key.topology = 4;
  key.vertex_buffers.push_back({0, vertex_resource, 0, 32});
  key.index_buffer = wuwa_tfr::TraceIndexBinding{vertex_resource + 1, 0, 4};
  return key;
}

wuwa_tfr::TraceConcreteDrawKey Route(std::uint64_t pso_incarnation,
    std::uint64_t vertex_resource = 1, std::uint64_t pass_fingerprint = 100) {
  return wuwa_tfr::TraceConcreteDrawKey{
      pso_incarnation, Geometry(vertex_resource), pass_fingerprint};
}

FadeControlRecordKey MakeFadeKey(std::uint64_t pso_incarnation = 1) {
  FadeControlRecordKey key;
  key.route = Route(pso_incarnation);
  key.role = FadeControlRole::Predicate;
  return key;
}

ExecutionPipelineIdentity MakePipeline() {
  ExecutionPipelineIdentity pipeline;
  pipeline.device = 1;
  pipeline.shader_hash = 0x1;
  return pipeline;
}

FadeControlValueSample Available(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return FadeControlValueSample{true, bits, 0};
}

// One Draw's worth of state, exactly what RecordedTraceDraw carries between
// Draw-record time and execute_command_list time in the real runtime.
struct RecordedDrawSim {
  ManualCaptureRecordKey manual_key;
  ExecutionPipelineIdentity pipeline;
  std::vector<PendingFadeControlObservation> pending_observations;
  FadeControlTrackerCapacityDiagnostics pending_tracker_taint;
};

// Mirrors SampleFadeControlValuesOnDraw: stage the sampled evidence and, with
// it, a snapshot of the tracker taint as it stands right now. Later tracker
// failures cannot reach back into a Draw already recorded.
void SimulateRecordDraw(RecordedDrawSim& draw,
    const FadeControlTrackerCapacityDiagnostics& runtime_taint,
    const PendingFadeControlObservation& observation) {
  draw.pending_observations.push_back(observation);
  MergeFadeControlTrackerCapacity(draw.pending_tracker_taint, runtime_taint);
}

// Mirrors how SampleFadeControlValuesOnDraw composes the two independent
// taint sources at Draw-record time: Fade-control's own tracker taint, plus
// the canonical resource-lifecycle owner's prune taint read through
// ResourceLifecycleCapacityTaintSnapshot(). Fade-control never reads Trace's
// globals to learn the latter.
FadeControlTrackerCapacityDiagnostics ComposeRecordTimeTaint(
    const FadeControlTrackerCapacityDiagnostics& tracker_taint,
    bool lifecycle_evidence_dropped) {
  FadeControlTrackerCapacityDiagnostics taint = tracker_taint;
  taint.resource_lifecycle_loss |= lifecycle_evidence_dropped;
  return taint;
}

struct CommandListRecordingSim {
  std::vector<RecordedDrawSim> draws;
  std::uint64_t fade_admitted_manual_session = 0;

  void Reset() {
    draws.clear();
    fade_admitted_manual_session = 0;
  }
};

CommandListRecordingSim Recording(std::vector<RecordedDrawSim> draws) {
  CommandListRecordingSim recording;
  recording.draws = std::move(draws);
  return recording;
}

void SimulateExecuteCommandList(const ManualCaptureSessionToken& token,
    ManualCaptureAccumulator& manual, FadeControlAccumulator& fade,
    FadeControlSnapshotAccumulator& fade_snapshots,
    FadeControlTrackerCapacityAccumulator& fade_diagnostics,
    CommandListRecordingSim& recording) {
  const std::uint64_t session_id = token.value();
  if (session_id == 0 || !manual.IsLiveSession(session_id)) return;
  const bool admit_fade =
      recording.fade_admitted_manual_session != session_id;
  if (admit_fade) recording.fade_admitted_manual_session = session_id;
  for (const auto& draw : recording.draws) {
    manual.Accumulate(
        draw.manual_key, draw.pipeline, 1, 1, ManualCaptureBindingObservation{});
    if (admit_fade)
      CommitPendingFadeControlObservations(fade, fade_snapshots,
          fade_diagnostics, draw.pipeline, draw.pending_observations, {},
          draw.pending_tracker_taint);
  }
}

void SimulateExecuteCommandList(const ManualCaptureSessionToken& token,
    ManualCaptureAccumulator& manual, FadeControlAccumulator& fade,
    FadeControlSnapshotAccumulator& fade_snapshots,
    FadeControlTrackerCapacityAccumulator& fade_diagnostics,
    std::vector<RecordedDrawSim> draws) {
  CommandListRecordingSim recording = Recording(std::move(draws));
  SimulateExecuteCommandList(
      token, manual, fade, fade_snapshots, fade_diagnostics, recording);
}

}  // namespace

int main() {
  // 1. Recorded before the session starts, submitted while it's live: Fade
  // sampling is unconditional at Draw-record time (no session check left in
  // SampleFadeControlValuesOnDraw), so pending data exists regardless, and
  // submission-time membership admits both channels together.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    CHECK(token.value() == 0);  // no session yet at record time

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    const auto manual_result = manual.Stop(1);
    const auto fade_result = fade.Stop();
    CHECK(manual_result.records.size() == 1);
    CHECK(fade_result.records.size() == 1);
    CHECK(fade_result.records[0].second.stats.available_observations == 1);
  }

  // 2. Recorded while the session is live, submitted after Stop(): neither
  // channel is admitted, since execute_command_list re-checks liveness
  // against the CURRENT token, never the value captured at record time.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};

    // The session stops before this command list reaches
    // execute_command_list.
    token.Stop();
    manual.Stop(1);
    fade.Stop();
    fade_snapshots.Stop();
    fade_diagnostics.Stop();

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    CHECK(manual.last_result().records.empty());
    CHECK(fade.last_result().records.empty());
  }

  // 3. Recorded and submitted while the same session is live: both admitted.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    const auto manual_result = manual.Stop(1);
    const auto fade_result = fade.Stop();
    CHECK(manual_result.records.size() == 1);
    CHECK(fade_result.records.size() == 1);
  }

  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    auto recording = Recording({draw});

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);

    const auto manual_result = manual.Stop(1);
    const auto fade_result = fade.Stop();
    CHECK(manual_result.records.size() == 1);
    CHECK(manual_result.records[0].second.submissions == 2);
    CHECK(fade_result.records.size() == 1);
    CHECK(fade_result.records[0].second.stats.draw_observations == 1);
    CHECK(fade_result.records[0].second.stats.available_observations == 1);
  }

  // 5. CommandListTrace::Reset() clears pending Fade observations: pending
  // data lives as plain std::vector members of RecordedTraceDraw, itself a
  // value stored in CommandListTrace::recorded_draws, so
  // recorded_draws.clear() (already called by Reset()) destroys it
  // transitively -- no separate top-level container was introduced to
  // track pending Fade state, so there is nothing extra Reset() could forget
  // to clear. This is a structural (RAII) guarantee verified by code
  // reading and by the fact that trace_state.hpp compiles with no new
  // Reset()-adjacent state; it is not re-derivable as a standalone value
  // level test here since CommandListTrace requires the ReShade SDK headers
  // this test target intentionally does not depend on (see the other
  // ReShade-glue files in dev/trace and dev/capture, none of which carry
  // standalone unit tests in this project either).

  // 6. A secondary command list's pending Fade observations propagate into
  // the primary command list with the route rewritten to the primary's
  // pass-inherited concrete key, exactly like OnExecuteSecondaryTrace does
  // for the existing Draw record -- never left under the secondary list's
  // own (possibly pass-unobserved) route.
  {
    const auto secondary_route = Route(1, /*vertex_resource=*/1,
        /*pass_fingerprint=*/0);
    const auto inherited_route = Route(1, /*vertex_resource=*/1,
        /*pass_fingerprint=*/999);

    FadeControlRecordKey secondary_key;
    secondary_key.route = secondary_route;
    secondary_key.role = FadeControlRole::Predicate;

    std::vector<PendingFadeControlObservation> secondary_pending{
        {secondary_key, Available(2.0f)}};

    // The merge OnExecuteSecondaryTrace performs: append with the key's
    // route overwritten to the primary's inherited route.
    std::vector<PendingFadeControlObservation> primary_pending;
    for (auto pending : secondary_pending) {
      pending.key.route = inherited_route;
      primary_pending.push_back(pending);
    }

    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;
    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 999};
    draw.pipeline = MakePipeline();
    draw.pending_observations = primary_pending;

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});
    const auto fade_result = fade.Stop();
    CHECK(fade_result.records.size() == 1);
    CHECK(fade_result.records[0].first.route == inherited_route);
    CHECK(!(fade_result.records[0].first.route == secondary_route));
  }

  // 7. Tracker loss happened BEFORE the capture started, and the Draw that
  // sampled through that damaged tracker state was also recorded before
  // Start, then submitted live. Start must not erase runtime taint: the
  // completed capture has to report the loss, because the evidence it admits
  // was produced under it.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    FadeControlTrackerCapacityDiagnostics runtime_taint;
    runtime_taint.mapped_buffer_loss = true;

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    SimulateRecordDraw(draw, runtime_taint, {MakeFadeKey(1), Available(1.0f)});

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    manual.Stop(1);
    fade.Stop();
    const auto diagnostics = fade_diagnostics.Stop();
    CHECK(diagnostics.mapped_buffer_loss);
    CHECK(!diagnostics.descriptor_slot_loss);
  }

  // 8. The Draw was recorded while every tracker was clean; the tracker only
  // failed afterwards. Submitting that Draw must not blame it for a loss it
  // never sampled through.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    FadeControlTrackerCapacityDiagnostics runtime_taint;  // clean
    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    SimulateRecordDraw(draw, runtime_taint, {MakeFadeKey(1), Available(1.0f)});

    // The tracker fails only now, after this Draw was already recorded.
    runtime_taint.descriptor_slot_loss = true;

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    const auto fade_result = fade.Stop();
    const auto diagnostics = fade_diagnostics.Stop();
    CHECK(fade_result.records.size() == 1);
    CHECK(!diagnostics.descriptor_slot_loss);
    CHECK(!FadeControlTrackerCapacityHasLoss(diagnostics));
  }

  // 9. A stale submission -- the session already stopped -- contributes
  // neither evidence nor provenance, even though the Draw carries taint.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    FadeControlTrackerCapacityDiagnostics runtime_taint;
    runtime_taint.layout_map_loss = true;
    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    SimulateRecordDraw(draw, runtime_taint, {MakeFadeKey(1), Available(1.0f)});

    token.Stop();
    manual.Stop(1);
    fade.Stop();
    fade_snapshots.Stop();
    fade_diagnostics.Stop();

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    CHECK(fade.last_result().records.empty());
    CHECK(!fade_diagnostics.last_result().layout_map_loss);
  }

  // 10. Tracker failures after Stop must not mutate the frozen result the
  // export reads.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    FadeControlTrackerCapacityDiagnostics runtime_taint;
    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    SimulateRecordDraw(draw, runtime_taint, {MakeFadeKey(1), Available(1.0f)});

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {draw});

    token.Stop();
    manual.Stop(1);
    fade.Stop();
    const auto frozen = fade_diagnostics.Stop();
    CHECK(!FadeControlTrackerCapacityHasLoss(frozen));

    // Everything below happens after Stop and before the export reads
    // last_result(): a tracker failure, and a late submission carrying it.
    runtime_taint.descriptor_range_truncated = true;
    RecordedDrawSim late;
    late.manual_key = ManualCaptureRecordKey{2, Geometry(2), 100};
    late.pipeline = MakePipeline();
    SimulateRecordDraw(late, runtime_taint, {MakeFadeKey(2), Available(2.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {late});

    CHECK(!fade_diagnostics.last_result().descriptor_range_truncated);
    CHECK(fade_diagnostics.last_result() == frozen);
  }

  // 11. A new capture starts with empty capture diagnostics, while the
  // runtime taint that survives Start is still available to evidence recorded
  // afterwards -- the two are separate pieces of state.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    // A first capture that reported loss.
    FadeControlTrackerCapacityDiagnostics runtime_taint;
    runtime_taint.descriptor_slot_loss = true;
    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();
    RecordedDrawSim first;
    first.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    first.pipeline = MakePipeline();
    SimulateRecordDraw(first, runtime_taint, {MakeFadeKey(1), Available(1.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {first});
    token.Stop();
    manual.Stop(1);
    fade.Stop();
    CHECK(fade_diagnostics.Stop().descriptor_slot_loss);

    // The second capture starts clean...
    token.Start(2);
    manual.Start(2, 0, kAllShaders);
    fade.Start(2);
    fade_snapshots.Start(2);
    fade_diagnostics.Start();
    CHECK(!FadeControlTrackerCapacityHasLoss(
        fade_diagnostics.active_diagnostics()));

    // ...but the runtime taint was never cleared by Start, so evidence
    // recorded now still carries it and the new capture reports it too.
    RecordedDrawSim second;
    second.manual_key = ManualCaptureRecordKey{2, Geometry(2), 100};
    second.pipeline = MakePipeline();
    SimulateRecordDraw(second, runtime_taint, {MakeFadeKey(2), Available(2.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {second});
    CHECK(fade_diagnostics.Stop().descriptor_slot_loss);
  }

  // 12. Canonical resource-lifecycle capacity loss travels the same
  // provenance path as Fade-control's own tracker taint. A Draw recorded
  // while the lifecycle owner was intact is not retroactively tainted by a
  // later prune; a Draw recorded after the prune is.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    const FadeControlTrackerCapacityDiagnostics clean_trackers;
    bool lifecycle_dropped = false;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim before;
    before.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    before.pipeline = MakePipeline();
    SimulateRecordDraw(before,
        ComposeRecordTimeTaint(clean_trackers, lifecycle_dropped),
        {MakeFadeKey(1), Available(1.0f)});
    CHECK(!before.pending_tracker_taint.resource_lifecycle_loss);

    // PruneResourceLifecycleTo drops incarnation records only now.
    lifecycle_dropped = true;

    RecordedDrawSim after;
    after.manual_key = ManualCaptureRecordKey{2, Geometry(2), 100};
    after.pipeline = MakePipeline();
    SimulateRecordDraw(after,
        ComposeRecordTimeTaint(clean_trackers, lifecycle_dropped),
        {MakeFadeKey(2), Available(2.0f)});
    CHECK(after.pending_tracker_taint.resource_lifecycle_loss);
    // The earlier Draw's staged provenance is untouched by the later prune.
    CHECK(!before.pending_tracker_taint.resource_lifecycle_loss);

    // Submitting only the clean Draw leaves the capture clean...
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {before});
    CHECK(!fade_diagnostics.active_diagnostics().resource_lifecycle_loss);

    // ...and submitting the tainted one propagates it, without setting any
    // of Fade-control's own tracker flags.
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {after});
    const auto active = fade_diagnostics.active_diagnostics();
    CHECK(active.resource_lifecycle_loss);
    CHECK(!active.descriptor_slot_loss && !active.mapped_buffer_loss);
    CHECK(!active.layout_map_loss && !active.descriptor_range_truncated);
    CHECK(FadeControlTrackerCapacityHasLoss(active));

    manual.Stop(1);
    fade.Stop();
    CHECK(fade_diagnostics.Stop().resource_lifecycle_loss);
  }

  // 13. A non-live submission contributes no lifecycle taint, and Stop
  // freezes the flag against later lifecycle loss -- while a subsequent
  // capture starts clean yet can still inherit the runtime taint, which the
  // lifecycle owner never clears.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    const FadeControlTrackerCapacityDiagnostics clean_trackers;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim clean;
    clean.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    clean.pipeline = MakePipeline();
    SimulateRecordDraw(clean, ComposeRecordTimeTaint(clean_trackers, false),
        {MakeFadeKey(1), Available(1.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {clean});

    token.Stop();
    manual.Stop(1);
    fade.Stop();
    const auto frozen = fade_diagnostics.Stop();
    CHECK(!frozen.resource_lifecycle_loss);

    // The lifecycle owner prunes after Stop, and a stale command list
    // carrying that taint is submitted late: neither may reach the frozen
    // capture.
    RecordedDrawSim stale;
    stale.manual_key = ManualCaptureRecordKey{2, Geometry(2), 100};
    stale.pipeline = MakePipeline();
    SimulateRecordDraw(stale, ComposeRecordTimeTaint(clean_trackers, true),
        {MakeFadeKey(2), Available(2.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {stale});
    CHECK(!fade_diagnostics.last_result().resource_lifecycle_loss);
    CHECK(fade_diagnostics.last_result() == frozen);

    // A new capture starts clean, but the runtime lifecycle taint survived,
    // so a Draw recorded now still inherits it.
    token.Start(2);
    manual.Start(2, 0, kAllShaders);
    fade.Start(2);
    fade_snapshots.Start(2);
    fade_diagnostics.Start();
    CHECK(!fade_diagnostics.active_diagnostics().resource_lifecycle_loss);

    RecordedDrawSim fresh;
    fresh.manual_key = ManualCaptureRecordKey{3, Geometry(3), 100};
    fresh.pipeline = MakePipeline();
    SimulateRecordDraw(fresh, ComposeRecordTimeTaint(clean_trackers, true),
        {MakeFadeKey(3), Available(3.0f)});
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, {fresh});
    CHECK(fade_diagnostics.Stop().resource_lifecycle_loss);
  }

  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    auto recording = Recording({draw});

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    token.Stop();
    manual.Stop(1);
    const auto first_session = fade.Stop();
    fade_snapshots.Stop();
    fade_diagnostics.Stop();
    CHECK(first_session.records[0].second.stats.draw_observations == 1);

    token.Start(2);
    manual.Start(2, 0, kAllShaders);
    fade.Start(2);
    fade_snapshots.Start(2);
    fade_diagnostics.Start();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    const auto second_session = fade.Stop();
    CHECK(second_session.records.size() == 1);
    CHECK(second_session.records[0].second.stats.draw_observations == 1);
  }

  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    auto recording = Recording({draw});

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    token.Start(1);
    token.Stop();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    CHECK(recording.fade_admitted_manual_session == 0);

    token.Start(2);
    manual.Start(2, 0, kAllShaders);
    fade.Start(2);
    fade_snapshots.Start(2);
    fade_diagnostics.Start();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    const auto result = fade.Stop();
    CHECK(result.records.size() == 1);
    CHECK(result.records[0].second.stats.draw_observations == 1);
  }

  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    auto recording = Recording({draw});

    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);

    recording.Reset();
    CHECK(recording.fade_admitted_manual_session == 0);
    recording.draws.push_back(draw);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);

    const auto result = fade.Stop();
    CHECK(result.records.size() == 1);
    CHECK(result.records[0].second.stats.draw_observations == 2);
  }

  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;
    FadeControlTrackerCapacityAccumulator fade_diagnostics;

    FadeControlTrackerCapacityDiagnostics runtime_taint;
    runtime_taint.mapped_buffer_loss = true;

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    SimulateRecordDraw(draw, ComposeRecordTimeTaint(runtime_taint, false),
        {MakeFadeKey(1), Available(1.0f)});
    auto recording = Recording({draw});

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);
    fade_diagnostics.Start();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    const auto first_fade = fade.Stop();
    const auto first_taint = fade_diagnostics.Stop();
    CHECK(first_fade.records[0].second.stats.draw_observations == 1);
    CHECK(first_taint.mapped_buffer_loss);
    token.Stop();
    manual.Stop(1);
    fade_snapshots.Stop();

    token.Start(2);
    manual.Start(2, 0, kAllShaders);
    fade.Start(2);
    fade_snapshots.Start(2);
    fade_diagnostics.Start();
    SimulateExecuteCommandList(
        token, manual, fade, fade_snapshots, fade_diagnostics, recording);
    CHECK(fade.Stop().records[0].second.stats.draw_observations == 1);
    CHECK(fade_diagnostics.Stop().mapped_buffer_loss);
  }

  return 0;
}

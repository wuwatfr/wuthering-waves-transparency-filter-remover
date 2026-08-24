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
using wuwa_tfr::dev::FadeControlValueSample;
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
};

// Mirrors OnManualCaptureExecute's control flow: determine the live manual
// capture session once, then commit both the Manual Capture draw and that
// draw's pending Fade observations to that same session -- or commit
// neither if the session isn't live at submission time.
void SimulateExecuteCommandList(const ManualCaptureSessionToken& token,
    ManualCaptureAccumulator& manual, FadeControlAccumulator& fade,
    FadeControlSnapshotAccumulator& fade_snapshots,
    const std::vector<RecordedDrawSim>& draws) {
  const std::uint64_t session_id = token.value();
  if (session_id == 0 || !manual.IsLiveSession(session_id)) return;
  for (const auto& draw : draws) {
    manual.Accumulate(
        draw.manual_key, draw.pipeline, 1, 1, ManualCaptureBindingObservation{});
    CommitPendingFadeControlObservations(
        fade, fade_snapshots, draw.pipeline, draw.pending_observations, {});
  }
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

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};
    CHECK(token.value() == 0);  // no session yet at record time

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);

    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});

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

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);

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

    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});

    CHECK(manual.last_result().records.empty());
    CHECK(fade.last_result().records.empty());
  }

  // 3. Recorded and submitted while the same session is live: both admitted.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};

    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});

    const auto manual_result = manual.Stop(1);
    const auto fade_result = fade.Stop();
    CHECK(manual_result.records.size() == 1);
    CHECK(fade_result.records.size() == 1);
  }

  // 4. The same command list submitted twice contributes consistently to
  // both accumulators -- deferring the commit to submission time must not
  // silently drop or dedup repeat submissions.
  {
    ManualCaptureSessionToken token;
    ManualCaptureAccumulator manual;
    FadeControlAccumulator fade;
    FadeControlSnapshotAccumulator fade_snapshots;

    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 100};
    draw.pipeline = MakePipeline();
    draw.pending_observations = {{MakeFadeKey(1), Available(1.0f)}};

    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});
    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});

    const auto manual_result = manual.Stop(1);
    const auto fade_result = fade.Stop();
    CHECK(manual_result.records.size() == 1);
    CHECK(manual_result.records[0].second.submissions == 2);
    CHECK(fade_result.records.size() == 1);
    CHECK(fade_result.records[0].second.stats.draw_observations == 2);
    CHECK(fade_result.records[0].second.stats.available_observations == 2);
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
    token.Start(1);
    manual.Start(1, 0, kAllShaders);
    fade.Start(1);
    fade_snapshots.Start(1);

    RecordedDrawSim draw;
    draw.manual_key = ManualCaptureRecordKey{1, Geometry(1), 999};
    draw.pipeline = MakePipeline();
    draw.pending_observations = primary_pending;

    SimulateExecuteCommandList(token, manual, fade, fade_snapshots, {draw});
    const auto fade_result = fade.Stop();
    CHECK(fade_result.records.size() == 1);
    CHECK(fade_result.records[0].first.route == inherited_route);
    CHECK(!(fade_result.records[0].first.route == secondary_route));
  }

  return 0;
}

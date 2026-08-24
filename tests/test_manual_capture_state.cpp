// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/manual_capture_state.hpp"

#include <unordered_set>

#include "test_check.hpp"

namespace {

using wuwa_tfr::dev::AllocateExportFilename;
using wuwa_tfr::dev::ManualCaptureAccumulator;
using wuwa_tfr::dev::ManualCaptureBindingObservation;
using wuwa_tfr::dev::ManualCapturePipelineInfo;
using wuwa_tfr::dev::ManualCaptureRecordKey;
using wuwa_tfr::dev::ManualCaptureSessionToken;
using wuwa_tfr::dev::ManualCaptureShaderFilter;
using wuwa_tfr::dev::ManualCaptureState;
using wuwa_tfr::dev::kMaxExportFilenameAttempts;
using wuwa_tfr::dev::kMaxManualCaptureRecords;

constexpr auto kAllShaders = ManualCaptureShaderFilter::AllObservedPixelShaders;

wuwa_tfr::TraceGeometryKey Geometry(std::uint64_t vertex_resource) {
  wuwa_tfr::TraceGeometryKey key;
  key.kind = wuwa_tfr::TraceDrawKind::Indexed;
  key.arguments = {10, 1, 0, 0, 0};
  key.topology = 4;
  key.vertex_buffers.push_back({0, vertex_resource, 0, 32});
  key.index_buffer = wuwa_tfr::TraceIndexBinding{vertex_resource + 1, 0, 4};
  key.observations = wuwa_tfr::TraceObservedPso |
      wuwa_tfr::TraceObservedVertexBuffers |
      wuwa_tfr::TraceObservedIndexBuffer | wuwa_tfr::TraceObservedTopology |
      wuwa_tfr::TraceObservedPass;
  return key;
}

// Record identity is now the same stable execution-route identity the
// existing concrete trace uses: PSO incarnation + geometry + pass, with no
// binding fingerprint in the key. See ManualCaptureBindingObservation for
// the (identity-independent) per-Draw binding state.
ManualCaptureRecordKey MakeKey(std::uint64_t pso_incarnation,
    std::uint64_t vertex_resource = 1, std::uint64_t pass_fingerprint = 100) {
  return ManualCaptureRecordKey{
      pso_incarnation, Geometry(vertex_resource), pass_fingerprint};
}

ManualCapturePipelineInfo MakePipeline(
    std::uint64_t pso_incarnation, std::uint64_t shader_hash) {
  ManualCapturePipelineInfo pipeline;
  pipeline.device = 1;
  pipeline.application_pso = 0x1000;
  pipeline.pso_incarnation = pso_incarnation;
  pipeline.pso_fingerprint = 0xAAAA;
  pipeline.pso_context_hash = 0xBBBB;
  pipeline.pixel_shader_hash = shader_hash;
  return pipeline;
}

// observed_bindings bits match dev/trace/trace_events.cpp: 0x1 root
// constants, 0x2 pushed CBVs, 0x4 descriptor tables.
ManualCaptureBindingObservation MakeBinding(std::uint64_t root_constant,
    std::uint64_t pushed_cbv, std::uint64_t descriptor_table,
    std::uint8_t observed_bindings = 0x7) {
  return ManualCaptureBindingObservation{
      root_constant, pushed_cbv, descriptor_table, observed_bindings};
}

}  // namespace

int main() {
  // Inactive execution does not accumulate.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Accumulate(
        MakeKey(1), MakePipeline(1, 0x1), 3, 1, MakeBinding(1, 1, 1));
    CHECK(accumulator.state() == ManualCaptureState::Idle);
    CHECK(accumulator.Summary().record_count == 0);
  }

  // Starting a session resets any previous manual result, and stopping
  // freezes the session data.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    accumulator.Accumulate(
        MakeKey(1), MakePipeline(1, 0x1), 2, 1, MakeBinding(1, 1, 1));
    const auto first_result = accumulator.Stop(5);
    CHECK(accumulator.state() == ManualCaptureState::Captured);
    CHECK(first_result.records.size() == 1);
    CHECK(first_result.session_id == 1);
    CHECK(first_result.end_frame == 5);

    // Execution after Stop() must not mutate the frozen result.
    accumulator.Accumulate(
        MakeKey(1), MakePipeline(1, 0x1), 99, 6, MakeBinding(1, 1, 1));
    CHECK(accumulator.last_result().records.size() == 1);
    CHECK(accumulator.last_result().records[0].second.commands == 2);

    // A new session clears the previous manual-capture result and does not
    // inherit previous execution rows.
    accumulator.Start(2, 0, kAllShaders);
    CHECK(accumulator.Summary().record_count == 0);
    CHECK(accumulator.state() == ManualCaptureState::Capturing);
  }

  // Repeated identical execution aggregates commands/submissions and keeps
  // first_frame fixed while last_frame advances.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    const auto key = MakeKey(1);
    const auto pipeline = MakePipeline(1, 0x1);
    accumulator.Accumulate(key, pipeline, 2, 1, MakeBinding(1, 1, 1));
    accumulator.Accumulate(key, pipeline, 3, 4, MakeBinding(1, 1, 1));
    const auto result = accumulator.Stop(4);
    CHECK(result.records.size() == 1);
    const auto& record = result.records[0].second;
    CHECK(record.commands == 5);
    CHECK(record.submissions == 2);
    CHECK(record.first_frame == 1);
    CHECK(record.last_frame == 4);
  }

  // Same PSO + geometry + pass with different binding fingerprints
  // aggregates into ONE record (the first real in-game captures showed the
  // opposite bug: including binding fingerprints in identity fragmented one
  // stable route into thousands of near-duplicates).
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    const auto key = MakeKey(1);
    const auto pipeline = MakePipeline(1, 0x1);
    accumulator.Accumulate(key, pipeline, 1, 1, MakeBinding(10, 20, 30));
    accumulator.Accumulate(key, pipeline, 1, 2, MakeBinding(11, 21, 31));
    accumulator.Accumulate(key, pipeline, 1, 3, MakeBinding(12, 22, 32));
    const auto result = accumulator.Stop(3);
    CHECK(result.records.size() == 1);
    const auto& record = result.records[0].second;
    // Command/submission counts keep accumulating across all three Draws.
    CHECK(record.commands == 3);
    CHECK(record.submissions == 3);
    // First/last fingerprints track the first and most recent observation.
    CHECK(record.root_constants.first_fingerprint == 10);
    CHECK(record.root_constants.last_fingerprint == 12);
    CHECK(record.pushed_cbvs.first_fingerprint == 20);
    CHECK(record.pushed_cbvs.last_fingerprint == 22);
    CHECK(record.descriptor_tables.first_fingerprint == 30);
    CHECK(record.descriptor_tables.last_fingerprint == 32);
    // Fingerprints changed across observations, so all three flags are set.
    CHECK(record.root_constants.changed);
    CHECK(record.pushed_cbvs.changed);
    CHECK(record.descriptor_tables.changed);
    CHECK(record.observed_bindings == 0x7);
  }

  // Binding-changed flags stay false when every observation agrees, and
  // stay true (sticky) once a later observation happens to match again.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    const auto key = MakeKey(1);
    const auto pipeline = MakePipeline(1, 0x1);
    accumulator.Accumulate(key, pipeline, 1, 1, MakeBinding(5, 5, 5));
    accumulator.Accumulate(key, pipeline, 1, 2, MakeBinding(5, 5, 5));
    {
      const auto result = accumulator.active();
      const auto& record = result.records[0].second;
      CHECK(!record.root_constants.changed);
      CHECK(!record.pushed_cbvs.changed);
      CHECK(!record.descriptor_tables.changed);
    }
    accumulator.Accumulate(key, pipeline, 1, 3, MakeBinding(9, 9, 9));
    accumulator.Accumulate(key, pipeline, 1, 4, MakeBinding(5, 5, 5));
    const auto result = accumulator.Stop(4);
    const auto& record = result.records[0].second;
    CHECK(record.root_constants.changed);
    CHECK(record.pushed_cbvs.changed);
    CHECK(record.descriptor_tables.changed);
    CHECK(record.root_constants.last_fingerprint == 5);
  }

  // A binding kind never observed for a route (its bit never set) keeps its
  // default, unset summary rather than being folded in as a fingerprint of
  // 0.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 1, 1,
        MakeBinding(7, 0, 0, /*observed_bindings=*/0x1));
    const auto result = accumulator.Stop(1);
    const auto& record = result.records[0].second;
    CHECK(record.observed_bindings == 0x1);
    CHECK(record.root_constants.first_fingerprint == 7);
    CHECK(record.pushed_cbvs.first_fingerprint == 0);
    CHECK(!record.pushed_cbvs.changed);
  }

  // Distinct PSO incarnation, distinct geometry, and distinct pass each
  // remain distinct records even with identical binding observations.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    const auto binding = MakeBinding(1, 1, 1);
    accumulator.Accumulate(
        MakeKey(1), MakePipeline(1, 0x1), 1, 1, binding);  // baseline
    accumulator.Accumulate(MakeKey(2), MakePipeline(2, 0x2), 1, 1,
        binding);  // distinct PSO incarnation
    accumulator.Accumulate(MakeKey(1, /*vertex_resource=*/2), MakePipeline(1, 0x1),
        1, 1, binding);  // distinct geometry
    accumulator.Accumulate(MakeKey(1, /*vertex_resource=*/1, /*pass_fingerprint=*/200),
        MakePipeline(1, 0x1), 1, 1, binding);  // distinct pass
    const auto result = accumulator.Stop(1);
    CHECK(result.records.size() == 4);
  }

  // Capacity overflow still applies, now to stable-route records: exceeding
  // it becomes explicitly incomplete without discarding records already
  // captured or crashing.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    const auto binding = MakeBinding(1, 1, 1);
    for (std::size_t i = 0; i < kMaxManualCaptureRecords + 8; ++i) {
      accumulator.Accumulate(MakeKey(static_cast<std::uint64_t>(i) + 1),
          MakePipeline(1, 0x1), 1, 1, binding);
    }
    const auto result = accumulator.Stop(1);
    CHECK(result.records.size() == kMaxManualCaptureRecords);
    CHECK(result.capacity_exceeded);
  }

  // The shader filter is snapshotted at Start and carried into both the
  // active session and the frozen result.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(
        1, 0, ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly);
    CHECK(accumulator.active().shader_filter ==
        ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly);
    const auto result = accumulator.Stop(0);
    CHECK(result.shader_filter ==
        ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly);
    CHECK(accumulator.Summary().shader_filter ==
        ManualCaptureShaderFilter::VerifiedFadePrimitiveOnly);
  }

  // Clear only affects the frozen result, not an in-progress session, and
  // Clear while Capturing is a no-op.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    accumulator.Accumulate(
        MakeKey(1), MakePipeline(1, 0x1), 1, 1, MakeBinding(1, 1, 1));
    accumulator.Clear();
    CHECK(accumulator.state() == ManualCaptureState::Capturing);
    CHECK(accumulator.Summary().record_count == 1);

    accumulator.Stop(1);
    accumulator.Clear();
    CHECK(accumulator.state() == ManualCaptureState::Idle);
    CHECK(accumulator.Summary().record_count == 0);
  }

  // ManualCaptureSessionToken: the one shared, lock-free "is a manual
  // capture session live, and which one" signal other channels (fade-value,
  // fade-snapshot) participate in the session through. 0 while idle;
  // Start() makes it that session's id; Stop() always returns it to 0,
  // never leaking the previous session's id into a later idle window.
  {
    ManualCaptureSessionToken token;
    CHECK(token.value() == 0);
    token.Start(1);
    CHECK(token.value() == 1);
    token.Start(2);  // a later session's id fully replaces the earlier one
    CHECK(token.value() == 2);
    token.Stop();
    CHECK(token.value() == 0);
  }

  // AllocateExportFilename: no collision returns the bare stem+extension.
  {
    std::unordered_set<std::string> existing;
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    CHECK(AllocateExportFilename("manual-capture-20260823-220500", ".tsv",
              exists) == "manual-capture-20260823-220500.tsv");
  }

  // A single collision (two captures stopped in the same second) picks the
  // "-1" suffix; a second collision advances to "-2".
  {
    std::unordered_set<std::string> existing = {
        "manual-capture-20260823-220500.tsv"};
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    CHECK(AllocateExportFilename("manual-capture-20260823-220500", ".tsv",
              exists) == "manual-capture-20260823-220500-1.tsv");

    existing.insert("manual-capture-20260823-220500-1.tsv");
    CHECK(AllocateExportFilename("manual-capture-20260823-220500", ".tsv",
              exists) == "manual-capture-20260823-220500-2.tsv");
  }

  // Exhausting every numbered suffix returns the last candidate instead of
  // looping forever.
  {
    std::unordered_set<std::string> existing = {"stem.tsv"};
    for (int suffix = 1; suffix <= kMaxExportFilenameAttempts; ++suffix)
      existing.insert("stem-" + std::to_string(suffix) + ".tsv");
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    CHECK(AllocateExportFilename("stem", ".tsv", exists) ==
        "stem-" + std::to_string(kMaxExportFilenameAttempts) + ".tsv");
  }

  return 0;
}

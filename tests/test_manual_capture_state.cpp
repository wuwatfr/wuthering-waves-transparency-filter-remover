// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/manual_capture_state.hpp"

#include "test_check.hpp"

namespace {

using wuwa_tfr::dev::ManualCaptureAccumulator;
using wuwa_tfr::dev::ManualCapturePipelineInfo;
using wuwa_tfr::dev::ManualCaptureRecordKey;
using wuwa_tfr::dev::ManualCaptureState;
using wuwa_tfr::dev::kMaxManualCaptureRecords;

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

ManualCaptureRecordKey MakeKey(std::uint64_t pso_incarnation,
    std::uint64_t vertex_resource = 1, std::uint64_t pass_fingerprint = 100,
    std::uint64_t root_constant_fingerprint = 0,
    std::uint64_t pushed_cbv_fingerprint = 0,
    std::uint64_t descriptor_table_fingerprint = 0,
    std::uint8_t observed_bindings = 0) {
  return ManualCaptureRecordKey{pso_incarnation, Geometry(vertex_resource),
      pass_fingerprint, root_constant_fingerprint, pushed_cbv_fingerprint,
      descriptor_table_fingerprint, observed_bindings};
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

}  // namespace

int main() {
  // Inactive execution does not accumulate.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 3, 1);
    CHECK(accumulator.state() == ManualCaptureState::Idle);
    CHECK(accumulator.Summary().record_count == 0);
  }

  // Starting a session resets any previous manual result, and stopping
  // freezes the session data.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0);
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 2, 1);
    const auto first_result = accumulator.Stop(5);
    CHECK(accumulator.state() == ManualCaptureState::Captured);
    CHECK(first_result.records.size() == 1);
    CHECK(first_result.session_id == 1);
    CHECK(first_result.end_frame == 5);

    // Execution after Stop() must not mutate the frozen result.
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 99, 6);
    CHECK(accumulator.last_result().records.size() == 1);
    CHECK(accumulator.last_result().records[0].second.commands == 2);

    // A new session clears the previous manual-capture result and does not
    // inherit previous execution rows.
    accumulator.Start(2, 0);
    CHECK(accumulator.Summary().record_count == 0);
    CHECK(accumulator.state() == ManualCaptureState::Capturing);
  }

  // Repeated identical execution aggregates commands/submissions and keeps
  // first_frame fixed while last_frame advances.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0);
    const auto key = MakeKey(1);
    const auto pipeline = MakePipeline(1, 0x1);
    accumulator.Accumulate(key, pipeline, 2, 1);
    accumulator.Accumulate(key, pipeline, 3, 4);
    const auto result = accumulator.Stop(4);
    CHECK(result.records.size() == 1);
    const auto& record = result.records[0].second;
    CHECK(record.commands == 5);
    CHECK(record.submissions == 2);
    CHECK(record.first_frame == 1);
    CHECK(record.last_frame == 4);
  }

  // Distinct PSO incarnation, distinct shader (via a distinct incarnation),
  // and distinct pass/binding routes each remain distinct records.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0);
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 1, 1);
    accumulator.Accumulate(MakeKey(2), MakePipeline(2, 0x2), 1, 1);
    accumulator.Accumulate(
        MakeKey(1, /*vertex_resource=*/1, /*pass_fingerprint=*/200),
        MakePipeline(1, 0x1), 1, 1);
    accumulator.Accumulate(
        MakeKey(1, /*vertex_resource=*/1, /*pass_fingerprint=*/100,
            /*root_constant_fingerprint=*/7),
        MakePipeline(1, 0x1), 1, 1);
    const auto result = accumulator.Stop(1);
    CHECK(result.records.size() == 4);
  }

  // Capacity overflow becomes explicitly incomplete, without discarding
  // records already captured or crashing.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0);
    for (std::size_t i = 0; i < kMaxManualCaptureRecords + 8; ++i) {
      accumulator.Accumulate(
          MakeKey(static_cast<std::uint64_t>(i) + 1), MakePipeline(1, 0x1),
          1, 1);
    }
    const auto result = accumulator.Stop(1);
    CHECK(result.records.size() == kMaxManualCaptureRecords);
    CHECK(result.capacity_exceeded);
  }

  // Clear only affects the frozen result, not an in-progress session, and
  // Clear while Capturing is a no-op.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0);
    accumulator.Accumulate(MakeKey(1), MakePipeline(1, 0x1), 1, 1);
    accumulator.Clear();
    CHECK(accumulator.state() == ManualCaptureState::Capturing);
    CHECK(accumulator.Summary().record_count == 1);

    accumulator.Stop(1);
    accumulator.Clear();
    CHECK(accumulator.state() == ManualCaptureState::Idle);
    CHECK(accumulator.Summary().record_count == 0);
  }

  return 0;
}

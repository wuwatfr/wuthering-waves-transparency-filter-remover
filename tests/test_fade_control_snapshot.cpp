// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_snapshot.hpp"

#include "test_check.hpp"

namespace {

using wuwa_tfr::dev::FadeControlPipelineIdentity;
using wuwa_tfr::dev::FadeControlRecordKey;
using wuwa_tfr::dev::FadeControlRole;
using wuwa_tfr::dev::FadeControlSnapshotAccumulator;
using wuwa_tfr::dev::FadeControlSnapshotRecord;
using wuwa_tfr::dev::kMaxFadeControlSnapshotBytes;
using wuwa_tfr::dev::kMaxFadeControlSnapshots;
using wuwa_tfr::dev::ResolveFadeControlSnapshotWindow;

wuwa_tfr::TraceGeometryKey Geometry(std::uint64_t vertex_resource) {
  wuwa_tfr::TraceGeometryKey key;
  key.kind = wuwa_tfr::TraceDrawKind::Indexed;
  key.arguments = {10, 1, 0, 0, 0};
  key.topology = 4;
  key.vertex_buffers.push_back({0, vertex_resource, 0, 32});
  key.index_buffer = wuwa_tfr::TraceIndexBinding{vertex_resource + 1, 0, 4};
  return key;
}

wuwa_tfr::TraceConcreteDrawKey Route(std::uint64_t pso_incarnation) {
  return wuwa_tfr::TraceConcreteDrawKey{pso_incarnation, Geometry(1), 100};
}

FadeControlRecordKey MakeKey(std::uint64_t pso_incarnation = 1,
    std::uint64_t runtime_resource_incarnation = 1) {
  FadeControlRecordKey key;
  key.route = Route(pso_incarnation);
  key.role = FadeControlRole::Predicate;
  key.runtime_resource_incarnation = runtime_resource_incarnation;
  return key;
}

FadeControlPipelineIdentity MakePipeline() {
  FadeControlPipelineIdentity pipeline;
  pipeline.device = 1;
  pipeline.pixel_shader_hash = 0x1;
  return pipeline;
}

}  // namespace

int main() {
  // 1. Normal case: vector far from both mapped ends -- the full symmetric
  // window is granted, exactly kMaxFadeControlSnapshotBytes wide.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        /*cbv_offset=*/0, /*predicate_vector=*/100, /*radius_vectors=*/16,
        /*mapped_start=*/0, /*mapped_end=*/1u << 20);
    CHECK(window.valid);
    CHECK(window.size == kMaxFadeControlSnapshotBytes);
    CHECK(window.start == (100 - 16) * 16);
  }

  // 2. Low-end clamp: predicate_vector smaller than the radius must clamp
  // to the CBV's own start (vector 0), never underflow.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        /*cbv_offset=*/256, /*predicate_vector=*/3, /*radius_vectors=*/16,
        /*mapped_start=*/0, /*mapped_end=*/1u << 20);
    CHECK(window.valid);
    CHECK(window.start == 256);  // vector 0 of this CBV, not negative
    CHECK(window.size == (3 + 16 + 1) * 16);
  }

  // 3. High-end clamp: a small mapped region caps the window at its real
  // end rather than reading past it.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        /*cbv_offset=*/0, /*predicate_vector=*/100, /*radius_vectors=*/16,
        /*mapped_start=*/0, /*mapped_end=*/(100 * 16) + 8);
    CHECK(window.valid);
    CHECK(window.start == (100 - 16) * 16);
    CHECK(window.start + window.size == (100 * 16) + 8);
  }

  // 4. Empty/invalid mapped region -> no window, not a zero-sized "valid"
  // one.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        0, 100, 16, /*mapped_start=*/500, /*mapped_end=*/500);
    CHECK(!window.valid);
    const auto inverted = ResolveFadeControlSnapshotWindow(
        0, 100, 16, /*mapped_start=*/500, /*mapped_end=*/100);
    CHECK(!inverted.valid);
  }

  // 5. Desired window entirely outside the mapped region -> no window.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        /*cbv_offset=*/0, /*predicate_vector=*/1000, /*radius_vectors=*/16,
        /*mapped_start=*/0, /*mapped_end=*/100);
    CHECK(!window.valid);
  }

  // 6. Defensive size cap: an oversized radius must never produce a window
  // wider than kMaxFadeControlSnapshotBytes, regardless of how generous the
  // mapped region is.
  {
    const auto window = ResolveFadeControlSnapshotWindow(
        /*cbv_offset=*/0, /*predicate_vector=*/10000, /*radius_vectors=*/5000,
        /*mapped_start=*/0, /*mapped_end=*/1ull << 30);
    CHECK(window.valid);
    CHECK(window.size == kMaxFadeControlSnapshotBytes);
  }

  // 7. Dedup: ShouldCapture is true once, then false after Commit, and
  // stays false on repeat query.
  {
    FadeControlSnapshotAccumulator accumulator;
    const auto key = MakeKey();
    const auto pipeline = MakePipeline();
    CHECK(!accumulator.ShouldCapture(key));  // not started yet

    accumulator.Start(1);
    CHECK(accumulator.ShouldCapture(key));

    FadeControlSnapshotRecord record;
    record.pipeline = pipeline;
    record.window_size = 16;
    accumulator.Commit(key, record);
    CHECK(!accumulator.ShouldCapture(key));

    // A second Commit for the same key must not replace or duplicate it.
    FadeControlSnapshotRecord second_record;
    second_record.pipeline = pipeline;
    second_record.window_size = 999;
    accumulator.Commit(key, second_record);

    const auto result = accumulator.Stop();
    CHECK(result.snapshots.size() == 1);
    CHECK(result.snapshots[0].second.window_size == 16);  // first wins
  }

  // 8. Distinct keys each get their own entry.
  {
    FadeControlSnapshotAccumulator accumulator;
    accumulator.Start(1);
    FadeControlSnapshotRecord record;
    record.pipeline = MakePipeline();
    accumulator.Commit(MakeKey(1), record);
    accumulator.Commit(MakeKey(2), record);
    accumulator.Commit(MakeKey(1, 2), record);  // distinct runtime binding
    const auto result = accumulator.Stop();
    CHECK(result.snapshots.size() == 3);
  }

  // 9. Capacity overflow becomes explicit without discarding records
  // already captured or crashing.
  {
    FadeControlSnapshotAccumulator accumulator;
    accumulator.Start(1);
    FadeControlSnapshotRecord record;
    record.pipeline = MakePipeline();
    for (std::size_t i = 0; i < kMaxFadeControlSnapshots + 8; ++i)
      accumulator.Commit(MakeKey(static_cast<std::uint64_t>(i) + 1), record);
    const auto result = accumulator.Stop();
    CHECK(result.snapshots.size() == kMaxFadeControlSnapshots);
    CHECK(result.capacity_exceeded);
  }

  // 10. Inactive accumulator does not capture; Start resets the previous
  // result.
  {
    FadeControlSnapshotAccumulator accumulator;
    FadeControlSnapshotRecord record;
    record.pipeline = MakePipeline();
    accumulator.Commit(MakeKey(), record);
    CHECK(!accumulator.active());
    CHECK(accumulator.last_result().snapshots.empty());

    accumulator.Start(1);
    accumulator.Commit(MakeKey(), record);
    const auto first_result = accumulator.Stop();
    CHECK(first_result.snapshots.size() == 1);

    accumulator.Start(2);
    // Start() clears both the in-progress set AND last_result() -- the same
    // convention FadeControlAccumulator::Start() already uses.
    CHECK(accumulator.active_snapshot().snapshots.empty());
    CHECK(accumulator.last_result().snapshots.empty());
    // A new session also clears the dedup index, so a key already captured
    // in the previous session may be captured again in this one.
    CHECK(accumulator.ShouldCapture(MakeKey()));
  }

  return 0;
}

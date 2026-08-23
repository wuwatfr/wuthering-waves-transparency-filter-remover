// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_state.hpp"

#include <cstring>

#include "test_check.hpp"

namespace {

using wuwa_tfr::dev::FadeControlAccumulator;
using wuwa_tfr::dev::FadeControlPipelineIdentity;
using wuwa_tfr::dev::FadeControlRecordKey;
using wuwa_tfr::dev::FadeControlRole;
using wuwa_tfr::dev::FadeControlValueSample;
using wuwa_tfr::dev::FadeControlValueStats;
using wuwa_tfr::dev::kFadeControlReasonBindingUnresolved;
using wuwa_tfr::dev::kFadeControlReasonNotMapped;
using wuwa_tfr::dev::kFadeControlReasonOutOfRange;
using wuwa_tfr::dev::kMaxFadeControlDistinctValues;
using wuwa_tfr::dev::kMaxFadeControlRecords;

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

FadeControlRecordKey MakeKey(std::uint64_t pso_incarnation = 1,
    std::uint32_t primitive_index = 0,
    FadeControlRole role = FadeControlRole::Predicate,
    std::uint32_t cbuffer_space = 0, std::uint32_t cbuffer_register = 0,
    std::uint32_t vector_index = 0, std::uint32_t component = 0,
    std::uint64_t runtime_resource_incarnation = 1,
    std::uint64_t runtime_range_offset = 0) {
  return FadeControlRecordKey{Route(pso_incarnation), primitive_index, role,
      cbuffer_space, cbuffer_register, vector_index, component,
      runtime_resource_incarnation, runtime_range_offset};
}

FadeControlPipelineIdentity MakePipeline() {
  FadeControlPipelineIdentity pipeline;
  pipeline.device = 1;
  pipeline.application_pso = 0x1000;
  pipeline.pso_incarnation = 1;
  pipeline.pso_context_hash = 0xBBBB;
  pipeline.pixel_shader_hash = 0x1;
  return pipeline;
}

FadeControlValueSample Available(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return FadeControlValueSample{true, bits, 0};
}

FadeControlValueSample Unavailable(std::uint16_t reason) {
  return FadeControlValueSample{false, 0, reason};
}

}  // namespace

int main() {
  // 4. Byte offset formula.
  {
    CHECK(wuwa_tfr::dev::ResolveFadeControlByteOffset(0, 0, 0) == 0);
    CHECK(wuwa_tfr::dev::ResolveFadeControlByteOffset(0, 1, 0) == 16);
    CHECK(wuwa_tfr::dev::ResolveFadeControlByteOffset(0, 0, 2) == 8);
    CHECK(wuwa_tfr::dev::ResolveFadeControlByteOffset(256, 3, 1) ==
        256 + 3 * 16 + 1 * 4);
  }

  // 5. Mapped-region bounds checking.
  {
    using wuwa_tfr::dev::FadeControlByteOffsetInMappedRegion;
    // Fully inside.
    CHECK(FadeControlByteOffsetInMappedRegion(100, 0, 200));
    // Exactly touches the end (last valid 4-byte read).
    CHECK(FadeControlByteOffsetInMappedRegion(96, 0, 100));
    // One byte past what fits.
    CHECK(!FadeControlByteOffsetInMappedRegion(97, 0, 100));
    // Before the mapped region entirely.
    CHECK(!FadeControlByteOffsetInMappedRegion(10, 20, 100));
    // Non-zero mapped_offset, value inside.
    CHECK(FadeControlByteOffsetInMappedRegion(120, 100, 24));
    // Non-zero mapped_offset, value out of range.
    CHECK(!FadeControlByteOffsetInMappedRegion(121, 100, 24));
  }

  // 6/7/8: repeated observations of the same control source aggregate,
  // update the changed flag, and track finite min/max.
  {
    FadeControlValueStats stats;
    stats.Observe(Available(1.0f));
    CHECK(stats.draw_observations == 1);
    CHECK(stats.available_observations == 1);
    CHECK(!stats.changed);
    CHECK(stats.finite_min == 1.0f && stats.finite_max == 1.0f);

    stats.Observe(Available(1.0f));
    CHECK(!stats.changed);  // still identical -- no change yet.

    stats.Observe(Available(0.25f));
    CHECK(stats.changed);   // now differs from the running last value.
    CHECK(stats.finite_min == 0.25f);
    CHECK(stats.finite_max == 1.0f);

    stats.Observe(Available(1.0f));
    CHECK(stats.changed);   // sticky: stays true even though it matches
                             // the very first observation again.
    CHECK(stats.first_bits ==
        [] { std::uint32_t b; float v = 1.0f; std::memcpy(&b, &v, 4); return b; }());
    CHECK(stats.available_observations == 4);
    CHECK(stats.draw_observations == 4);
  }

  // 9. Distinct raw values capped at 32, with an explicit overflow flag.
  {
    FadeControlValueStats stats;
    for (int i = 0; i < 40; ++i)
      stats.Observe(Available(static_cast<float>(i)));
    CHECK(stats.distinct_value_count == kMaxFadeControlDistinctValues);
    CHECK(stats.distinct_overflow);
    CHECK(stats.available_observations == 40);
    // Re-observing an already-tracked value still counts, without growing
    // the (already full) distinct table.
    stats.Observe(Available(0.0f));
    CHECK(stats.distinct_value_count == kMaxFadeControlDistinctValues);
    CHECK(stats.available_observations == 41);
  }

  // 10. Unavailable reasons accumulate into a bitmask across observations,
  // and never silently substitute a zero value.
  {
    FadeControlValueStats stats;
    stats.Observe(Unavailable(kFadeControlReasonNotMapped));
    stats.Observe(Unavailable(kFadeControlReasonOutOfRange));
    stats.Observe(Available(2.0f));
    CHECK(stats.unavailable_observations == 2);
    CHECK(stats.available_observations == 1);
    CHECK((stats.unavailable_reason_mask & kFadeControlReasonNotMapped) != 0);
    CHECK((stats.unavailable_reason_mask & kFadeControlReasonOutOfRange) != 0);
    CHECK((stats.unavailable_reason_mask &
              kFadeControlReasonBindingUnresolved) == 0);
    // The available observation's value is real, not a stand-in zero.
    CHECK(stats.first_bits ==
        [] { std::uint32_t b; float v = 2.0f; std::memcpy(&b, &v, 4); return b; }());
  }

  // Inactive accumulator does not accumulate; Start resets the previous
  // result.
  {
    FadeControlAccumulator accumulator;
    accumulator.Observe(MakeKey(), MakePipeline(), Available(1.0f));
    CHECK(!accumulator.active());
    CHECK(accumulator.active_snapshot().records.empty());

    accumulator.Start(1);
    accumulator.Observe(MakeKey(), MakePipeline(), Available(1.0f));
    const auto first_result = accumulator.Stop();
    CHECK(first_result.records.size() == 1);
    CHECK(first_result.session_id == 1);

    accumulator.Start(2);
    CHECK(accumulator.active_snapshot().records.empty());
  }

  // 11. Different primitive/source/route remain distinct records; same key
  // repeated aggregates into one.
  {
    FadeControlAccumulator accumulator;
    accumulator.Start(1);
    const auto pipeline = MakePipeline();
    accumulator.Observe(MakeKey(1), pipeline, Available(1.0f));
    accumulator.Observe(MakeKey(1), pipeline, Available(2.0f));  // same key
    accumulator.Observe(
        MakeKey(2), pipeline, Available(1.0f));  // distinct route (PSO)
    accumulator.Observe(MakeKey(1, /*primitive_index=*/1), pipeline,
        Available(1.0f));  // distinct primitive
    accumulator.Observe(
        MakeKey(1, 0, FadeControlRole::Coverage), pipeline,
        Available(1.0f));  // distinct role
    accumulator.Observe(
        MakeKey(1, 0, FadeControlRole::Predicate, /*cbuffer_space=*/1),
        pipeline, Available(1.0f));  // distinct static source
    accumulator.Observe(
        MakeKey(1, 0, FadeControlRole::Predicate, 0, 0, 0, 0,
            /*runtime_resource_incarnation=*/2),
        pipeline, Available(1.0f));  // distinct runtime binding
    const auto result = accumulator.Stop();
    CHECK(result.records.size() == 6);
    for (const auto& [key, record] : result.records) {
      if (key == MakeKey(1)) CHECK(record.stats.available_observations == 2);
    }
  }

  // 12. Capacity overflow becomes explicit without discarding records
  // already captured or crashing.
  {
    FadeControlAccumulator accumulator;
    accumulator.Start(1);
    const auto pipeline = MakePipeline();
    for (std::size_t i = 0; i < kMaxFadeControlRecords + 8; ++i) {
      accumulator.Observe(
          MakeKey(static_cast<std::uint64_t>(i) + 1), pipeline,
          Available(1.0f));
    }
    const auto result = accumulator.Stop();
    CHECK(result.records.size() == kMaxFadeControlRecords);
    CHECK(result.capacity_exceeded);
  }

  return 0;
}

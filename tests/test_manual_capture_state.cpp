// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/manual_capture_state.hpp"

#include <unordered_set>

#include "test_check.hpp"

namespace {

using wuwa_tfr::dev::AllocateExportFilename;
using wuwa_tfr::dev::AllocateExportFilenameGroup;
using wuwa_tfr::dev::ManualCaptureAccumulator;
using wuwa_tfr::ExecutionPipelineIdentity;
using wuwa_tfr::dev::ManualCaptureBindingObservation;
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

ExecutionPipelineIdentity MakePipeline(
    std::uint64_t pso_incarnation, std::uint64_t shader_hash) {
  ExecutionPipelineIdentity pipeline;
  pipeline.device = 1;
  pipeline.application_pipeline = 0x1000;
  pipeline.incarnation_id = pso_incarnation;
  pipeline.pso_fingerprint = 0xAAAA;
  pipeline.context_hash = 0xBBBB;
  pipeline.shader_hash = shader_hash;
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

  // ManualCaptureSessionToken::StillLive: the exact-session-id validation
  // logic every Draw-time channel re-checks before committing work. Never
  // true for session id 0 (an uncaptured/idle token value), even if the
  // token itself happens to read 0.
  {
    ManualCaptureSessionToken token;
    CHECK(!token.StillLive(0));
    CHECK(!token.StillLive(1));  // not started yet

    token.Start(1);
    CHECK(token.StillLive(1));
    CHECK(!token.StillLive(2));  // a different id was never live

    token.Stop();
    CHECK(!token.StillLive(1));  // stopped: no id is live anymore
  }

  // Regression: stale work captured under session A must never be
  // admitted into session B, even though B is also Capturing by the time
  // the check runs -- exactly the scenario a bare "state == Capturing"
  // check (without also comparing the captured session id) would miss.
  // Deterministic: models the interleaving directly, no threads/sleeps.
  {
    ManualCaptureSessionToken token;
    token.Start(1);
    const std::uint64_t captured_session_id = token.value();
    CHECK(captured_session_id == 1);

    // Session A stops and session B starts before the captured id is
    // re-checked (e.g. an inspection-cache lookup ran in between).
    token.Stop();
    token.Start(2);

    CHECK(!token.StillLive(captured_session_id));
    CHECK(token.StillLive(token.value()));
  }

  // Same regression, against ManualCaptureAccumulator::IsLiveSession --
  // the authoritative, mutex-protected counterpart channels re-check under
  // g_trace_mutex once they actually have work to commit.
  {
    ManualCaptureAccumulator accumulator;
    accumulator.Start(1, 0, kAllShaders);
    CHECK(accumulator.IsLiveSession(1));
    CHECK(!accumulator.IsLiveSession(2));

    // Work captured under session 1's id must not be accepted once session
    // 1 has stopped and session 2 has started, even though the accumulator
    // is Capturing again by then.
    accumulator.Stop(0);
    accumulator.Start(2, 0, kAllShaders);
    CHECK(accumulator.state() == ManualCaptureState::Capturing);
    CHECK(!accumulator.IsLiveSession(1));
    CHECK(accumulator.IsLiveSession(2));
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

  // Exhausting every numbered suffix reports failure. It must never hand back
  // an occupied name: callers open the result with std::ios::trunc, so the
  // old "return the last candidate" behaviour destroyed the export it had
  // just collided with.
  {
    std::unordered_set<std::string> existing = {"stem.tsv"};
    for (int suffix = 1; suffix <= kMaxExportFilenameAttempts; ++suffix)
      existing.insert("stem-" + std::to_string(suffix) + ".tsv");
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    CHECK(!AllocateExportFilename("stem", ".tsv", exists).has_value());
  }

  // Freeing any one of the exhausted candidates makes allocation succeed
  // again, and the name it returns is the free one -- never an occupied one.
  {
    std::unordered_set<std::string> existing = {"stem.tsv"};
    for (int suffix = 1; suffix <= kMaxExportFilenameAttempts; ++suffix)
      existing.insert("stem-" + std::to_string(suffix) + ".tsv");
    existing.erase("stem-500.tsv");
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    const auto allocated = AllocateExportFilename("stem", ".tsv", exists);
    CHECK(allocated.has_value());
    CHECK(*allocated == "stem-500.tsv");
    CHECK(!existing.contains(*allocated));
  }

  // Investigation-range exports share this allocator rather than deriving a
  // filename straight from the timestamp: two exports in the same second get
  // distinct files, and an existing one is never selected for overwrite.
  {
    std::unordered_set<std::string> existing;
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    const std::string stem = "investigation-range-20260825-174500";

    const auto first = AllocateExportFilename(stem, ".tsv", exists);
    CHECK(first.has_value());
    CHECK(*first == stem + ".tsv");
    existing.insert(*first);

    const auto second = AllocateExportFilename(stem, ".tsv", exists);
    CHECK(second.has_value());
    CHECK(*second == stem + "-1.tsv");
    CHECK(*second != *first);
    existing.insert(*second);

    const auto third = AllocateExportFilename(stem, ".tsv", exists);
    CHECK(third.has_value());
    CHECK(*third == stem + "-2.tsv");
    existing.insert(*third);

    // Every allocation picked a name that did not already exist.
    CHECK(existing.size() == 3);
  }

  // The Manual Capture export set: with Fade-control enabled one Stop+export
  // writes three files, and they must stay correlated by one shared suffix.
  // Allocating them independently let a collision in only one stem give that
  // file a "-1" while the others kept the bare timestamp.
  {
    const std::string stamp = "20260825-204500";
    const std::vector<std::string> stems{"manual-capture-" + stamp,
        "manual-fade-controls-" + stamp, "manual-fade-snapshots-" + stamp};

    // No collision: all three bare.
    {
      std::unordered_set<std::string> existing;
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      const auto allocated = AllocateExportFilenameGroup(stems, ".tsv", exists);
      CHECK(allocated.has_value());
      CHECK((*allocated)[0] == "manual-capture-" + stamp + ".tsv");
      CHECK((*allocated)[1] == "manual-fade-controls-" + stamp + ".tsv");
      CHECK((*allocated)[2] == "manual-fade-snapshots-" + stamp + ".tsv");
    }

    // A collision in ONLY the manual-capture base advances all three.
    {
      std::unordered_set<std::string> existing = {
          "manual-capture-" + stamp + ".tsv"};
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      const auto allocated = AllocateExportFilenameGroup(stems, ".tsv", exists);
      CHECK(allocated.has_value());
      CHECK((*allocated)[0] == "manual-capture-" + stamp + "-1.tsv");
      CHECK((*allocated)[1] == "manual-fade-controls-" + stamp + "-1.tsv");
      CHECK((*allocated)[2] == "manual-fade-snapshots-" + stamp + "-1.tsv");
    }

    // A collision in ONLY the fade-controls base advances all three too --
    // the member that collided does not matter.
    {
      std::unordered_set<std::string> existing = {
          "manual-fade-controls-" + stamp + ".tsv"};
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      const auto allocated = AllocateExportFilenameGroup(stems, ".tsv", exists);
      CHECK(allocated.has_value());
      CHECK((*allocated)[0] == "manual-capture-" + stamp + "-1.tsv");
      CHECK((*allocated)[1] == "manual-fade-controls-" + stamp + "-1.tsv");
      CHECK((*allocated)[2] == "manual-fade-snapshots-" + stamp + "-1.tsv");
    }

    // Collisions spread across different members and different suffixes: the
    // set advances to the first suffix free for every member.
    {
      std::unordered_set<std::string> existing = {
          "manual-fade-snapshots-" + stamp + ".tsv",
          "manual-capture-" + stamp + "-1.tsv"};
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      const auto allocated = AllocateExportFilenameGroup(stems, ".tsv", exists);
      CHECK(allocated.has_value());
      CHECK((*allocated)[0] == "manual-capture-" + stamp + "-2.tsv");
      CHECK((*allocated)[1] == "manual-fade-controls-" + stamp + "-2.tsv");
      CHECK((*allocated)[2] == "manual-fade-snapshots-" + stamp + "-2.tsv");
      // Nothing already on disk was selected.
      for (const auto& name : *allocated) CHECK(!existing.contains(name));
    }

    // Exhaustion fails outright rather than returning an occupied name, so
    // no file in the set is opened with truncation.
    {
      std::unordered_set<std::string> existing;
      for (const auto& stem : stems) {
        existing.insert(stem + ".tsv");
        for (int attempt = 1; attempt <= kMaxExportFilenameAttempts; ++attempt)
          existing.insert(stem + "-" + std::to_string(attempt) + ".tsv");
      }
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      CHECK(!AllocateExportFilenameGroup(stems, ".tsv", exists).has_value());
    }

    // Fade-control disabled: the set is just the one manual-capture file, and
    // the same allocator handles it.
    {
      std::unordered_set<std::string> existing = {
          "manual-capture-" + stamp + ".tsv"};
      const auto exists = [&existing](const std::string& candidate) {
        return existing.contains(candidate);
      };
      const std::vector<std::string> alone{"manual-capture-" + stamp};
      const auto allocated = AllocateExportFilenameGroup(alone, ".tsv", exists);
      CHECK(allocated.has_value());
      CHECK(allocated->size() == 1);
      CHECK((*allocated)[0] == "manual-capture-" + stamp + "-1.tsv");
    }
  }

  // The group allocator reports failure the same way, so the correlated
  // three-file trace export cannot silently overwrite a previous group.
  {
    const std::vector<std::string> stems{"a", "b", "c"};
    std::unordered_set<std::string> existing;
    for (const auto& stem : stems) {
      existing.insert(stem + ".tsv");
      for (int attempt = 1; attempt <= kMaxExportFilenameAttempts; ++attempt)
        existing.insert(stem + "-" + std::to_string(attempt) + ".tsv");
    }
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    CHECK(!AllocateExportFilenameGroup(stems, ".tsv", exists).has_value());
  }

  // AllocateExportFilenameGroup: first export of a group gets the bare
  // stem+extension for every member -- no group member exists yet.
  {
    std::unordered_set<std::string> existing;
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    const auto allocated = AllocateExportFilenameGroup(
        {"runtime-trace-20260825-071000",
            "concrete-submission-trace-20260825-071000",
            "lifecycle-ambiguities-20260825-071000"},
        ".tsv", exists);
    CHECK(allocated.has_value());
    const auto& filenames = *allocated;
    CHECK(filenames.size() == 3);
    CHECK(filenames[0] == "runtime-trace-20260825-071000.tsv");
    CHECK(filenames[1] == "concrete-submission-trace-20260825-071000.tsv");
    CHECK(filenames[2] == "lifecycle-ambiguities-20260825-071000.tsv");
  }

  // A collision on just ONE member of the group must not leave the group
  // correlated by different suffixes -- every filename advances together.
  {
    std::unordered_set<std::string> existing = {
        "runtime-trace-20260825-071000.tsv"};
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    const auto allocated = AllocateExportFilenameGroup(
        {"runtime-trace-20260825-071000",
            "concrete-submission-trace-20260825-071000",
            "lifecycle-ambiguities-20260825-071000"},
        ".tsv", exists);
    CHECK(allocated.has_value());
    const auto& filenames = *allocated;
    CHECK(filenames[0] == "runtime-trace-20260825-071000-1.tsv");
    CHECK(filenames[1] == "concrete-submission-trace-20260825-071000-1.tsv");
    CHECK(filenames[2] == "lifecycle-ambiguities-20260825-071000-1.tsv");

    // None of the previously-allocated group's files were touched by the
    // second allocation: the caller never even queried a name that
    // resolves to something already in `existing` beyond the one seed.
    CHECK(existing.size() == 1);
  }

  // A second collision (this time hitting a different member than before)
  // still advances the whole group together, to "-2".
  {
    std::unordered_set<std::string> existing = {
        "runtime-trace-20260825-071000.tsv",
        "runtime-trace-20260825-071000-1.tsv",
        "lifecycle-ambiguities-20260825-071000-1.tsv"};
    const auto exists = [&existing](const std::string& candidate) {
      return existing.contains(candidate);
    };
    const auto allocated = AllocateExportFilenameGroup(
        {"runtime-trace-20260825-071000",
            "concrete-submission-trace-20260825-071000",
            "lifecycle-ambiguities-20260825-071000"},
        ".tsv", exists);
    CHECK(allocated.has_value());
    const auto& filenames = *allocated;
    CHECK(filenames[0] == "runtime-trace-20260825-071000-2.tsv");
    CHECK(filenames[1] == "concrete-submission-trace-20260825-071000-2.tsv");
    CHECK(filenames[2] == "lifecycle-ambiguities-20260825-071000-2.tsv");
  }

  return 0;
}

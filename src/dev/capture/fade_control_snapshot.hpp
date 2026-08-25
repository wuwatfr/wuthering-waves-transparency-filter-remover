// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dev/capture/fade_control_state.hpp"

namespace wuwa_tfr::dev {

constexpr std::uint32_t kFadeControlSnapshotVectorRadius = 16;
constexpr std::size_t kMaxFadeControlSnapshotBytes =
    (2 * kFadeControlSnapshotVectorRadius + 1) * 16;
constexpr std::size_t kMaxFadeControlSnapshots = 512;

struct FadeControlSnapshotWindow {
  bool valid = false;
  std::uint64_t start = 0;
  std::uint64_t size = 0;
};

FadeControlSnapshotWindow ResolveFadeControlSnapshotWindow(
    std::uint64_t cbv_offset, std::uint32_t predicate_vector,
    std::uint32_t radius_vectors, std::uint64_t mapped_start,
    std::uint64_t mapped_end) noexcept;

struct FadeControlSnapshotRecord {
  wuwa_tfr::ExecutionPipelineIdentity pipeline;
  std::uint64_t cbv_offset = 0;
  std::uint64_t mapped_range_offset = 0;
  std::uint64_t mapped_range_size = 0;
  std::uint64_t window_start = 0;
  std::uint64_t window_size = 0;
  std::array<std::byte, kMaxFadeControlSnapshotBytes> raw_bytes{};
};

struct FadeControlSnapshotSet {
  std::uint64_t session_id = 0;
  bool capacity_exceeded = false;
  std::vector<std::pair<FadeControlRecordKey, FadeControlSnapshotRecord>>
      snapshots;
};

class FadeControlSnapshotAccumulator {
 public:
  void Start(std::uint64_t session_id);
  bool ShouldCapture(const FadeControlRecordKey& key) const noexcept;
  void Commit(const FadeControlRecordKey& key,
      const FadeControlSnapshotRecord& record);
  FadeControlSnapshotSet Stop();

  bool active() const noexcept { return active_; }
  const FadeControlSnapshotSet& active_snapshot() const noexcept {
    return active_set_;
  }
  const FadeControlSnapshotSet& last_result() const noexcept {
    return last_result_;
  }

 private:
  bool active_ = false;
  FadeControlSnapshotSet active_set_;
  FadeControlSnapshotSet last_result_;
  std::unordered_map<FadeControlRecordKey, std::size_t, FadeControlRecordKeyHash>
      index_;
};

struct PendingFadeControlObservation {
  FadeControlRecordKey key;
  FadeControlValueSample sample;
};

struct PendingFadeControlSnapshot {
  FadeControlRecordKey key;
  FadeControlSnapshotRecord record;
};

void CommitPendingFadeControlObservations(FadeControlAccumulator& accumulator,
    FadeControlSnapshotAccumulator& snapshot_accumulator,
    FadeControlTrackerCapacityAccumulator& tracker_capacity,
    const wuwa_tfr::ExecutionPipelineIdentity& pipeline,
    const std::vector<PendingFadeControlObservation>& observations,
    const std::vector<PendingFadeControlSnapshot>& snapshots,
    const FadeControlTrackerCapacityDiagnostics& tracker_provenance);

}

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_snapshot.hpp"

#include <algorithm>

namespace wuwa_tfr::dev {

FadeControlSnapshotWindow ResolveFadeControlSnapshotWindow(
    std::uint64_t cbv_offset, std::uint32_t predicate_vector,
    std::uint32_t radius_vectors, std::uint64_t mapped_start,
    std::uint64_t mapped_end) noexcept {
  if (mapped_end <= mapped_start) return {};

  // Clamp the low end in vector units first, so an unsigned underflow can
  // never occur regardless of how small predicate_vector is.
  const std::uint32_t window_start_vector =
      predicate_vector > radius_vectors ? predicate_vector - radius_vectors
                                         : 0;
  // +1 makes this an exclusive end covering the full [predicate_vector +
  // radius_vectors] row.
  const std::uint64_t window_end_vector =
      static_cast<std::uint64_t>(predicate_vector) + radius_vectors + 1;

  const std::uint64_t desired_start =
      cbv_offset + static_cast<std::uint64_t>(window_start_vector) * 16;
  const std::uint64_t desired_end = cbv_offset + window_end_vector * 16;

  const std::uint64_t effective_start = std::max(desired_start, mapped_start);
  const std::uint64_t effective_end = std::min(desired_end, mapped_end);
  if (effective_end <= effective_start) return {};

  std::uint64_t size = effective_end - effective_start;
  // Defensive regardless of the radius_vectors argument actually passed --
  // the caller's fixed-size raw_bytes storage must never be overrun.
  if (size > kMaxFadeControlSnapshotBytes) size = kMaxFadeControlSnapshotBytes;

  FadeControlSnapshotWindow window;
  window.valid = true;
  window.start = effective_start;
  window.size = size;
  return window;
}

void FadeControlSnapshotAccumulator::Start(std::uint64_t session_id) {
  active_set_ = FadeControlSnapshotSet{};
  active_set_.session_id = session_id;
  index_.clear();
  last_result_ = FadeControlSnapshotSet{};
  active_ = true;
}

bool FadeControlSnapshotAccumulator::ShouldCapture(
    const FadeControlRecordKey& key) const noexcept {
  return active_ && !index_.contains(key);
}

void FadeControlSnapshotAccumulator::Commit(
    const FadeControlRecordKey& key, const FadeControlSnapshotRecord& record) {
  if (!active_ || index_.contains(key)) return;
  if (active_set_.snapshots.size() >= kMaxFadeControlSnapshots) {
    active_set_.capacity_exceeded = true;
    return;
  }
  const auto position = active_set_.snapshots.size();
  active_set_.snapshots.push_back({key, record});
  index_.emplace(key, position);
}

FadeControlSnapshotSet FadeControlSnapshotAccumulator::Stop() {
  if (!active_) return {};
  active_ = false;
  last_result_ = active_set_;
  active_set_ = FadeControlSnapshotSet{};
  index_.clear();
  return last_result_;
}

}  // namespace wuwa_tfr::dev

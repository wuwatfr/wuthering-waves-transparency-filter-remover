// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, ReShade-independent pieces of the Fade predicate CBV snapshot
// capture: a bounded byte window around a resolved predicate's cbuffer
// vector, captured once per unique (route, primitive, source, binding) so
// two capture sessions (e.g. camera-proximity fade vs. scripted/animation
// invisibility) can be diffed byte-for-byte instead of only compared via
// the single 32-bit predicate value already sampled by
// dev/capture/fade_control_state.hpp's FadeControlAccumulator (unchanged by
// this module -- this is a strictly additive capability layered on top of
// it, triggered only after that existing single-value sample already
// succeeded; see dev/capture/fade_control_runtime.cpp's ObserveMappedCbvValue
// for the exact trigger point).
//
// Every captured byte here is, like the rest of this tracer, a CPU
// command-recording-time observation of already-mapped constant-buffer
// memory -- never GPU completion evidence.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dev/capture/fade_control_state.hpp"

namespace wuwa_tfr::dev {

// Radius, in 16-byte cbuffer vectors (rows), captured on either side of the
// predicate's own vector. The default window per the task spec: vectors
// [predicate_vector - 16, predicate_vector + 16], inclusive both ends.
constexpr std::uint32_t kFadeControlSnapshotVectorRadius = 16;

// Exact capacity for the widest possible window at the radius above:
// (2 * radius + 1) vectors of 16 bytes each = 33 * 16 = 528 bytes. Enforced
// defensively by ResolveFadeControlSnapshotWindow regardless of the radius
// argument actually passed, so FadeControlSnapshotRecord::raw_bytes can
// never be overrun.
constexpr std::size_t kMaxFadeControlSnapshotBytes =
    (2 * kFadeControlSnapshotVectorRadius + 1) * 16;

// Bounded total distinct snapshots per session -- dedup already keeps this
// small in practice (one entry per unique resolved predicate binding), but
// an explicit cap with a capacity_exceeded flag is kept for the same
// fail-safe reason every other bounded collection in this tracer has one.
constexpr std::size_t kMaxFadeControlSnapshots = 512;

// A resolved, memory-safe absolute byte window, or an explicitly invalid
// one (never a window that would read outside [mapped_start, mapped_end)).
struct FadeControlSnapshotWindow {
  bool valid = false;
  std::uint64_t start = 0;  // absolute byte offset into the resource
  std::uint64_t size = 0;   // <= kMaxFadeControlSnapshotBytes, always > 0 when valid
};

// Computes the clamped, memory-safe absolute byte window centered on
// `predicate_vector` (a cbuffer row index, relative to `cbv_offset` as
// vector 0) with `radius_vectors` vectors captured on either side. The
// window is intersected with [mapped_start, mapped_end) -- the CPU-mapped
// region's real, verified extent, which this tracer treats as "the actual
// CBV range" for clamping purposes (a root-pushed CBV carries no reliable
// declared size of its own; see fade_control_runtime.cpp's OnMapFadeControl
// Buffer). Returns an invalid window (never a partially-safe one) when the
// intersection is empty.
FadeControlSnapshotWindow ResolveFadeControlSnapshotWindow(
    std::uint64_t cbv_offset, std::uint32_t predicate_vector,
    std::uint32_t radius_vectors, std::uint64_t mapped_start,
    std::uint64_t mapped_end) noexcept;

// One captured snapshot: the raw bytes plus enough context (cbv_offset,
// window bounds, pipeline identity) to interpret and align it against a
// second capture. Fixed-size storage -- no heap allocation on the Draw
// path.
struct FadeControlSnapshotRecord {
  FadeControlPipelineIdentity pipeline;
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

// Not internally synchronized -- the caller (dev/capture/fade_control_runtime.cpp)
// serializes access under its existing g_fade_control_mutex, alongside
// FadeControlAccumulator.
class FadeControlSnapshotAccumulator {
 public:
  void Start(std::uint64_t session_id);

  // Pure dedup check: true only when a session is active AND `key` has not
  // already been captured. Does not touch the capacity cap -- a caller that
  // gets true here still may have its Commit() declined if the set is
  // already full (see Commit()'s own comment for why this ordering is
  // fine).
  bool ShouldCapture(const FadeControlRecordKey& key) const noexcept;

  // Stores `record` under `key`, provided a session is active, `key` has
  // not already been captured (first capture wins, never overwritten), and
  // capacity remains. Silently declines (setting capacity_exceeded when
  // that was the reason) rather than growing unbounded or crashing.
  void Commit(const FadeControlRecordKey& key,
      const FadeControlSnapshotRecord& record);

  // Freezes the active session into last_result() and returns a copy.
  // No-op (returns a default, empty set) outside an active session.
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

}  // namespace wuwa_tfr::dev

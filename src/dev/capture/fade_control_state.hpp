// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, ReShade-independent pieces of the targeted Fade control-value
// tracer: the byte-offset formula, the mapped-region bounds check, and the
// bounded per-route aggregation state. See dev/capture/fade_control_runtime.*
// for the ReShade-facing pipeline-layout/mapped-buffer tracking, the
// Draw-time sampling hook, and TSV export that drive this state.
//
// Every value here is a CPU command-recording-time observation of mapped
// constant-buffer memory (see fade_control_runtime.hpp's module comment for
// the full disclosure) -- never GPU completion evidence, never proof of the
// value the GPU ultimately consumed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dev/capture/fade_control_analysis.hpp"
#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

enum class FadeControlRole : std::uint8_t {
  Predicate,
  Coverage,
};

// Bit flags: a single value observation can only be unavailable for one
// reason, but a record's *aggregated* mask can carry several across its
// lifetime (e.g. mapped early, unmapped later in the same session).
constexpr std::uint8_t kFadeControlReasonNotMapped = 1u << 0;
constexpr std::uint8_t kFadeControlReasonOutOfRange = 1u << 1;
constexpr std::uint8_t kFadeControlReasonBindingUnresolved = 1u << 2;
constexpr std::uint8_t kFadeControlReasonSourceUnresolved = 1u << 3;
constexpr std::uint8_t kFadeControlReasonUnsupportedBindingRoute = 1u << 4;
// The descriptor-table-backed route was statically resolved, but the
// bind_descriptor_tables call establishing it carried a nonzero
// dynamic_offset_count -- see descriptor_table_state.hpp's
// DescriptorTableBindingHasExactDynamicOffsets. Never observed on the
// D3D12 backend today; kept explicit rather than assumed.
constexpr std::uint8_t kFadeControlReasonDynamicOffsetUnresolved = 1u << 5;
// The descriptor-table slot's cached resource incarnation no longer
// matches the resource's current one: it was written once (typically at
// resource-allocation time) and the underlying resource has since been
// destroyed and its handle reused, without an intervening
// update_descriptor_tables/copy_descriptor_tables call re-establishing this
// slot. Rejected rather than trusted -- see
// descriptor_table_state.hpp's DescriptorSlotContentIsCurrent.
constexpr std::uint8_t kFadeControlReasonStaleDescriptorBinding = 1u << 6;

// Which binding mechanism resolved this record's live CBV, independent of
// whether the byte value itself was ultimately readable. Distinct from
// "unavailable_reason": a record can be RootPushDescriptors or
// DescriptorTable and still report not_mapped/out_of_range for a given
// observation.
enum class FadeControlBindingRoute : std::uint8_t {
  Unresolved,
  RootPushDescriptors,
  DescriptorTable,
};

constexpr std::size_t kMaxFadeControlDistinctValues = 32;
constexpr std::size_t kMaxFadeControlRecords = 4096;

// range_offset + vector_index * 16 + component * 4: a cbufferLoadLegacy row
// is a 16-byte-aligned vector of four 4-byte lanes, and range_offset is the
// live CBV binding's byte offset into the backing resource.
constexpr std::uint64_t ResolveFadeControlByteOffset(std::uint64_t range_offset,
    std::uint32_t vector_index, std::uint32_t component) noexcept {
  return range_offset + static_cast<std::uint64_t>(vector_index) * 16 +
      static_cast<std::uint64_t>(component) * 4;
}

// True only when the whole 4-byte value at `byte_offset` lies entirely
// inside [mapped_offset, mapped_offset + mapped_size). Pure so the bounds
// arithmetic (including overflow/underflow at the edges) is unit-testable
// without a real mapped pointer.
constexpr bool FadeControlByteOffsetInMappedRegion(std::uint64_t byte_offset,
    std::uint64_t mapped_offset, std::uint64_t mapped_size) noexcept {
  if (byte_offset < mapped_offset) return false;
  const std::uint64_t relative = byte_offset - mapped_offset;
  return relative <= mapped_size && mapped_size - relative >= 4;
}

// One Draw-time observation attempt, already resolved to either a raw
// 32-bit value or a reason it could not be read. Never a silently
// substituted zero.
struct FadeControlValueSample {
  bool available = false;
  std::uint32_t raw_bits = 0;
  std::uint8_t unavailable_reason = 0;
};

struct FadeControlDistinctValue {
  std::uint32_t raw_bits = 0;
  std::uint64_t count = 0;
};

// Bounded (kMaxFadeControlDistinctValues) per-value-source statistics.
// Deliberately not a std::vector/std::unordered_set of unbounded size: the
// distinct-value table is a fixed array with an explicit overflow flag.
struct FadeControlValueStats {
  std::uint64_t draw_observations = 0;
  std::uint64_t available_observations = 0;
  std::uint64_t unavailable_observations = 0;
  bool has_available = false;
  std::uint32_t first_bits = 0;
  std::uint32_t last_bits = 0;
  bool changed = false;
  bool has_finite = false;
  float finite_min = 0.0f;
  float finite_max = 0.0f;
  std::array<FadeControlDistinctValue, kMaxFadeControlDistinctValues>
      distinct_values{};
  std::size_t distinct_value_count = 0;
  bool distinct_overflow = false;
  std::uint8_t unavailable_reason_mask = 0;

  void Observe(const FadeControlValueSample& sample);
};

// Identity for one accumulated control-value record: the stable execution
// route (same identity the manual capture and concrete trace already use),
// the primitive within its shader, which control role, the proven static
// source, and the resolved runtime CBV binding. Two Draws that differ in
// any of these are never merged -- see fade_control_state.cpp's tests for
// why each field is load-bearing.
struct FadeControlRecordKey {
  wuwa_tfr::TraceConcreteDrawKey route;
  std::uint32_t primitive_index = 0;
  FadeControlRole role = FadeControlRole::Predicate;
  std::uint32_t cbuffer_space = 0;
  std::uint32_t cbuffer_register = 0;
  std::uint32_t vector_index = 0;
  std::uint32_t component = 0;
  std::uint64_t runtime_resource_incarnation = 0;
  std::uint64_t runtime_range_offset = 0;
  // Which binding mechanism produced runtime_resource_incarnation above --
  // load-bearing for identity because RootPushDescriptors and
  // DescriptorTable resolutions are resolved through two independent
  // incarnation counters (dev/trace/trace_state.hpp's resource-incarnation
  // index for the former, fade_control_runtime.cpp's own for the latter),
  // whose numeric values are not mutually comparable and could otherwise
  // coincidentally collide.
  FadeControlBindingRoute binding_route = FadeControlBindingRoute::Unresolved;

  friend bool operator==(
      const FadeControlRecordKey&, const FadeControlRecordKey&) = default;
};

struct FadeControlRecordKeyHash {
  std::size_t operator()(const FadeControlRecordKey& key) const noexcept {
    std::size_t hash = wuwa_tfr::TraceConcreteDrawKeyHash{}(key.route);
    wuwa_tfr::TraceHashCombine(hash, key.primitive_index);
    wuwa_tfr::TraceHashCombine(hash, static_cast<std::uint64_t>(key.role));
    wuwa_tfr::TraceHashCombine(hash, key.cbuffer_space);
    wuwa_tfr::TraceHashCombine(hash, key.cbuffer_register);
    wuwa_tfr::TraceHashCombine(hash, key.vector_index);
    wuwa_tfr::TraceHashCombine(hash, key.component);
    wuwa_tfr::TraceHashCombine(hash, key.runtime_resource_incarnation);
    wuwa_tfr::TraceHashCombine(hash, key.runtime_range_offset);
    wuwa_tfr::TraceHashCombine(
        hash, static_cast<std::uint64_t>(key.binding_route));
    return hash;
  }
};

// Pipeline/shader identity copied verbatim at first-observation time, for
// export only -- never recomputed here. Mirrors
// dev/capture/manual_capture_state.hpp's ManualCapturePipelineInfo split.
struct FadeControlPipelineIdentity {
  std::uint64_t device = 0;
  std::uint64_t application_pso = 0;
  std::uint64_t pso_incarnation = 0;
  std::uint64_t pso_context_hash = 0;
  std::uint64_t pixel_shader_hash = 0;
};

struct FadeControlRecord {
  FadeControlPipelineIdentity pipeline;
  FadeControlValueStats stats;
};

struct FadeControlSnapshot {
  std::uint64_t session_id = 0;
  bool capacity_exceeded = false;
  std::vector<std::pair<FadeControlRecordKey, FadeControlRecord>> records;
};

// Not internally synchronized -- the caller (dev/capture/fade_control_runtime.cpp)
// serializes access under its own dedicated mutex, deliberately independent
// of g_trace_mutex/g_inspection_mutex (see that file's module comment for
// the lock-ordering rationale).
class FadeControlAccumulator {
 public:
  void Start(std::uint64_t session_id);

  // No-op outside an active session. Folds one Draw-time observation into
  // the bounded aggregate for `key`.
  void Observe(const FadeControlRecordKey& key,
      const FadeControlPipelineIdentity& pipeline,
      const FadeControlValueSample& sample);

  // Freezes the active session into last_result() and returns a copy.
  // No-op (returns a default, empty snapshot) outside an active session.
  FadeControlSnapshot Stop();

  bool active() const noexcept { return active_; }
  const FadeControlSnapshot& active_snapshot() const noexcept {
    return active_snapshot_;
  }
  const FadeControlSnapshot& last_result() const noexcept {
    return last_result_;
  }

 private:
  bool active_ = false;
  FadeControlSnapshot active_snapshot_;
  FadeControlSnapshot last_result_;
  std::unordered_map<FadeControlRecordKey, std::size_t, FadeControlRecordKeyHash>
      index_;
};

}  // namespace wuwa_tfr::dev

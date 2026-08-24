// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

enum class FadeControlRole : std::uint8_t {
  Predicate,
  PreFadeOperandOne,
  PreFadeOperandTwo,
};

constexpr std::uint16_t kFadeControlReasonNotMapped = 1u << 0;
constexpr std::uint16_t kFadeControlReasonOutOfRange = 1u << 1;
constexpr std::uint16_t kFadeControlReasonBindingUnresolved = 1u << 2;
constexpr std::uint16_t kFadeControlReasonSourceUnresolved = 1u << 3;
constexpr std::uint16_t kFadeControlReasonUnsupportedBindingRoute = 1u << 4;
constexpr std::uint16_t kFadeControlReasonDynamicOffsetUnresolved = 1u << 5;
constexpr std::uint16_t kFadeControlReasonStaleDescriptorBinding = 1u << 6;
constexpr std::uint16_t kFadeControlReasonDescriptorUnknown = 1u << 7;
constexpr std::uint16_t kFadeControlReasonPushConstantBacked = 1u << 8;

enum class FadeControlBindingRoute : std::uint8_t {
  Unresolved,
  RootPushDescriptors,
  DescriptorTable,
};

constexpr std::size_t kMaxFadeControlDistinctValues = 32;
constexpr std::size_t kMaxFadeControlRecords = 4096;

constexpr std::uint64_t ResolveFadeControlByteOffset(std::uint64_t range_offset,
    std::uint32_t vector_index, std::uint32_t component) noexcept {
  return range_offset + static_cast<std::uint64_t>(vector_index) * 16 +
      static_cast<std::uint64_t>(component) * 4;
}

constexpr bool FadeControlByteOffsetInMappedRegion(std::uint64_t byte_offset,
    std::uint64_t mapped_offset, std::uint64_t mapped_size) noexcept {
  if (byte_offset < mapped_offset) return false;
  const std::uint64_t relative = byte_offset - mapped_offset;
  return relative <= mapped_size && mapped_size - relative >= 4;
}

struct FadeControlValueSample {
  bool available = false;
  std::uint32_t raw_bits = 0;
  std::uint16_t unavailable_reason = 0;
};

struct FadeControlDistinctValue {
  std::uint32_t raw_bits = 0;
  std::uint64_t count = 0;
};

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
  std::uint16_t unavailable_reason_mask = 0;

  void Observe(const FadeControlValueSample& sample);
};

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

class FadeControlAccumulator {
 public:
  void Start(std::uint64_t session_id);

  void Observe(const FadeControlRecordKey& key,
      const FadeControlPipelineIdentity& pipeline,
      const FadeControlValueSample& sample);

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

}

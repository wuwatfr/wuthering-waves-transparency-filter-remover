// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_state.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wuwa_tfr::dev {

void FadeControlValueStats::Observe(const FadeControlValueSample& sample) {
  ++draw_observations;
  if (!sample.available) {
    ++unavailable_observations;
    unavailable_reason_mask |= sample.unavailable_reason;
    return;
  }
  ++available_observations;
  if (!has_available) {
    first_bits = sample.raw_bits;
    last_bits = sample.raw_bits;
    changed = false;
    has_available = true;
  } else {
    if (last_bits != sample.raw_bits) changed = true;
    last_bits = sample.raw_bits;
  }

  float value = 0.0f;
  std::memcpy(&value, &sample.raw_bits, sizeof(value));
  if (std::isfinite(value)) {
    if (!has_finite) {
      finite_min = value;
      finite_max = value;
      has_finite = true;
    } else {
      finite_min = std::min(finite_min, value);
      finite_max = std::max(finite_max, value);
    }
  }

  for (std::size_t i = 0; i < distinct_value_count; ++i) {
    if (distinct_values[i].raw_bits == sample.raw_bits) {
      ++distinct_values[i].count;
      return;
    }
  }
  if (distinct_value_count < distinct_values.size()) {
    distinct_values[distinct_value_count++] = {sample.raw_bits, 1};
  } else {
    distinct_overflow = true;
  }
}

void FadeControlAccumulator::Start(std::uint64_t session_id) {
  active_snapshot_ = FadeControlSnapshot{};
  active_snapshot_.session_id = session_id;
  index_.clear();
  last_result_ = FadeControlSnapshot{};
  active_ = true;
}

void FadeControlAccumulator::Observe(const FadeControlRecordKey& key,
    const wuwa_tfr::ExecutionPipelineIdentity& pipeline,
    const FadeControlValueSample& sample) {
  if (!active_) return;

  std::size_t position;
  if (const auto existing = index_.find(key); existing != index_.end()) {
    position = existing->second;
  } else {
    if (active_snapshot_.records.size() >= kMaxFadeControlRecords) {
      active_snapshot_.capacity_exceeded = true;
      return;
    }
    position = active_snapshot_.records.size();
    active_snapshot_.records.push_back({key, FadeControlRecord{}});
    index_.emplace(key, position);
    active_snapshot_.records[position].second.pipeline = pipeline;
  }
  active_snapshot_.records[position].second.stats.Observe(sample);
}

FadeControlSnapshot FadeControlAccumulator::Stop() {
  if (!active_) return {};
  active_ = false;
  last_result_ = active_snapshot_;
  active_snapshot_ = FadeControlSnapshot{};
  index_.clear();
  return last_result_;
}

}

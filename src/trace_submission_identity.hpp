// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wuwa_tfr {

inline void TraceHashCombine(std::size_t& hash, std::uint64_t value) noexcept {
  const std::size_t next = std::hash<std::uint64_t>{}(value);
  hash ^= next + static_cast<std::size_t>(0x9E3779B9u) +
      (hash << 6) + (hash >> 2);
}

struct TraceLiveHandleKey {
  std::uintptr_t owner = 0;
  std::uint64_t handle = 0;

  friend bool operator==(const TraceLiveHandleKey&, const TraceLiveHandleKey&) =
      default;
};

struct TraceLiveHandleKeyHash {
  std::size_t operator()(const TraceLiveHandleKey& key) const noexcept {
    std::size_t hash = std::hash<std::uintptr_t>{}(key.owner);
    TraceHashCombine(hash, key.handle);
    return hash;
  }
};

template <typename Identity>
class TraceIncarnationIndex {
 public:
  struct Record {
    std::uint64_t id = 0;
    TraceLiveHandleKey live_key;
    Identity identity;
    bool live = false;
  };

  struct Activation {
    std::uint64_t id = 0;
    bool duplicate = false;
    bool rotated_without_destroy = false;
    std::optional<Identity> previous_identity;
  };

  Activation Activate(TraceLiveHandleKey key, Identity identity) {
    bool rotated = false;
    std::optional<Identity> previous_identity;
    if (const auto active = active_.find(key); active != active_.end()) {
      const auto& record = records_.at(active->second);
      if (record.identity == identity)
        return {record.id, true, false, std::nullopt};
      previous_identity = record.identity;
      EraseRecord(active->second);
      active_.erase(active);
      rotated = true;
    }

    Record record;
    record.id = next_id_++;
    record.live_key = key;
    record.identity = std::move(identity);
    record.live = true;
    const std::uint64_t id = record.id;
    records_.emplace(id, std::move(record));
    active_.emplace(key, id);
    activation_order_.push_back(id);
    order_positions_.emplace(id, std::prev(activation_order_.end()));
    return {id, false, rotated, std::move(previous_identity)};
  }

  const Record* FindActive(TraceLiveHandleKey key) const {
    const auto active = active_.find(key);
    return active == active_.end() ? nullptr : Find(active->second);
  }

  const Record* Find(std::uint64_t id) const {
    const auto record = records_.find(id);
    return record == records_.end() ? nullptr : &record->second;
  }

  std::optional<std::uint64_t> Destroy(TraceLiveHandleKey key) {
    const auto active = active_.find(key);
    if (active == active_.end()) return std::nullopt;
    const std::uint64_t id = active->second;
    EraseRecord(id);
    active_.erase(active);
    return id;
  }

  template <typename Predicate>
  void DestroyWhere(Predicate&& predicate) {
    for (auto it = active_.begin(); it != active_.end();) {
      if (!std::forward<Predicate>(predicate)(it->first)) {
        ++it;
        continue;
      }
      EraseRecord(it->second);
      it = active_.erase(it);
    }
  }

  std::size_t PruneTo(std::size_t maximum_records) {
    std::size_t pruned = 0;
    while (records_.size() > maximum_records && !activation_order_.empty()) {
      const std::uint64_t id = activation_order_.front();
      const auto record = records_.find(id);
      if (record == records_.end()) {
        activation_order_.pop_front();
        order_positions_.erase(id);
        continue;
      }
      const auto active = active_.find(record->second.live_key);
      if (active != active_.end() && active->second == id)
        active_.erase(active);
      EraseRecord(id);
      ++pruned;
    }
    return pruned;
  }

  std::size_t size() const noexcept { return records_.size(); }

 private:
  void EraseRecord(std::uint64_t id) {
    if (const auto position = order_positions_.find(id);
        position != order_positions_.end()) {
      activation_order_.erase(position->second);
      order_positions_.erase(position);
    }
    records_.erase(id);
  }

  std::uint64_t next_id_ = 1;
  std::unordered_map<TraceLiveHandleKey, std::uint64_t,
      TraceLiveHandleKeyHash> active_;
  std::unordered_map<std::uint64_t, Record> records_;
  std::list<std::uint64_t> activation_order_;
  std::unordered_map<std::uint64_t, typename std::list<std::uint64_t>::iterator>
      order_positions_;
};

struct TraceVersionedSlotUpdate {
  std::uint64_t version = 0;
  bool duplicate = false;
  bool replaced = false;
};

template <typename Identity>
TraceVersionedSlotUpdate UpdateTraceVersionedSlot(
    TraceIncarnationIndex<Identity>& index, TraceLiveHandleKey key,
    const Identity& identity) {
  if (const auto* current = index.FindActive(key)) {
    if (current->identity == identity)
      return {current->id, true, false};
    index.Destroy(key);
    const auto replacement = index.Activate(key, identity);
    return {replacement.id, false, true};
  }
  const auto created = index.Activate(key, identity);
  return {created.id, false, false};
}

template <typename Identity, std::size_t SampleLimit = 32>
class TraceLifecycleAmbiguityDiagnostics {
 public:
  struct Sample {
    TraceLiveHandleKey key;
    Identity previous_identity;
    Identity new_identity;
    std::uint64_t frame = 0;
    std::uint64_t event_serial = 0;
    std::uint64_t handle_event_count = 0;
  };

  struct Snapshot {
    std::uint64_t total_events = 0;
    std::uint64_t unique_handles = 0;
    std::uint64_t max_events_for_one_handle = 0;
    std::vector<Sample> samples;
  };

  void Record(TraceLiveHandleKey key, const Identity& previous_identity,
      const Identity& new_identity, std::uint64_t frame,
      std::uint64_t event_serial) {
    ++total_events_;
    auto [count, inserted] = handle_counts_.try_emplace(key, 0);
    ++count->second;
    max_events_for_one_handle_ =
        std::max(max_events_for_one_handle_, count->second);
    if (inserted && samples_.size() < SampleLimit) {
      samples_.push_back({key, previous_identity, new_identity, frame,
          event_serial, 1});
    }
  }

  Snapshot GetSnapshot() const {
    Snapshot snapshot;
    snapshot.total_events = total_events_;
    snapshot.unique_handles = handle_counts_.size();
    snapshot.max_events_for_one_handle = max_events_for_one_handle_;
    snapshot.samples = samples_;
    for (auto& sample : snapshot.samples)
      sample.handle_event_count = handle_counts_.at(sample.key);
    return snapshot;
  }

  void Reset() {
    total_events_ = 0;
    max_events_for_one_handle_ = 0;
    handle_counts_.clear();
    samples_.clear();
  }

 private:
  std::uint64_t total_events_ = 0;
  std::uint64_t max_events_for_one_handle_ = 0;
  std::unordered_map<TraceLiveHandleKey, std::uint64_t,
      TraceLiveHandleKeyHash> handle_counts_;
  std::vector<Sample> samples_;
};

enum class TraceDrawKind : std::uint8_t {
  Direct,
  Indexed,
  Mesh,
  IndirectDraw,
  IndirectIndexed,
  IndirectMesh,
};

enum TraceGeometryObservation : std::uint32_t {
  TraceObservedPso = 1u << 0,
  TraceObservedVertexBuffers = 1u << 1,
  TraceObservedIndexBuffer = 1u << 2,
  TraceObservedTopology = 1u << 3,
  TraceObservedPass = 1u << 4,
  TraceIndirectArgumentsUnknown = 1u << 5,
  TracePassInheritedAtSecondaryExecute = 1u << 6,
};

struct TraceVertexBinding {
  std::uint32_t slot = 0;
  std::uint64_t resource_incarnation = 0;
  std::uint64_t offset = 0;
  std::uint32_t stride = 0;
  bool dynamic_contents = false;

  friend bool operator==(const TraceVertexBinding&, const TraceVertexBinding&) =
      default;
};

struct TraceIndexBinding {
  std::uint64_t resource_incarnation = 0;
  std::uint64_t offset = 0;
  std::uint32_t index_size = 0;
  bool dynamic_contents = false;

  friend bool operator==(const TraceIndexBinding&, const TraceIndexBinding&) =
      default;
};

struct TraceGeometryKey {
  TraceDrawKind kind = TraceDrawKind::Direct;
  std::array<std::uint64_t, 5> arguments{};
  std::uint32_t topology = 0;
  std::vector<TraceVertexBinding> vertex_buffers;
  std::optional<TraceIndexBinding> index_buffer;
  std::uint64_t indirect_resource_incarnation = 0;
  std::uint64_t indirect_offset = 0;
  std::uint32_t indirect_declared_count = 0;
  std::uint32_t indirect_stride = 0;
  std::uint32_t observations = 0;

  friend bool operator==(const TraceGeometryKey&, const TraceGeometryKey&) =
      default;
};

struct TraceGeometryKeyHash {
  std::size_t operator()(const TraceGeometryKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint32_t>{}(
        static_cast<std::uint32_t>(key.kind));
    for (const auto value : key.arguments) TraceHashCombine(hash, value);
    TraceHashCombine(hash, key.topology);
    for (const auto& binding : key.vertex_buffers) {
      TraceHashCombine(hash, binding.slot);
      TraceHashCombine(hash, binding.resource_incarnation);
      TraceHashCombine(hash, binding.offset);
      TraceHashCombine(hash, binding.stride);
      TraceHashCombine(hash, binding.dynamic_contents);
    }
    TraceHashCombine(hash, key.index_buffer.has_value());
    if (key.index_buffer) {
      TraceHashCombine(hash, key.index_buffer->resource_incarnation);
      TraceHashCombine(hash, key.index_buffer->offset);
      TraceHashCombine(hash, key.index_buffer->index_size);
      TraceHashCombine(hash, key.index_buffer->dynamic_contents);
    }
    TraceHashCombine(hash, key.indirect_resource_incarnation);
    TraceHashCombine(hash, key.indirect_offset);
    TraceHashCombine(hash, key.indirect_declared_count);
    TraceHashCombine(hash, key.indirect_stride);
    TraceHashCombine(hash, key.observations);
    return hash;
  }
};

struct TraceConcreteDrawKey {
  std::uint64_t pso_incarnation = 0;
  TraceGeometryKey geometry;
  std::uint64_t pass_fingerprint = 0;

  friend bool operator==(
      const TraceConcreteDrawKey&,
      const TraceConcreteDrawKey&) = default;
};

struct TraceConcreteDrawKeyHash {
  std::size_t operator()(const TraceConcreteDrawKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.pso_incarnation);
    TraceHashCombine(hash, TraceGeometryKeyHash{}(key.geometry));
    TraceHashCombine(hash, key.pass_fingerprint);
    return hash;
  }
};

struct TraceDrawRouteKey {
  TraceGeometryKey geometry;
  std::uint64_t pass_fingerprint = 0;

  friend bool operator==(const TraceDrawRouteKey&, const TraceDrawRouteKey&) =
      default;
};

struct TraceDrawRouteKeyHash {
  std::size_t operator()(const TraceDrawRouteKey& key) const noexcept {
    std::size_t hash = TraceGeometryKeyHash{}(key.geometry);
    TraceHashCombine(hash, key.pass_fingerprint);
    return hash;
  }
};

inline TraceDrawRouteKey MakeTraceDrawRoute(
    const TraceConcreteDrawKey& key) {
  return {key.geometry, key.pass_fingerprint};
}

enum class TraceRouteConclusion : std::uint8_t {
  Unknown,
  SubmittedInFullWindow,
  NotObservedInFullWindow,
};

inline TraceRouteConclusion ClassifyTraceRoute(
    bool has_positive_control, bool submitted_in_full_window,
    bool exact_pass_observed, bool dynamic_contents_heuristic,
    bool capacity_loss) noexcept {
  if (!has_positive_control || !exact_pass_observed ||
      dynamic_contents_heuristic || capacity_loss)
    return TraceRouteConclusion::Unknown;
  return submitted_in_full_window
      ? TraceRouteConclusion::SubmittedInFullWindow
      : TraceRouteConclusion::NotObservedInFullWindow;
}

struct TraceSubmissionWindowMetrics {
  std::uint64_t queue_submitted_commands = 0;
  std::uint64_t command_list_submissions = 0;
  std::uint64_t active_frames = 0;
  std::uint64_t last_frame = 0;
};

template <std::size_t WindowCount>
struct TraceSubmissionRecord {
  std::array<TraceSubmissionWindowMetrics, WindowCount> windows;
};

template <std::size_t WindowCount>
void AccumulateTraceSubmission(TraceSubmissionRecord<WindowCount>& record,
    std::optional<std::size_t> window, std::uint64_t commands,
    bool first_for_key_in_command_list, std::uint64_t frame) noexcept {
  if (!window || *window >= WindowCount) return;
  auto& metrics = record.windows[*window];
  metrics.queue_submitted_commands += commands;
  if (!first_for_key_in_command_list) return;
  ++metrics.command_list_submissions;
  if (metrics.last_frame != frame) {
    metrics.last_frame = frame;
    ++metrics.active_frames;
  }
}

inline TraceConcreteDrawKey WithInheritedTracePass(
    TraceConcreteDrawKey key, std::uint64_t pass_fingerprint,
    bool pass_observed) {
  if (!pass_observed ||
      (key.geometry.observations & TraceObservedPass) != 0)
    return key;
  key.pass_fingerprint = pass_fingerprint;
  key.geometry.observations |=
      TraceObservedPass | TracePassInheritedAtSecondaryExecute;
  return key;
}

inline bool TraceGeometryIsConcrete(const TraceGeometryKey& key) noexcept {
  if ((key.observations & TraceObservedPso) == 0 ||
      (key.observations & TraceObservedTopology) == 0 ||
      (key.observations & TraceObservedPass) == 0 ||
      (key.observations & TraceIndirectArgumentsUnknown) != 0)
    return false;
  const bool vertex_resources_known = !key.vertex_buffers.empty() &&
      std::all_of(key.vertex_buffers.begin(), key.vertex_buffers.end(),
          [](const auto& binding) {
            return binding.resource_incarnation != 0;
          });
  if (key.kind == TraceDrawKind::Indexed)
    return (key.observations & TraceObservedIndexBuffer) != 0 &&
        key.index_buffer && key.index_buffer->resource_incarnation != 0 &&
        vertex_resources_known;
  if (key.kind == TraceDrawKind::Direct)
    return (key.observations & TraceObservedVertexBuffers) != 0 &&
        vertex_resources_known;
  return false;
}

inline bool TraceGeometryIsSkipEligible(const TraceGeometryKey& key) noexcept {
  return TraceGeometryIsConcrete(key) &&
      (key.observations & TracePassInheritedAtSecondaryExecute) == 0;
}

inline bool TraceGeometryUsesDynamicContents(
    const TraceGeometryKey& key) noexcept {
  return std::any_of(key.vertex_buffers.begin(), key.vertex_buffers.end(),
             [](const auto& binding) { return binding.dynamic_contents; }) ||
      (key.index_buffer && key.index_buffer->dynamic_contents);
}

enum class TraceInvestigationView : std::uint8_t {
  NormalPartialNoDiscard,
  NormalPartial,
  NormalPartialDiscard,
  NormalPartialStrictSpatialDither,
  NormalOnlyRimShortlist,
  ShowAll,
};

struct TraceCandidateRange {
  std::size_t begin = 0;
  std::size_t end = 0;

  std::size_t size() const noexcept { return end - begin; }

  friend bool operator==(const TraceCandidateRange&, const TraceCandidateRange&) =
      default;
};

inline TraceCandidateRange FullTraceCandidateRange(
    std::size_t population_size) noexcept {
  return {0, population_size};
}

inline TraceCandidateRange FirstTraceCandidateHalf(
    TraceCandidateRange range) noexcept {
  return {range.begin, range.begin + (range.size() + 1) / 2};
}

inline TraceCandidateRange SecondTraceCandidateHalf(
    TraceCandidateRange range) noexcept {
  return {range.begin + (range.size() + 1) / 2, range.end};
}

inline const char* TraceInvestigationViewName(
    TraceInvestigationView view) noexcept {
  switch (view) {
    case TraceInvestigationView::NormalPartialNoDiscard:
      return "normal-partial-no-discard";
    case TraceInvestigationView::NormalPartial:
      return "normal-partial";
    case TraceInvestigationView::NormalPartialDiscard:
      return "normal-partial-discard";
    case TraceInvestigationView::NormalPartialStrictSpatialDither:
      return "normal-partial-strict-spatial-dither";
    case TraceInvestigationView::NormalOnlyRimShortlist:
      return "normal-only-rim-shortlist";
    case TraceInvestigationView::ShowAll:
      return "show-all";
  }
  return "unknown";
}

template <std::size_t WindowCount>
bool TraceFadeTransitionCandidate(
    const std::array<TraceSubmissionWindowMetrics, WindowCount>& windows)
    noexcept {
  if constexpr (WindowCount < 3) {
    return false;
  } else {
    return windows[0].command_list_submissions == 0 &&
        windows[1].command_list_submissions > 0 &&
        windows[2].command_list_submissions > 0;
  }
}

template <std::size_t WindowCount>
bool TraceNormalOnlySubmissionCandidate(
    const std::array<TraceSubmissionWindowMetrics, WindowCount>& windows)
    noexcept {
  if constexpr (WindowCount < 3) {
    return false;
  } else {
    return windows[0].command_list_submissions > 0 &&
        windows[1].command_list_submissions == 0 &&
        windows[2].command_list_submissions == 0;
  }
}

template <std::size_t WindowCount>
bool TracePartialFullEqual(
    const std::array<TraceSubmissionWindowMetrics, WindowCount>& windows)
    noexcept {
  if constexpr (WindowCount < 3) {
    return false;
  } else {
    return windows[1].command_list_submissions ==
        windows[2].command_list_submissions;
  }
}

template <std::size_t WindowCount>
bool TraceInvestigationVisible(const TraceConcreteDrawKey& key,
    const std::array<TraceSubmissionWindowMetrics, WindowCount>& windows,
    TraceInvestigationView view, bool has_discard,
    bool has_strict_spatial_dither) noexcept {
  if (view == TraceInvestigationView::ShowAll) return true;
  if (!TraceGeometryIsConcrete(key.geometry) ||
      (key.geometry.kind != TraceDrawKind::Direct &&
          key.geometry.kind != TraceDrawKind::Indexed))
    return false;
  if constexpr (WindowCount < 2) {
    return false;
  } else {
    if (windows[0].command_list_submissions == 0 ||
        windows[1].command_list_submissions == 0)
      return false;
    if (view == TraceInvestigationView::NormalPartialNoDiscard)
      return !has_discard;
    if (view == TraceInvestigationView::NormalPartialDiscard)
      return has_discard;
    if (view ==
        TraceInvestigationView::NormalPartialStrictSpatialDither)
      return has_strict_spatial_dither;
    return view == TraceInvestigationView::NormalPartial;
  }
}

} // namespace wuwa_tfr

// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wuwa_tfr {

template <typename Owner, typename Pipeline>
class PipelineGenerationState {
 private:
  struct Entry;

 public:
  enum class ReconcileResult { Published, AlreadyPublished, InFlight, Rejected };

  struct Ticket {
    Owner owner{};
    std::uint64_t application_pipeline = 0;
    std::uint64_t generation = 0;
  };

  class BindLease {
   public:
    BindLease() = default;
    BindLease(const BindLease&) = delete;
    BindLease& operator=(const BindLease&) = delete;
    BindLease(BindLease&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)),
          entry_(std::move(other.entry_)), pipeline_(std::move(other.pipeline_)) {}
    BindLease& operator=(BindLease&& other) noexcept {
      if (this == &other) return *this;
      Release();
      state_ = std::exchange(other.state_, nullptr);
      entry_ = std::move(other.entry_);
      pipeline_ = std::move(other.pipeline_);
      return *this;
    }
    ~BindLease() { Release(); }

    explicit operator bool() const noexcept { return state_ != nullptr; }
    const Pipeline& pipeline() const noexcept { return *pipeline_; }

   private:
    friend class PipelineGenerationState;

    BindLease(PipelineGenerationState* state, std::shared_ptr<Entry> entry,
        Pipeline pipeline)
        : state_(state), entry_(std::move(entry)),
          pipeline_(std::move(pipeline)) {}

    void Release() noexcept {
      if (!state_) return;
      state_->ReleaseBind(entry_);
      state_ = nullptr;
      entry_.reset();
      pipeline_.reset();
    }

    PipelineGenerationState* state_ = nullptr;
    std::shared_ptr<Entry> entry_;
    std::optional<Pipeline> pipeline_;
  };

  template <typename Build, typename Destroy>
  ReconcileResult Reconcile(Owner owner, std::uint64_t application_pipeline,
      std::uint64_t observed_shader_identity, Build&& build, Destroy&& destroy) {
    const BeginResult begin = Begin(owner, application_pipeline,
        observed_shader_identity);
    if (begin.kind == BeginKind::AlreadyPublished)
      return ReconcileResult::AlreadyPublished;
    if (begin.kind == BeginKind::InFlight)
      return ReconcileResult::InFlight;
    if (begin.kind != BeginKind::Start)
      return ReconcileResult::Rejected;

    std::optional<Pipeline> candidate = std::forward<Build>(build)();
    if (!candidate) {
      Abandon(begin.ticket);
      return ReconcileResult::Rejected;
    }
    if (Publish(begin.ticket, *candidate))
      return ReconcileResult::Published;
    std::forward<Destroy>(destroy)(*candidate);
    return ReconcileResult::Rejected;
  }

  std::vector<Pipeline> DestroyPipeline(
      Owner owner, std::uint64_t application_pipeline) {
    std::unique_lock lock(mutex_);
    const auto it = entries_.find(Key{owner, application_pipeline});
    if (it == entries_.end()) return {};
    const std::shared_ptr<Entry> entry = it->second;
    entries_.erase(it);
    WaitForNoBinds(lock, entry);
    return TakePipelines(*entry);
  }

  std::vector<Pipeline> DrainOwner(Owner owner) {
    std::unique_lock lock(mutex_);
    std::vector<std::shared_ptr<Entry>> entries;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->first.owner != owner) {
        ++it;
        continue;
      }
      entries.push_back(it->second);
      it = entries_.erase(it);
    }
    std::vector<Pipeline> result;
    for (const auto& entry : entries) {
      WaitForNoBinds(lock, entry);
      auto pipelines = TakePipelines(*entry);
      result.insert(result.end(), std::make_move_iterator(pipelines.begin()),
          std::make_move_iterator(pipelines.end()));
    }
    return result;
  }

  std::optional<BindLease> AcquireSelected(
      Owner owner, std::uint64_t application_pipeline) {
    std::unique_lock lock(mutex_);
    const auto it = entries_.find(Key{owner, application_pipeline});
    if (it == entries_.end() || it->second->selection_disabled ||
        !it->second->selected)
      return std::nullopt;
    ++it->second->bind_count;
    return BindLease(this, it->second, *it->second->selected);
  }

  template <typename Bind>
  bool WithSelected(Owner owner, std::uint64_t application_pipeline,
      Bind&& bind) {
    auto lease = AcquireSelected(owner, application_pipeline);
    if (!lease) return false;
    std::forward<Bind>(bind)(lease->pipeline());
    return true;
  }

  std::size_t Size() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [_, entry] : entries_)
      count += entry->selected.has_value();
    return count;
  }

  std::size_t RetainedSize() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [_, entry] : entries_) {
      count += entry->selected.has_value();
      count += entry->retired.size();
    }
    return count;
  }

 private:
  enum class EntryState { Preparing, Published, Rejected };
  enum class BeginKind { Start, AlreadyPublished, InFlight, Rejected };

  struct Key {
    Owner owner{};
    std::uint64_t application_pipeline = 0;

    friend bool operator==(const Key&, const Key&) = default;
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
      std::size_t hash = std::hash<Owner>{}(key.owner);
      const std::size_t pipeline_hash =
          std::hash<std::uint64_t>{}(key.application_pipeline);
      return hash ^ (pipeline_hash + static_cast<std::size_t>(0x9E3779B9u) +
          (hash << 6) + (hash >> 2));
    }
  };

  struct Entry {
    std::uint64_t generation = 0;
    std::uint64_t observed_shader_identity = 0;
    std::size_t bind_count = 0;
    EntryState state = EntryState::Preparing;
    bool selection_disabled = false;
    std::optional<Pipeline> selected;
    std::vector<Pipeline> retired;
  };

  struct BeginResult {
    BeginKind kind = BeginKind::Rejected;
    Ticket ticket;
  };

  BeginResult Begin(Owner owner, std::uint64_t application_pipeline,
      std::uint64_t observed_shader_identity) {
    std::lock_guard lock(mutex_);
    const Key key{owner, application_pipeline};
    const auto existing = entries_.find(key);
    if (existing == entries_.end()) {
      const std::uint64_t generation = IssueGeneration();
      if (generation == 0) return {};
      auto entry = std::make_shared<Entry>();
      entry->generation = generation;
      entry->observed_shader_identity = observed_shader_identity;
      entries_.emplace(key, std::move(entry));
      return {BeginKind::Start, {owner, application_pipeline, generation}};
    }

    const std::shared_ptr<Entry>& entry = existing->second;
    if (entry->state == EntryState::Preparing && !entry->selection_disabled &&
        entry->observed_shader_identity == observed_shader_identity)
      return {BeginKind::InFlight, {owner, application_pipeline, entry->generation}};
    if (entry->state == EntryState::Published && !entry->selection_disabled &&
        entry->observed_shader_identity == observed_shader_identity)
      return {BeginKind::AlreadyPublished,
          {owner, application_pipeline, entry->generation}};

    entry->selection_disabled = true;
    entry->state = EntryState::Rejected;
    if (entry->selected) {
      entry->retired.push_back(std::move(*entry->selected));
      entry->selected.reset();
    }
    entry->generation = IssueGeneration();
    return {BeginKind::Rejected, {owner, application_pipeline, entry->generation}};
  }

  bool Publish(const Ticket& ticket, Pipeline replacement) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(Key{ticket.owner, ticket.application_pipeline});
    if (it == entries_.end() || ticket.generation == 0 ||
        it->second->generation != ticket.generation ||
        it->second->state != EntryState::Preparing ||
        it->second->selection_disabled)
      return false;
    it->second->selected = std::move(replacement);
    it->second->state = EntryState::Published;
    return true;
  }

  void Abandon(const Ticket& ticket) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(Key{ticket.owner, ticket.application_pipeline});
    if (it != entries_.end() && ticket.generation != 0 &&
        it->second->generation == ticket.generation &&
        it->second->state == EntryState::Preparing &&
        !it->second->selection_disabled)
      entries_.erase(it);
  }

  static std::vector<Pipeline> TakePipelines(Entry& entry) {
    std::vector<Pipeline> result;
    result.reserve(entry.retired.size() + entry.selected.has_value());
    if (entry.selected) result.push_back(std::move(*entry.selected));
    entry.selected.reset();
    for (Pipeline& pipeline : entry.retired)
      result.push_back(std::move(pipeline));
    entry.retired.clear();
    return result;
  }

  void WaitForNoBinds(std::unique_lock<std::mutex>& lock,
      const std::shared_ptr<Entry>& entry) {
    no_binds_.wait(lock, [&] { return entry->bind_count == 0; });
  }

  void ReleaseBind(const std::shared_ptr<Entry>& entry) noexcept {
    std::lock_guard lock(mutex_);
    if (entry->bind_count != 0) --entry->bind_count;
    if (entry->bind_count == 0) no_binds_.notify_all();
  }

  std::uint64_t IssueGeneration() noexcept {
    if (generation_exhausted_ || next_generation_ == UINT64_MAX) {
      generation_exhausted_ = true;
      return 0;
    }
    return next_generation_++;
  }

  mutable std::mutex mutex_;
  std::condition_variable no_binds_;
  std::unordered_map<Key, std::shared_ptr<Entry>, KeyHash> entries_;
  std::uint64_t next_generation_ = 1;
  bool generation_exhausted_ = false;
};

}

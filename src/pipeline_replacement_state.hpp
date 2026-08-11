// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wuwa_tfr {

// Thread-safe ownership index for application-pipeline replacement handles.
// Readers keep the shared lock through the caller-provided bind operation so a
// destroy callback cannot invalidate the selected replacement concurrently.
template <typename Owner, typename Pipeline>
class PipelineReplacementState {
 public:
  struct Removed {
    std::optional<Pipeline> original;
    std::optional<Pipeline> final_antifade;
  };

  struct Drained {
    std::uint64_t application_pipeline = 0;
    Removed replacements;
  };

  template <typename Bind>
  bool WithSelected(
      Owner owner,
      std::uint64_t application_pipeline,
      bool antifade_enabled,
      bool allow_final_antifade,
      Bind&& bind) const {
    std::shared_lock lock(mutex_);
    if (antifade_enabled && !allow_final_antifade) return false;
    const auto it = entries_.find(Key{owner, application_pipeline});
    if (it == entries_.end()) return false;
    const auto& selected =
        antifade_enabled ? it->second.final_antifade : it->second.original;
    if (!selected) return false;
    std::forward<Bind>(bind)(*selected);
    return true;
  }

  std::optional<Pipeline> PutOriginal(
      Owner owner,
      std::uint64_t application_pipeline,
      Pipeline replacement) {
    return Put(owner, application_pipeline, std::move(replacement), false);
  }

  std::optional<Pipeline> PutFinalAntiFade(
      Owner owner,
      std::uint64_t application_pipeline,
      Pipeline replacement) {
    return Put(owner, application_pipeline, std::move(replacement), true);
  }

  Removed Remove(Owner owner, std::uint64_t application_pipeline) {
    std::unique_lock lock(mutex_);
    Removed removed;
    const auto it = entries_.find(Key{owner, application_pipeline});
    if (it == entries_.end()) return removed;
    Take(it->second, removed);
    entries_.erase(it);
    return removed;
  }

  std::vector<Drained> DrainOwner(Owner owner) {
    std::unique_lock lock(mutex_);
    std::vector<Drained> drained;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->first.owner != owner) {
        ++it;
        continue;
      }
      Drained item;
      item.application_pipeline = it->first.application_pipeline;
      Take(it->second, item.replacements);
      drained.push_back(std::move(item));
      it = entries_.erase(it);
    }
    return drained;
  }

  std::pair<std::size_t, std::size_t> Sizes() const {
    std::shared_lock lock(mutex_);
    return {original_count_, final_antifade_count_};
  }

 private:
  struct Key {
    Owner owner;
    std::uint64_t application_pipeline;

    friend bool operator==(const Key&, const Key&) = default;
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
      std::size_t hash = std::hash<Owner>{}(key.owner);
      const std::size_t pipeline_hash =
          std::hash<std::uint64_t>{}(key.application_pipeline);
      hash ^= pipeline_hash + static_cast<std::size_t>(0x9E3779B9u) +
          (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  struct Entry {
    std::optional<Pipeline> original;
    std::optional<Pipeline> final_antifade;
  };

  std::optional<Pipeline> Put(
      Owner owner,
      std::uint64_t application_pipeline,
      Pipeline replacement,
      bool final_antifade) {
    std::unique_lock lock(mutex_);
    auto& entry = entries_[Key{owner, application_pipeline}];
    auto& slot = final_antifade ? entry.final_antifade : entry.original;
    if (!slot) {
      slot.emplace(std::move(replacement));
      (final_antifade ? final_antifade_count_ : original_count_)++;
      return std::nullopt;
    }
    std::optional<Pipeline> previous(std::move(*slot));
    *slot = std::move(replacement);
    return previous;
  }

  void Take(Entry& entry, Removed& removed) {
    if (entry.original) {
      removed.original.emplace(std::move(*entry.original));
      --original_count_;
    }
    if (entry.final_antifade) {
      removed.final_antifade.emplace(std::move(*entry.final_antifade));
      --final_antifade_count_;
    }
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<Key, Entry, KeyHash> entries_;
  std::size_t original_count_ = 0;
  std::size_t final_antifade_count_ = 0;
};

} // namespace wuwa_tfr

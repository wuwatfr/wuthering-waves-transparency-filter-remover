// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/resource_lifecycle_state.hpp"

#include <mutex>

namespace wuwa_tfr::dev {

namespace {

std::mutex g_resource_lifecycle_mutex;
wuwa_tfr::TraceIncarnationIndex<TraceResourceIdentity>
    g_resource_lifecycle_index;
ResourceLifecycleCapacityTaint g_resource_lifecycle_taint;

}  // namespace

ResourceLifecycleActivation ActivateResourceLifecycle(
    wuwa_tfr::TraceLiveHandleKey key, const TraceResourceIdentity& identity) {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  return g_resource_lifecycle_index.Activate(key, identity);
}

void DestroyResourceLifecycle(wuwa_tfr::TraceLiveHandleKey key) {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  g_resource_lifecycle_index.Destroy(key);
}

void DestroyResourceLifecycleForDevice(std::uintptr_t device) {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  g_resource_lifecycle_index.DestroyWhere(
      [device](const auto& key) { return key.owner == device; });
}

ActiveResourceLifecycle FindActiveResourceLifecycle(
    wuwa_tfr::TraceLiveHandleKey key) {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  const auto* record = g_resource_lifecycle_index.FindActive(key);
  return record
      ? ActiveResourceLifecycle{record->id, record->identity.dynamic_contents}
      : ActiveResourceLifecycle{};
}

std::size_t PruneResourceLifecycleTo(std::size_t maximum_records) {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  const std::size_t pruned = g_resource_lifecycle_index.PruneTo(maximum_records);
  if (pruned != 0) {
    g_resource_lifecycle_taint.evidence_dropped = true;
    ++g_resource_lifecycle_taint.prune_generation;
  }
  return pruned;
}

ResourceLifecycleCapacityTaint ResourceLifecycleCapacityTaintSnapshot() {
  std::lock_guard lock(g_resource_lifecycle_mutex);
  return g_resource_lifecycle_taint;
}

}  // namespace wuwa_tfr::dev

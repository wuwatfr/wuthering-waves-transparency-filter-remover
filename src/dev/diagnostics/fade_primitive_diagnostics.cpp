// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/diagnostics/fade_primitive_diagnostics.hpp"

#include <algorithm>
#include <unordered_set>
#include <vector>

#include "dev/trace/trace_events.hpp"
#include "production/addon_shared.hpp"

namespace wuwa_tfr::dev {

std::mutex g_fade_primitive_diagnostics_mutex;
std::unordered_map<std::uint64_t, bool> g_fade_primitive_highlighted_hashes;

std::string FadePrimitiveConsumers(
    const wuwa_tfr::FadePrimitiveDiagnostic& diagnostic) {
  std::vector<const char*> names;
  for (const auto& instance : diagnostic.instances) {
    const char* name = wuwa_tfr::FadePrimitiveConsumerName(instance.consumer);
    if (std::find(names.begin(), names.end(), name) == names.end())
      names.push_back(name);
  }
  std::string result;
  for (const char* name : names) {
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result.empty() ? "unknown" : result;
}

std::optional<FadePrimitiveDiagnosticSummary> VerifiedFadePrimitiveTarget(
    std::uint64_t shader_hash) {
  std::lock_guard lock(g_inspection_mutex);
  const auto inspection = g_inspections.find(shader_hash);
  if (inspection == g_inspections.end() || !inspection->second.success ||
      inspection->second.fade_primitive.instances.empty())
    return std::nullopt;
  return FadePrimitiveDiagnosticSummary{
      static_cast<std::uint32_t>(
          inspection->second.fade_primitive.instances.size()),
      FadePrimitiveConsumers(inspection->second.fade_primitive)};
}

void ImportCurrentCapturedPsosIntoHighlights() {
  std::unordered_set<std::uint64_t> hashes;
  for (const auto& row : ConcreteTraceSnapshot())
    hashes.insert(row.pipeline.shader_hash);
  std::lock_guard lock(g_fade_primitive_diagnostics_mutex);
  for (const std::uint64_t shader_hash : hashes)
    g_fade_primitive_highlighted_hashes.try_emplace(shader_hash, false);
}

void SetFadePrimitiveHighlighted(std::uint64_t shader_hash, bool highlighted) {
  std::lock_guard lock(g_fade_primitive_diagnostics_mutex);
  g_fade_primitive_highlighted_hashes[shader_hash] = highlighted;
}

void ClearFadePrimitiveHighlights() {
  std::lock_guard lock(g_fade_primitive_diagnostics_mutex);
  g_fade_primitive_highlighted_hashes.clear();
}

}  // namespace wuwa_tfr::dev

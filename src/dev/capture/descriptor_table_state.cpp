// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/descriptor_table_state.hpp"

namespace wuwa_tfr::dev {

std::optional<DescriptorCbvSlot> ResolveDescriptorTableCbvSlot(
    const std::vector<DescriptorCbvRangeInfo>& ranges,
    std::uint32_t register_space, std::uint32_t register_index) {
  std::optional<DescriptorCbvSlot> match;
  for (const auto& range : ranges) {
    if (range.register_space != register_space) continue;
    if (register_index < range.register_index) continue;
    const std::uint32_t offset = register_index - range.register_index;
    if (offset >= range.count) continue;
    if (match) return std::nullopt;
    match = DescriptorCbvSlot{range.param_index, range.binding_start + offset};
  }
  return match;
}

std::optional<std::uint32_t> ResolvePushConstantBackedParam(
    const std::vector<PushConstantRangeInfo>& ranges,
    std::uint32_t register_space, std::uint32_t register_index) {
  std::optional<std::uint32_t> match;
  for (const auto& range : ranges) {
    if (range.register_space != register_space ||
        range.register_index != register_index)
      continue;
    if (match) return std::nullopt;
    match = range.param_index;
  }
  return match;
}

bool SetDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& key,
    std::optional<DescriptorSlotContent> content) {
  if (!content) {
    table.erase(key);
    return true;
  }
  if (table.size() >= kMaxTrackedDescriptorSlots && !table.contains(key))
    return false;
  table[key] = *content;
  return true;
}

bool CopyDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& source, const DescriptorSlotKey& dest) {
  const auto it = table.find(source);
  if (it == table.end()) {
    table.erase(dest);
    return true;
  }
  return SetDescriptorTableSlot(table, dest, it->second);
}

void InvalidateDescriptorTableSlotsForResource(DescriptorSlotTable& table,
    std::uintptr_t device, std::uint64_t resource_handle) {
  std::erase_if(table, [device, resource_handle](const auto& entry) {
    return entry.first.table.owner == device &&
        entry.second.resource_handle == resource_handle;
  });
}

std::optional<DescriptorSlotContent> FindDescriptorTableSlot(
    const DescriptorSlotTable& table, const DescriptorSlotKey& key) {
  const auto it = table.find(key);
  return it == table.end() ? std::nullopt
                            : std::optional<DescriptorSlotContent>(it->second);
}

}

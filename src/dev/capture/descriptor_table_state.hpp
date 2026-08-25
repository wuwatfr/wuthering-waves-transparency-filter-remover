// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

struct DescriptorCbvRangeInfo {
  std::uint32_t param_index = 0;
  std::uint32_t binding_start = 0;
  std::uint32_t register_space = 0;
  std::uint32_t register_index = 0;
  std::uint32_t count = 0;

  friend bool operator==(
      const DescriptorCbvRangeInfo&, const DescriptorCbvRangeInfo&) = default;
};

struct DescriptorCbvSlot {
  std::uint32_t param_index = 0;
  std::uint32_t slot = 0;

  friend bool operator==(const DescriptorCbvSlot&, const DescriptorCbvSlot&) =
      default;
};

std::optional<DescriptorCbvSlot> ResolveDescriptorTableCbvSlot(
    const std::vector<DescriptorCbvRangeInfo>& ranges,
    std::uint32_t register_space, std::uint32_t register_index);

// A D3D12 root-constants range. Deliberately not a DescriptorCbvRangeInfo:
// constant_count is the number of 32-bit constants inside the range, never a
// span of consecutive shader registers, so the range binds exactly
// register_space/register_index and nothing beyond it.
struct PushConstantRangeInfo {
  std::uint32_t param_index = 0;
  std::uint32_t register_space = 0;
  std::uint32_t register_index = 0;
  std::uint32_t constant_count = 0;

  friend bool operator==(
      const PushConstantRangeInfo&, const PushConstantRangeInfo&) = default;
};

// Returns the layout parameter backing exactly this shader register, or
// nullopt when no range declares it or more than one does.
std::optional<std::uint32_t> ResolvePushConstantBackedParam(
    const std::vector<PushConstantRangeInfo>& ranges,
    std::uint32_t register_space, std::uint32_t register_index);

struct DescriptorSlotKey {
  wuwa_tfr::TraceLiveHandleKey table;
  std::uint32_t slot = 0;

  friend bool operator==(const DescriptorSlotKey&, const DescriptorSlotKey&) =
      default;
};

struct DescriptorSlotKeyHash {
  std::size_t operator()(const DescriptorSlotKey& key) const noexcept {
    std::size_t hash = wuwa_tfr::TraceLiveHandleKeyHash{}(key.table);
    wuwa_tfr::TraceHashCombine(hash, key.slot);
    return hash;
  }
};

struct DescriptorSlotContent {
  std::uint64_t resource_handle = 0;
  std::uint64_t resource_incarnation = 0;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;

  friend bool operator==(
      const DescriptorSlotContent&, const DescriptorSlotContent&) = default;
};

using DescriptorSlotTable = std::unordered_map<DescriptorSlotKey,
    DescriptorSlotContent, DescriptorSlotKeyHash>;

constexpr std::size_t kMaxTrackedDescriptorSlots = 4096;

bool SetDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& key, std::optional<DescriptorSlotContent> content);

// Returns false only when copying an existing source slot's content into
// dest failed because the table was at capacity; returns true both when the
// copy succeeded and when source was unbound (dest is simply cleared, which
// is never a capacity failure).
bool CopyDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& source, const DescriptorSlotKey& dest);

void InvalidateDescriptorTableSlotsForResource(DescriptorSlotTable& table,
    std::uintptr_t device, std::uint64_t resource_handle);

std::optional<DescriptorSlotContent> FindDescriptorTableSlot(
    const DescriptorSlotTable& table, const DescriptorSlotKey& key);

constexpr bool DescriptorSlotContentIsCurrent(
    const DescriptorSlotContent& content,
    std::uint64_t current_resource_incarnation) noexcept {
  return content.resource_incarnation == current_resource_incarnation;
}

constexpr bool DescriptorTableBindingHasExactDynamicOffsets(
    std::uint32_t dynamic_offset_count) noexcept {
  return dynamic_offset_count == 0;
}

}

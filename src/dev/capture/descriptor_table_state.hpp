// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only, ReShade-independent pieces of exact descriptor-table-backed CBV
// resolution for the targeted Fade control-value tracer: static
// (register space, register index) -> (layout param, table-relative slot)
// resolution among a pipeline layout's declared CBV ranges, and the current
// (not accumulated-history) content of individual descriptor-table slots as
// observed through update_descriptor_tables/copy_descriptor_tables. See
// dev/capture/fade_control_runtime.* for the ReShade-facing event handlers
// that populate and consume this state.
//
// Every function here is pure and portable: no ReShade dependency, so the
// resolution/bookkeeping logic is unit-testable without a real device.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

// One CBV descriptor range statically declared in a pipeline layout's
// descriptor_table/descriptor_table_with_flags parameter. `binding_start` is
// the range's offset (in descriptors) from the start of its table -- the
// same numbering used by descriptor_table_update::binding, which is what
// makes the two sides of resolution (static layout, live update) directly
// comparable. Only exact, bounded ranges (count != 0, count != UINT32_MAX,
// array_size == 1) are ever recorded by the caller; see
// fade_control_runtime.cpp's OnInitFadeControlPipelineLayout.
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

// Resolves exactly which (param, table-relative slot) a static
// (register_space, register_index) maps to among `ranges`. Fails closed
// (returns std::nullopt) when zero ranges claim the register, or when more
// than one does -- an overlapping/ambiguous declaration is never guessed
// at, it is simply reported as unresolved.
std::optional<DescriptorCbvSlot> ResolveDescriptorTableCbvSlot(
    const std::vector<DescriptorCbvRangeInfo>& ranges,
    std::uint32_t register_space, std::uint32_t register_index);

// Identifies one descriptor in one allocated descriptor table: the table
// handle plus its table-relative slot (binding offset). Two different
// table allocations for the same root parameter never collide here, since
// `table_handle` is unique per allocation.
struct DescriptorSlotKey {
  std::uint64_t table_handle = 0;
  std::uint32_t slot = 0;

  friend bool operator==(const DescriptorSlotKey&, const DescriptorSlotKey&) =
      default;
};

struct DescriptorSlotKeyHash {
  std::size_t operator()(const DescriptorSlotKey& key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.table_handle);
    wuwa_tfr::TraceHashCombine(hash, key.slot);
    return hash;
  }
};

// Current (not accumulated-history) content of one descriptor-table slot,
// as last written by update_descriptor_tables or propagated by
// copy_descriptor_tables. `resource_incarnation` is whatever the caller's
// own resource-lifetime tracking reported active for `resource_handle` at
// the moment this entry was written -- see
// DescriptorSlotContentIsCurrent() below for why it must be re-checked
// before trusting this entry at a later sample time.
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

// Overwrites the content at `key` (or, if `content` is std::nullopt, erases
// it -- the descriptor_table_update convention for a null/empty buffer
// range). Bounded: refuses to add a *new* key once `table` already holds
// kMaxTrackedDescriptorSlots entries (an existing key may still be updated
// or erased). Returns false only on that refusal.
bool SetDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& key, std::optional<DescriptorSlotContent> content);

// Mirrors one copy_descriptor_tables entry: copies `source`'s current
// content to `dest`. If `source` has no known content, `dest` is erased
// rather than left holding whatever it had before -- an unknown copy
// invalidates its destination, it never silently preserves stale data.
void CopyDescriptorTableSlot(DescriptorSlotTable& table,
    const DescriptorSlotKey& source, const DescriptorSlotKey& dest);

// Removes every slot (in any table) whose cached resource_handle is
// `resource_handle`. Called when that resource is destroyed, so a later,
// unrelated object that happens to reuse the same handle value can never be
// misattributed to a slot recorded before the destroy.
void InvalidateDescriptorTableSlotsForResource(
    DescriptorSlotTable& table, std::uint64_t resource_handle);

// Plain lookup; std::nullopt for an unknown/unbound slot.
std::optional<DescriptorSlotContent> FindDescriptorTableSlot(
    const DescriptorSlotTable& table, const DescriptorSlotKey& key);

// True only if `content`'s cached incarnation still matches the resource's
// current one. Descriptor-table slots are typically written once (at
// resource-allocation time) and then read for the remaining lifetime of the
// process -- unlike root/pushed CBVs, which are rebound most draws -- so a
// stale slot (its resource destroyed and the handle reused since) is a real
// risk here and must be explicitly rejected rather than trusted.
constexpr bool DescriptorSlotContentIsCurrent(
    const DescriptorSlotContent& content,
    std::uint64_t current_resource_incarnation) noexcept {
  return content.resource_incarnation == current_resource_incarnation;
}

// The D3D12 addon backend always passes dynamic_offset_count == 0 to
// bind_descriptor_tables (there is no native D3D12 equivalent of a Vulkan
// dynamic-offset descriptor). This predicate exists so that fact is
// enforced structurally rather than assumed: a nonzero count means this
// binding's exact offset correspondence cannot be proven, and must be
// reported unresolved rather than assumed to be zero.
constexpr bool DescriptorTableBindingHasExactDynamicOffsets(
    std::uint32_t dynamic_offset_count) noexcept {
  return dynamic_offset_count == 0;
}

}  // namespace wuwa_tfr::dev

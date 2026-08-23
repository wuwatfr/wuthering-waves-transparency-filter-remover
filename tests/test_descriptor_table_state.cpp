// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/descriptor_table_state.hpp"

#include <cassert>
#include <cstdio>

using wuwa_tfr::dev::CopyDescriptorTableSlot;
using wuwa_tfr::dev::DescriptorCbvRangeInfo;
using wuwa_tfr::dev::DescriptorSlotContent;
using wuwa_tfr::dev::DescriptorSlotContentIsCurrent;
using wuwa_tfr::dev::DescriptorSlotKey;
using wuwa_tfr::dev::DescriptorSlotTable;
using wuwa_tfr::dev::DescriptorTableBindingHasExactDynamicOffsets;
using wuwa_tfr::dev::FindDescriptorTableSlot;
using wuwa_tfr::dev::ResolveDescriptorTableCbvSlot;
using wuwa_tfr::dev::SetDescriptorTableSlot;

namespace {

void TestRegisterResolvesToCorrectSlot() {
  const std::vector<DescriptorCbvRangeInfo> ranges{
      {/*param_index=*/2, /*binding_start=*/5, /*register_space=*/0,
          /*register_index=*/3, /*count=*/1}};
  const auto slot = ResolveDescriptorTableCbvSlot(ranges, 0, 3);
  assert(slot.has_value());
  assert(slot->param_index == 2);
  assert(slot->slot == 5);
}

void TestRangeOffsetIsHonored() {
  // A single range spanning registers t10..t14 (space 0), table-relative
  // offset 8: register 12 must resolve to slot 8 + (12-10) = 10, not 8.
  const std::vector<DescriptorCbvRangeInfo> ranges{
      {/*param_index=*/0, /*binding_start=*/8, /*register_space=*/0,
          /*register_index=*/10, /*count=*/5}};
  const auto slot = ResolveDescriptorTableCbvSlot(ranges, 0, 12);
  assert(slot.has_value());
  assert(slot->slot == 10);
}

void TestAmbiguousRangeFailsClosed() {
  const std::vector<DescriptorCbvRangeInfo> ranges{
      {0, 0, 0, 0, 4},
      {1, 0, 0, 0, 4},  // Same (space=0, register=0..3): overlapping/ambiguous.
  };
  const auto slot = ResolveDescriptorTableCbvSlot(ranges, 0, 0);
  assert(!slot.has_value());
}

void TestNoMatchingRangeFailsClosed() {
  const std::vector<DescriptorCbvRangeInfo> ranges{
      {0, 0, 0, 0, 4},
  };
  assert(!ResolveDescriptorTableCbvSlot(ranges, 1, 0).has_value());
  assert(!ResolveDescriptorTableCbvSlot(ranges, 0, 4).has_value());
}

void TestUpdateOverwritesCurrentDescriptor() {
  DescriptorSlotTable table;
  const DescriptorSlotKey key{100, 3};
  assert(SetDescriptorTableSlot(
      table, key, DescriptorSlotContent{1, 1, 0, 256}));
  assert(SetDescriptorTableSlot(
      table, key, DescriptorSlotContent{2, 2, 64, 256}));
  const auto content = FindDescriptorTableSlot(table, key);
  assert(content.has_value());
  assert(content->resource_handle == 2);
  assert(content->resource_incarnation == 2);
  assert(content->offset == 64);
}

void TestCopyPropagatesCurrentDescriptor() {
  DescriptorSlotTable table;
  const DescriptorSlotKey source{100, 0};
  const DescriptorSlotKey dest{200, 5};
  SetDescriptorTableSlot(table, source, DescriptorSlotContent{7, 3, 16, 64});
  CopyDescriptorTableSlot(table, source, dest);
  const auto content = FindDescriptorTableSlot(table, dest);
  assert(content.has_value());
  assert(content->resource_handle == 7);
  assert(content->resource_incarnation == 3);
  assert(content->offset == 16);
  assert(content->size == 64);
}

void TestUnknownCopyInvalidatesDestination() {
  DescriptorSlotTable table;
  const DescriptorSlotKey source{999, 0};  // never written
  const DescriptorSlotKey dest{200, 5};
  SetDescriptorTableSlot(table, dest, DescriptorSlotContent{7, 3, 16, 64});
  CopyDescriptorTableSlot(table, source, dest);
  assert(!FindDescriptorTableSlot(table, dest).has_value());
}

void TestUnboundSlotFailsClosed() {
  DescriptorSlotTable table;
  assert(!FindDescriptorTableSlot(table, DescriptorSlotKey{1, 0}).has_value());
}

void TestResourceIncarnationMismatchIsRejected() {
  const DescriptorSlotContent content{5, /*resource_incarnation=*/10, 0, 64};
  assert(DescriptorSlotContentIsCurrent(content, 10));
  assert(!DescriptorSlotContentIsCurrent(content, 11));
}

void TestDestroyInvalidatesEveryReferencingSlot() {
  DescriptorSlotTable table;
  SetDescriptorTableSlot(
      table, DescriptorSlotKey{10, 0}, DescriptorSlotContent{42, 1, 0, 64});
  SetDescriptorTableSlot(
      table, DescriptorSlotKey{20, 3}, DescriptorSlotContent{42, 1, 0, 64});
  SetDescriptorTableSlot(
      table, DescriptorSlotKey{30, 1}, DescriptorSlotContent{99, 1, 0, 64});
  wuwa_tfr::dev::InvalidateDescriptorTableSlotsForResource(table, 42);
  assert(!FindDescriptorTableSlot(table, DescriptorSlotKey{10, 0}).has_value());
  assert(!FindDescriptorTableSlot(table, DescriptorSlotKey{20, 3}).has_value());
  assert(FindDescriptorTableSlot(table, DescriptorSlotKey{30, 1}).has_value());
}

void TestSlotCapacityIsBounded() {
  DescriptorSlotTable table;
  for (std::uint32_t i = 0; i < wuwa_tfr::dev::kMaxTrackedDescriptorSlots; ++i) {
    assert(SetDescriptorTableSlot(
        table, DescriptorSlotKey{1, i}, DescriptorSlotContent{1, 1, 0, 4}));
  }
  // Table is full: a brand-new key is refused...
  assert(!SetDescriptorTableSlot(table, DescriptorSlotKey{1, 999999},
      DescriptorSlotContent{1, 1, 0, 4}));
  // ...but an existing key may still be updated.
  assert(SetDescriptorTableSlot(
      table, DescriptorSlotKey{1, 0}, DescriptorSlotContent{2, 2, 0, 4}));
}

void TestDynamicOffsetExactOrFailClosed() {
  assert(DescriptorTableBindingHasExactDynamicOffsets(0));
  assert(!DescriptorTableBindingHasExactDynamicOffsets(1));
  assert(!DescriptorTableBindingHasExactDynamicOffsets(4));
}

}  // namespace

int main() {
  TestRegisterResolvesToCorrectSlot();
  TestRangeOffsetIsHonored();
  TestAmbiguousRangeFailsClosed();
  TestNoMatchingRangeFailsClosed();
  TestUpdateOverwritesCurrentDescriptor();
  TestCopyPropagatesCurrentDescriptor();
  TestUnknownCopyInvalidatesDestination();
  TestUnboundSlotFailsClosed();
  TestResourceIncarnationMismatchIsRejected();
  TestDestroyInvalidatesEveryReferencingSlot();
  TestSlotCapacityIsBounded();
  TestDynamicOffsetExactOrFailClosed();
  std::puts("test_descriptor_table_state: all tests passed");
  return 0;
}

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
using wuwa_tfr::dev::PushConstantRangeInfo;
using wuwa_tfr::dev::ResolveDescriptorTableCbvSlot;
using wuwa_tfr::dev::ResolvePushConstantBackedParam;
using wuwa_tfr::dev::SetDescriptorTableSlot;

namespace {

constexpr std::uintptr_t kDeviceA = 1;
constexpr std::uintptr_t kDeviceB = 2;

DescriptorSlotKey Key(std::uintptr_t device, std::uint64_t table_handle,
    std::uint32_t slot) {
  return DescriptorSlotKey{{device, table_handle}, slot};
}

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

void TestPushConstantCountIsNotARegisterSpan() {
  // Root constants at b5 declaring 16 32-bit constants. The 16 is a DWORD
  // count inside b5, so only b5 is push-constant-backed: b6 and b20 are
  // unrelated registers this parameter never binds.
  const std::vector<PushConstantRangeInfo> ranges{
      {/*param_index=*/3, /*register_space=*/0, /*register_index=*/5,
          /*constant_count=*/16}};
  const auto matched = ResolvePushConstantBackedParam(ranges, 0, 5);
  assert(matched.has_value() && *matched == 3);
  assert(!ResolvePushConstantBackedParam(ranges, 0, 6).has_value());
  assert(!ResolvePushConstantBackedParam(ranges, 0, 20).has_value());
  assert(!ResolvePushConstantBackedParam(ranges, 0, 4).has_value());
}

void TestPushConstantRegisterSpaceIsHonored() {
  const std::vector<PushConstantRangeInfo> ranges{
      {/*param_index=*/1, /*register_space=*/0, /*register_index=*/2,
          /*constant_count=*/4}};
  assert(ResolvePushConstantBackedParam(ranges, 0, 2).has_value());
  // Same register number in a different space is a different binding.
  assert(!ResolvePushConstantBackedParam(ranges, 1, 2).has_value());
}

void TestMultiplePushConstantParamsResolveIndependently() {
  const std::vector<PushConstantRangeInfo> ranges{
      {/*param_index=*/0, /*register_space=*/0, /*register_index=*/1,
          /*constant_count=*/4},
      {/*param_index=*/2, /*register_space=*/0, /*register_index=*/7,
          /*constant_count=*/32},
      {/*param_index=*/4, /*register_space=*/1, /*register_index=*/1,
          /*constant_count=*/1},
  };
  const auto first = ResolvePushConstantBackedParam(ranges, 0, 1);
  const auto second = ResolvePushConstantBackedParam(ranges, 0, 7);
  const auto third = ResolvePushConstantBackedParam(ranges, 1, 1);
  assert(first.has_value() && *first == 0);
  assert(second.has_value() && *second == 2);
  assert(third.has_value() && *third == 4);
  // b2..b6 sit between the first two ranges and belong to neither, even
  // though the b1 range declares 4 constants and the b7 range declares 32.
  assert(!ResolvePushConstantBackedParam(ranges, 0, 2).has_value());
  assert(!ResolvePushConstantBackedParam(ranges, 0, 6).has_value());
  assert(!ResolvePushConstantBackedParam(ranges, 0, 8).has_value());
}

void TestAmbiguousPushConstantRangeFailsClosed() {
  const std::vector<PushConstantRangeInfo> ranges{
      {0, 0, 3, 4},
      {1, 0, 3, 8},  // Two parameters claiming b3: not resolvable.
  };
  assert(!ResolvePushConstantBackedParam(ranges, 0, 3).has_value());
}

void TestUnmatchedSourceIsNotPushConstantBacked() {
  // The register a Fade-control source actually reads is not declared by any
  // root-constant range, so TryReportPushConstantBackedSource must decline
  // and let the caller fall through to unsupported/unresolved.
  const std::vector<PushConstantRangeInfo> ranges{
      {/*param_index=*/0, /*register_space=*/0, /*register_index=*/0,
          /*constant_count=*/64}};
  assert(!ResolvePushConstantBackedParam(ranges, 0, 13).has_value());
  assert(!ResolvePushConstantBackedParam({}, 0, 0).has_value());
}

void TestDescriptorRangeCountStillSpansRegisters() {
  // The descriptor-range meaning of count is unchanged by the push-constant
  // fix: a CBV range at b5 with count=16 does cover b5..b20.
  const std::vector<DescriptorCbvRangeInfo> ranges{
      {/*param_index=*/3, /*binding_start=*/0, /*register_space=*/0,
          /*register_index=*/5, /*count=*/16}};
  assert(ResolveDescriptorTableCbvSlot(ranges, 0, 5).has_value());
  const auto last = ResolveDescriptorTableCbvSlot(ranges, 0, 20);
  assert(last.has_value() && last->slot == 15);
  assert(!ResolveDescriptorTableCbvSlot(ranges, 0, 21).has_value());
}

void TestUpdateOverwritesCurrentDescriptor() {
  DescriptorSlotTable table;
  const DescriptorSlotKey key = Key(kDeviceA, 100, 3);
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
  const DescriptorSlotKey source = Key(kDeviceA, 100, 0);
  const DescriptorSlotKey dest = Key(kDeviceA, 200, 5);
  SetDescriptorTableSlot(table, source, DescriptorSlotContent{7, 3, 16, 64});
  assert(CopyDescriptorTableSlot(table, source, dest));
  const auto content = FindDescriptorTableSlot(table, dest);
  assert(content.has_value());
  assert(content->resource_handle == 7);
  assert(content->resource_incarnation == 3);
  assert(content->offset == 16);
  assert(content->size == 64);
}

void TestUnknownCopyInvalidatesDestination() {
  DescriptorSlotTable table;
  const DescriptorSlotKey source = Key(kDeviceA, 999, 0);  // never written
  const DescriptorSlotKey dest = Key(kDeviceA, 200, 5);
  SetDescriptorTableSlot(table, dest, DescriptorSlotContent{7, 3, 16, 64});
  // An unbound source clearing dest is a normal outcome, not a capacity
  // failure -- must report success.
  assert(CopyDescriptorTableSlot(table, source, dest));
  assert(!FindDescriptorTableSlot(table, dest).has_value());
}

void TestCopyReportsCapacityFailure() {
  DescriptorSlotTable table;
  const DescriptorSlotKey source = Key(kDeviceA, 100, 0);
  SetDescriptorTableSlot(table, source, DescriptorSlotContent{7, 3, 16, 64});
  for (std::uint32_t i = 1; i < wuwa_tfr::dev::kMaxTrackedDescriptorSlots;
       ++i) {
    SetDescriptorTableSlot(
        table, Key(kDeviceA, 1, i), DescriptorSlotContent{1, 1, 0, 4});
  }
  // Table is now exactly at capacity (source counts as one of the slots).
  const DescriptorSlotKey brand_new_dest = Key(kDeviceA, 999999, 0);
  // A copy into a genuinely new key at capacity must report failure, not
  // silently drop it -- this is the exact case CopyDescriptorTableSlot used
  // to swallow.
  assert(!CopyDescriptorTableSlot(table, source, brand_new_dest));
  assert(!FindDescriptorTableSlot(table, brand_new_dest).has_value());

  // Copying into an EXISTING key at capacity is an update, not a capacity
  // failure.
  const DescriptorSlotKey existing_dest = Key(kDeviceA, 1, 1);
  assert(CopyDescriptorTableSlot(table, source, existing_dest));
  const auto updated = FindDescriptorTableSlot(table, existing_dest);
  assert(updated.has_value() && updated->resource_handle == 7);
}

void TestUnboundSlotFailsClosed() {
  DescriptorSlotTable table;
  assert(!FindDescriptorTableSlot(table, Key(kDeviceA, 1, 0)).has_value());
}

void TestSameTableAndSlotOnDifferentDevicesRemainDistinct() {
  DescriptorSlotTable table;
  SetDescriptorTableSlot(
      table, Key(kDeviceA, 100, 3), DescriptorSlotContent{1, 1, 0, 64});
  SetDescriptorTableSlot(
      table, Key(kDeviceB, 100, 3), DescriptorSlotContent{2, 2, 0, 64});
  assert(table.size() == 2);  // not deduplicated across devices

  const auto on_a = FindDescriptorTableSlot(table, Key(kDeviceA, 100, 3));
  const auto on_b = FindDescriptorTableSlot(table, Key(kDeviceB, 100, 3));
  assert(on_a.has_value() && on_a->resource_handle == 1);
  assert(on_b.has_value() && on_b->resource_handle == 2);
}

void TestResourceIncarnationMismatchIsRejected() {
  const DescriptorSlotContent content{5, /*resource_incarnation=*/10, 0, 64};
  assert(DescriptorSlotContentIsCurrent(content, 10));
  assert(!DescriptorSlotContentIsCurrent(content, 11));
}

void TestDestroyInvalidatesEveryReferencingSlot() {
  DescriptorSlotTable table;
  SetDescriptorTableSlot(
      table, Key(kDeviceA, 10, 0), DescriptorSlotContent{42, 1, 0, 64});
  SetDescriptorTableSlot(
      table, Key(kDeviceA, 20, 3), DescriptorSlotContent{42, 1, 0, 64});
  SetDescriptorTableSlot(
      table, Key(kDeviceA, 30, 1), DescriptorSlotContent{99, 1, 0, 64});
  wuwa_tfr::dev::InvalidateDescriptorTableSlotsForResource(
      table, kDeviceA, 42);
  assert(!FindDescriptorTableSlot(table, Key(kDeviceA, 10, 0)).has_value());
  assert(!FindDescriptorTableSlot(table, Key(kDeviceA, 20, 3)).has_value());
  assert(FindDescriptorTableSlot(table, Key(kDeviceA, 30, 1)).has_value());
}

void TestResourceInvalidationIsDeviceLocal() {
  DescriptorSlotTable table;
  // Both devices happen to report the same raw resource handle (42): a
  // realistic case, since raw D3D12 handles are only unique per device.
  SetDescriptorTableSlot(
      table, Key(kDeviceA, 10, 0), DescriptorSlotContent{42, 1, 0, 64});
  SetDescriptorTableSlot(
      table, Key(kDeviceB, 10, 0), DescriptorSlotContent{42, 1, 0, 64});
  wuwa_tfr::dev::InvalidateDescriptorTableSlotsForResource(
      table, kDeviceA, 42);
  assert(!FindDescriptorTableSlot(table, Key(kDeviceA, 10, 0)).has_value());
  // Device B's slot referencing the same raw handle must survive.
  assert(FindDescriptorTableSlot(table, Key(kDeviceB, 10, 0)).has_value());
}

void TestSlotCapacityIsBounded() {
  DescriptorSlotTable table;
  for (std::uint32_t i = 0; i < wuwa_tfr::dev::kMaxTrackedDescriptorSlots; ++i) {
    assert(SetDescriptorTableSlot(
        table, Key(kDeviceA, 1, i), DescriptorSlotContent{1, 1, 0, 4}));
  }
  // Table is full: a brand-new key is refused...
  assert(!SetDescriptorTableSlot(table, Key(kDeviceA, 1, 999999),
      DescriptorSlotContent{1, 1, 0, 4}));
  // ...but an existing key may still be updated.
  assert(SetDescriptorTableSlot(
      table, Key(kDeviceA, 1, 0), DescriptorSlotContent{2, 2, 0, 4}));
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
  TestPushConstantCountIsNotARegisterSpan();
  TestPushConstantRegisterSpaceIsHonored();
  TestMultiplePushConstantParamsResolveIndependently();
  TestAmbiguousPushConstantRangeFailsClosed();
  TestUnmatchedSourceIsNotPushConstantBacked();
  TestDescriptorRangeCountStillSpansRegisters();
  TestUpdateOverwritesCurrentDescriptor();
  TestCopyPropagatesCurrentDescriptor();
  TestUnknownCopyInvalidatesDestination();
  TestCopyReportsCapacityFailure();
  TestUnboundSlotFailsClosed();
  TestSameTableAndSlotOnDifferentDevicesRemainDistinct();
  TestResourceIncarnationMismatchIsRejected();
  TestDestroyInvalidatesEveryReferencingSlot();
  TestResourceInvalidationIsDeviceLocal();
  TestSlotCapacityIsBounded();
  TestDynamicOffsetExactOrFailClosed();
  std::puts("test_descriptor_table_state: all tests passed");
  return 0;
}

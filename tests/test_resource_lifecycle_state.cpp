// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/resource_lifecycle_state.hpp"

#include "test_check.hpp"

// Trace and Fade-control used to each own an independent
// TraceIncarnationIndex for the same D3D12 resource lifetimes -- and
// Fade-control's used a constant identity, so it could never detect a
// handle reused for a different resource without an intervening destroy.
// ActivateResourceLifecycle/FindActiveResourceLifecycle/etc. are now the
// one canonical, device-scoped owner both consult; these tests exercise
// that owner directly, the same way both real consumers do.
//
// The owner is one process-wide singleton with no reset hook (matching
// every other global lifecycle index in this codebase), so every test
// below uses its own distinct resource handles rather than relying on a
// fresh container per test.

namespace {

using wuwa_tfr::TraceLiveHandleKey;
using wuwa_tfr::dev::ActivateResourceLifecycle;
using wuwa_tfr::dev::DestroyResourceLifecycle;
using wuwa_tfr::dev::DestroyResourceLifecycleForDevice;
using wuwa_tfr::dev::FindActiveResourceLifecycle;
using wuwa_tfr::dev::PruneResourceLifecycleTo;
using wuwa_tfr::dev::ResourceLifecycleCapacityTaintSnapshot;
using wuwa_tfr::dev::TraceResourceIdentity;

constexpr std::uintptr_t kDeviceA = 1;
constexpr std::uintptr_t kDeviceB = 2;
constexpr std::uintptr_t kDeviceC = 3;

TraceResourceIdentity Identity(std::uint64_t fingerprint,
    bool dynamic_contents = false) {
  return TraceResourceIdentity{fingerprint, dynamic_contents};
}

}  // namespace

int main() {
  // 0. Nothing has pruned yet in this process, so the capacity taint starts
  // clean. Captured before any other test runs, so test 8 below can observe
  // the one-way transition rather than assuming it.
  const auto initial_taint = ResourceLifecycleCapacityTaintSnapshot();
  CHECK(!initial_taint.evidence_dropped);
  CHECK(initial_taint.prune_generation == 0);

  // 1. Normal init/destroy/re-init: destroying an incarnation and then
  // re-initializing the same handle produces a genuinely new incarnation
  // id, not a stale one. Uses kDeviceC, never torn down by a later test,
  // since test 7 below expects this key to still be live.
  TraceLiveHandleKey test1_key{kDeviceC, 100};
  {
    const auto first = ActivateResourceLifecycle(test1_key, Identity(0xAAAA));
    CHECK(first.id != 0 && !first.duplicate);
    CHECK(FindActiveResourceLifecycle(test1_key).incarnation_id == first.id);

    DestroyResourceLifecycle(test1_key);
    CHECK(FindActiveResourceLifecycle(test1_key).incarnation_id == 0);

    const auto second = ActivateResourceLifecycle(test1_key, Identity(0xAAAA));
    CHECK(second.id != first.id);
    CHECK(!second.duplicate);
    CHECK(FindActiveResourceLifecycle(test1_key).incarnation_id == second.id);
  }

  // 2. Same raw handle on two different devices: distinct incarnations,
  // and destroying one device's never touches the other's.
  {
    const TraceLiveHandleKey key_a{kDeviceA, 200};
    const TraceLiveHandleKey key_b{kDeviceB, 200};
    const auto on_a = ActivateResourceLifecycle(key_a, Identity(1));
    const auto on_b = ActivateResourceLifecycle(key_b, Identity(1));
    CHECK(on_a.id != on_b.id);

    DestroyResourceLifecycle(key_a);
    CHECK(FindActiveResourceLifecycle(key_a).incarnation_id == 0);
    CHECK(FindActiveResourceLifecycle(key_b).incarnation_id == on_b.id);
  }

  // 3. Duplicate identical init: re-activating the same key with the same
  // identity (no destroy in between) is a no-op duplicate, not a rotation.
  {
    const TraceLiveHandleKey key{kDeviceA, 300};
    const auto first = ActivateResourceLifecycle(key, Identity(7, true));
    const auto again = ActivateResourceLifecycle(key, Identity(7, true));
    CHECK(again.id == first.id);
    CHECK(again.duplicate);
    CHECK(!again.rotated_without_destroy);
  }

  // 4. Re-init with a changed identity and no destroy in between rotates
  // the incarnation and reports the previous identity -- this is exactly
  // the handle-reuse-without-destroy case Fade-control's old constant
  // identity=0 index could never detect.
  {
    const TraceLiveHandleKey key{kDeviceA, 400};
    const auto first = ActivateResourceLifecycle(key, Identity(1));
    const auto rotated = ActivateResourceLifecycle(key, Identity(2));
    CHECK(rotated.id != first.id);
    CHECK(!rotated.duplicate);
    CHECK(rotated.rotated_without_destroy);
    CHECK(rotated.previous_identity.has_value());
    CHECK(rotated.previous_identity->fingerprint == 1);
    CHECK(FindActiveResourceLifecycle(key).incarnation_id == rotated.id);
  }

  // 5. Fade-control and Trace observe the same incarnation: simulate
  // Trace's init handler activating a resource, then Fade-control's
  // descriptor-table sampling path querying it independently -- both must
  // see the identical incarnation id, with no separate map involved.
  {
    const TraceLiveHandleKey key{kDeviceA, 500};
    const auto trace_side = ActivateResourceLifecycle(key, Identity(42));
    const auto fade_control_side = FindActiveResourceLifecycle(key);
    CHECK(fade_control_side.incarnation_id == trace_side.id);

    // And the reverse: Fade-control observes a destroy Trace's handler
    // performed, immediately and without any state of its own.
    DestroyResourceLifecycle(key);
    CHECK(FindActiveResourceLifecycle(key).incarnation_id == 0);
  }

  // 6. Device teardown isolates devices: tearing down device A's resource
  // lifecycle state must not affect device B's, even for identical raw
  // handles.
  {
    const TraceLiveHandleKey key_a{kDeviceA, 600};
    const TraceLiveHandleKey key_b{kDeviceB, 600};
    ActivateResourceLifecycle(key_a, Identity(1));
    const auto on_b = ActivateResourceLifecycle(key_b, Identity(1));

    DestroyResourceLifecycleForDevice(kDeviceA);
    CHECK(FindActiveResourceLifecycle(key_a).incarnation_id == 0);
    CHECK(FindActiveResourceLifecycle(key_b).incarnation_id == on_b.id);
  }

  // 7. Capacity pruning evicts the globally oldest records first, and
  // reports a count -- the caller (Trace) drives its own capacity
  // diagnostics from that count, exactly as it did against its own
  // formerly-private index.
  {
    // Still alive from test 1, and older than everything below: certain to
    // be among whatever gets evicted once capacity is enforced.
    CHECK(FindActiveResourceLifecycle(test1_key).incarnation_id != 0);

    TraceLiveHandleKey newest{kDeviceB, 0};
    for (std::uint64_t i = 0; i < 16; ++i) {
      newest = TraceLiveHandleKey{kDeviceB, 20000 + i};
      ActivateResourceLifecycle(newest, Identity(i));
    }

    const auto pruned = PruneResourceLifecycleTo(4);
    CHECK(pruned > 0);
    CHECK(FindActiveResourceLifecycle(test1_key).incarnation_id == 0);
    // The very last activation before pruning is certainly among the 4
    // most recently created records and must survive.
    CHECK(FindActiveResourceLifecycle(newest).incarnation_id != 0);
  }

  // 8. The capacity taint is monotonic. Test 7's prune dropped records, so
  // the flag is now set and the generation advanced; a prune that drops
  // nothing leaves both untouched; a further real prune advances the
  // generation again and never un-sets the flag.
  {
    const auto after_prune = ResourceLifecycleCapacityTaintSnapshot();
    CHECK(after_prune.evidence_dropped);
    CHECK(after_prune.prune_generation > initial_taint.prune_generation);

    // A no-op prune: capacity far above the record count.
    CHECK(PruneResourceLifecycleTo(1u << 20) == 0);
    const auto after_noop = ResourceLifecycleCapacityTaintSnapshot();
    CHECK(after_noop == after_prune);

    // Another real prune: generation advances, flag stays set.
    for (std::uint64_t i = 0; i < 8; ++i)
      ActivateResourceLifecycle(
          TraceLiveHandleKey{kDeviceB, 30000 + i}, Identity(i));
    CHECK(PruneResourceLifecycleTo(2) > 0);
    const auto after_second = ResourceLifecycleCapacityTaintSnapshot();
    CHECK(after_second.evidence_dropped);
    CHECK(after_second.prune_generation > after_prune.prune_generation);
  }

  return 0;
}

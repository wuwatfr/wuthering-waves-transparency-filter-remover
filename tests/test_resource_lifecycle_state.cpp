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

  return 0;
}

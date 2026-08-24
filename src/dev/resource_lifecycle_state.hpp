// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>

#include "trace_submission_identity.hpp"

namespace wuwa_tfr::dev {

struct TraceResourceIdentity {
  std::uint64_t fingerprint = 0;
  bool dynamic_contents = false;

  friend bool operator==(
      const TraceResourceIdentity&, const TraceResourceIdentity&) = default;
};

using ResourceLifecycleActivation =
    wuwa_tfr::TraceIncarnationIndex<TraceResourceIdentity>::Activation;

struct ActiveResourceLifecycle {
  std::uint64_t incarnation_id = 0;
  bool dynamic_contents = false;
};

// Canonical, device-scoped D3D12 resource-lifetime owner shared by Trace
// and Fade-control, so both observe the same incarnation id -- including
// the same handle-reused-without-destroy rotation detection -- for a given
// (device, resource handle). Self-locking and never held across a call
// back into caller code, so it can safely nest under either g_trace_mutex
// or g_fade_control_mutex without either of those two ever needing to
// acquire the other.
ResourceLifecycleActivation ActivateResourceLifecycle(
    wuwa_tfr::TraceLiveHandleKey key, const TraceResourceIdentity& identity);
void DestroyResourceLifecycle(wuwa_tfr::TraceLiveHandleKey key);
void DestroyResourceLifecycleForDevice(std::uintptr_t device);
ActiveResourceLifecycle FindActiveResourceLifecycle(
    wuwa_tfr::TraceLiveHandleKey key);
std::size_t PruneResourceLifecycleTo(std::size_t maximum_records);

}  // namespace wuwa_tfr::dev

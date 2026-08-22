// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "pipeline_replacement_coordinator.hpp"

#include "test_check.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace {

using Coordinator = wuwa_tfr::PipelineReplacementCoordinator<int, int>;
using InitResult = Coordinator::InitResult;

void WaitFor(const std::atomic<bool>& ready) {
  while (!ready.load(std::memory_order_acquire))
    std::this_thread::yield();
}

// This seam calls the same PipelineReplacementCoordinator methods used by
// FadePrimitiveRuntime::OnInitPipeline, OnBindPipeline,
// OnDestroyPipeline, and OnDestroyDevice. Integer pipelines simply make the
// callback ordering and exact destruction counts portable to CTest.
class RuntimeCoordinatorSeam {
 public:
  template <typename Build>
  InitResult OnInit(int device, std::uint64_t handle,
      std::uint64_t shader_identity, Build&& build) {
    return coordinator_.OnInit(device, handle, shader_identity,
        [this, build = std::forward<Build>(build)]() mutable {
          // Re-entering state here proves external creation is outside the
          // state mutex, as it is in FadePrimitiveRuntime.
          (void)coordinator_.Size();
          ++create_calls_;
          return build();
        }, [this](int pipeline) { DestroyExternal(pipeline); });
  }

  void OnDestroyPipeline(int device, std::uint64_t handle) {
    coordinator_.OnDestroyPipeline(device, handle,
        [this](int pipeline) { DestroyExternal(pipeline); });
  }

  void OnDestroyDevice(int device) {
    coordinator_.OnDestroyOwner(device,
        [this](int pipeline) { DestroyExternal(pipeline); });
  }

  bool OnBind(int device, std::uint64_t handle, int& selected) {
    return coordinator_.OnBind(device, handle,
        [&](int pipeline) { selected = pipeline; });
  }

  template <typename Bind>
  bool OnBindWith(int device, std::uint64_t handle, Bind&& bind) {
    return coordinator_.OnBind(device, handle, std::forward<Bind>(bind));
  }

  unsigned CreateCalls() const noexcept { return create_calls_; }
  unsigned DestroyCount(int pipeline) const {
    std::lock_guard lock(destroy_mutex_);
    const auto found = destroy_counts_.find(pipeline);
    return found == destroy_counts_.end() ? 0 : found->second;
  }
  std::size_t LiveCount() const { return coordinator_.Size(); }
  std::size_t RetainedCount() const { return coordinator_.RetainedSize(); }

 private:
  void DestroyExternal(int pipeline) {
    // Destruction is also external to the state mutex.
    (void)coordinator_.Size();
    std::lock_guard lock(destroy_mutex_);
    ++destroy_counts_[pipeline];
  }

  Coordinator coordinator_;
  unsigned create_calls_ = 0;
  mutable std::mutex destroy_mutex_;
  std::unordered_map<int, unsigned> destroy_counts_;
};

void RepeatedInitIsIdempotentAndPublishedLifetimeIsSafe() {
  RuntimeCoordinatorSeam runtime;
  CHECK(runtime.OnInit(1, 10, 0xA, [] { return std::optional<int>(100); }) ==
      InitResult::Published);
  int selected = 0;
  CHECK(runtime.OnBind(1, 10, selected));
  CHECK(selected == 100);

  // A repeat for the same live immutable D3D12 application pipeline is an
  // idempotent no-op: no create and no destroy occur.
  CHECK(runtime.OnInit(1, 10, 0xA, [] {
    CHECK(false);
    return std::optional<int>(101);
  }) == InitResult::AlreadyPublished);
  CHECK(runtime.CreateCalls() == 1);
  CHECK(runtime.DestroyCount(100) == 0);
  selected = 0;
  CHECK(runtime.OnBind(1, 10, selected));
  CHECK(selected == 100);

  // The replacement survives after bind returns and is released only at the
  // application pipeline's actual destroy event.
  runtime.OnDestroyPipeline(1, 10);
  CHECK(runtime.DestroyCount(100) == 1);
  CHECK(runtime.LiveCount() == 0);
}

void ContradictoryLiveInitFailsClosedAndRetiresPublishedReplacement() {
  RuntimeCoordinatorSeam runtime;
  CHECK(runtime.OnInit(1, 11, 0xB, [] { return std::optional<int>(110); }) ==
      InitResult::Published);
  CHECK(runtime.LiveCount() == 1);
  CHECK(runtime.RetainedCount() == 1);
  int selected = 0;
  CHECK(runtime.OnBind(1, 11, selected));
  CHECK(selected == 110);

  // A different observed shader identity cannot describe the same immutable
  // D3D12 PSO. It disables selection and retains, rather than destroys, the
  // published replacement until the real teardown boundary.
  CHECK(runtime.OnInit(1, 11, 0xC, [] {
    CHECK(false);
    return std::optional<int>(111);
  }) == InitResult::Rejected);
  CHECK(!runtime.OnBind(1, 11, selected));
  CHECK(runtime.CreateCalls() == 1);
  CHECK(runtime.DestroyCount(110) == 0);
  CHECK(runtime.LiveCount() == 0);
  CHECK(runtime.RetainedCount() == 1);
  runtime.OnDestroyPipeline(1, 11);
  CHECK(runtime.DestroyCount(110) == 1);
  CHECK(runtime.LiveCount() == 0);
  CHECK(runtime.RetainedCount() == 0);

  // Owner drain is the other valid teardown boundary for a retired published
  // replacement and must likewise bring the retained telemetry gauge to zero.
  CHECK(runtime.OnInit(1, 12, 0xD, [] { return std::optional<int>(120); }) ==
      InitResult::Published);
  CHECK(runtime.RetainedCount() == 1);
  CHECK(runtime.OnInit(1, 12, 0xE, [] {
    CHECK(false);
    return std::optional<int>(121);
  }) == InitResult::Rejected);
  CHECK(runtime.LiveCount() == 0);
  CHECK(runtime.RetainedCount() == 1);
  runtime.OnDestroyDevice(1);
  CHECK(runtime.DestroyCount(120) == 1);
  CHECK(runtime.LiveCount() == 0);
  CHECK(runtime.RetainedCount() == 0);
}

void ConcurrentFirstInitDestroyAndHandleReuseAreGenerationSafe() {
  RuntimeCoordinatorSeam runtime;
  std::atomic<bool> old_build_entered{false};
  std::atomic<bool> release_old_build{false};
  std::atomic<int> old_result{static_cast<int>(InitResult::Published)};
  std::thread old([&] {
    old_result.store(static_cast<int>(runtime.OnInit(1, 12, 0xD, [&] {
      old_build_entered.store(true, std::memory_order_release);
      WaitFor(release_old_build);
      return std::optional<int>(120);
    })), std::memory_order_release);
  });
  WaitFor(old_build_entered);

  // Same first-generation init is single-flight and cannot create a second
  // candidate while the first preparation is outstanding.
  CHECK(runtime.OnInit(1, 12, 0xD, [] {
    CHECK(false);
    return std::optional<int>(121);
  }) == InitResult::InFlight);

  // Actual destruction invalidates the old ticket. Handle reuse starts a
  // fresh generation which completes before the old candidate (reverse
  // completion); that stale, never-published candidate is destroyed at once.
  runtime.OnDestroyPipeline(1, 12);
  CHECK(runtime.OnInit(1, 12, 0xE, [] { return std::optional<int>(121); }) ==
      InitResult::Published);
  release_old_build.store(true, std::memory_order_release);
  old.join();
  CHECK(static_cast<InitResult>(old_result.load(std::memory_order_acquire)) ==
      InitResult::Rejected);
  CHECK(runtime.DestroyCount(120) == 1);
  int selected = 0;
  CHECK(runtime.OnBind(1, 12, selected));
  CHECK(selected == 121);
  runtime.OnDestroyPipeline(1, 12);
  CHECK(runtime.DestroyCount(121) == 1);
}

void DeviceScopeAndCpuBindConcurrencyAreSafe() {
  RuntimeCoordinatorSeam runtime;
  CHECK(runtime.OnInit(1, 14, 0x14, [] { return std::optional<int>(140); }) ==
      InitResult::Published);
  CHECK(runtime.OnInit(2, 14, 0x24, [] { return std::optional<int>(240); }) ==
      InitResult::Published);
  runtime.OnDestroyDevice(1);
  int selected = 0;
  CHECK(!runtime.OnBind(1, 14, selected));
  CHECK(runtime.OnBind(2, 14, selected));
  CHECK(selected == 240);
  CHECK(runtime.DestroyCount(140) == 1);

  CHECK(runtime.OnInit(2, 15, 0x25, [] { return std::optional<int>(250); }) ==
      InitResult::Published);
  std::atomic<bool> bind_entered{false};
  std::atomic<bool> allow_bind_return{false};
  std::atomic<bool> destroy_started{false};
  std::thread binder([&] {
    CHECK(runtime.OnBindWith(2, 15, [&](int pipeline) {
      CHECK(pipeline == 250);
      // Re-entry proves no state lock covers the external bind callback.
      CHECK(runtime.LiveCount() >= 2);
      bind_entered.store(true, std::memory_order_release);
      WaitFor(allow_bind_return);
    }));
  });
  WaitFor(bind_entered);
  std::thread destroyer([&] {
    destroy_started.store(true, std::memory_order_release);
    runtime.OnDestroyPipeline(2, 15);
  });
  WaitFor(destroy_started);
  CHECK(runtime.DestroyCount(250) == 0);
  allow_bind_return.store(true, std::memory_order_release);
  binder.join();
  destroyer.join();
  CHECK(runtime.DestroyCount(250) == 1);

  // Owner drain releases the remaining published replacement exactly once.
  runtime.OnDestroyDevice(2);
  runtime.OnDestroyDevice(2);
  CHECK(runtime.DestroyCount(240) == 1);
  CHECK(runtime.LiveCount() == 0);
}

void FailureAndOwnerDrainInvalidateInflightCandidates() {
  RuntimeCoordinatorSeam runtime;
  CHECK(runtime.OnInit(1, 16, 0x16, [] { return std::optional<int>(); }) ==
      InitResult::Rejected);
  int selected = 0;
  CHECK(!runtime.OnBind(1, 16, selected));
  CHECK(runtime.LiveCount() == 0);

  std::atomic<bool> build_entered{false};
  std::atomic<bool> release_build{false};
  std::thread init([&] {
    CHECK(runtime.OnInit(3, 17, 0x17, [&] {
      build_entered.store(true, std::memory_order_release);
      WaitFor(release_build);
      return std::optional<int>(370);
    }) == InitResult::Rejected);
  });
  WaitFor(build_entered);
  runtime.OnDestroyDevice(3);
  release_build.store(true, std::memory_order_release);
  init.join();
  CHECK(!runtime.OnBind(3, 17, selected));
  CHECK(runtime.DestroyCount(370) == 1);
  CHECK(runtime.LiveCount() == 0);
}

} // namespace

int main() {
  RepeatedInitIsIdempotentAndPublishedLifetimeIsSafe();
  ContradictoryLiveInitFailsClosedAndRetiresPublishedReplacement();
  ConcurrentFirstInitDestroyAndHandleReuseAreGenerationSafe();
  DeviceScopeAndCpuBindConcurrencyAreSafe();
  FailureAndOwnerDrainInvalidateInflightCandidates();
  return 0;
}

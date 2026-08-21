// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace wuwa_tfr {

constexpr std::uint64_t kMemoryTelemetryIntervalSeconds = 10;
constexpr std::uint64_t kMemoryTelemetryWarningIntervalSamples =
    600 / kMemoryTelemetryIntervalSeconds;

struct MemoryTelemetryTicket {
  std::uint64_t session = 0;
  std::uint64_t sample = 0;
  std::uint64_t elapsed_s = 0;
  bool schema_start = false;
};

struct MemoryTelemetrySnapshot {
  std::uint64_t working_set_bytes = 0;
  std::uint64_t private_commit_bytes = 0;
  std::uint64_t handle_count = 0;
  std::uint64_t shader_cache_entries = 0;
  std::uint64_t shader_cache_bytecode_bytes = 0;
  std::uint64_t preparations_in_flight = 0;
  std::uint64_t live_replacement_pipelines = 0;
  std::uint64_t active_devices = 0;
  std::uint64_t matched_shaders_total = 0;
  std::uint64_t prepared_shaders_total = 0;
  std::uint64_t replacements_created_total = 0;
  std::uint64_t replacements_failed_total = 0;
  std::uint64_t replacement_binds_total = 0;
};

inline std::string FormatMemoryTelemetryStart(
    const MemoryTelemetryTicket& ticket) {
  return "schema=1 session=" + std::to_string(ticket.session) +
      " interval_s=" + std::to_string(kMemoryTelemetryIntervalSeconds) +
      " sample=" + std::to_string(ticket.sample) +
      " elapsed_s=" + std::to_string(ticket.elapsed_s);
}

inline std::string FormatMemoryTelemetrySample(
    const MemoryTelemetryTicket& ticket,
    const MemoryTelemetrySnapshot& snapshot) {
  return "session=" + std::to_string(ticket.session) +
      " sample=" + std::to_string(ticket.sample) +
      " elapsed_s=" + std::to_string(ticket.elapsed_s) +
      " working_set_bytes=" + std::to_string(snapshot.working_set_bytes) +
      " private_commit_bytes=" + std::to_string(snapshot.private_commit_bytes) +
      " handle_count=" + std::to_string(snapshot.handle_count) +
      " shader_cache_entries=" + std::to_string(snapshot.shader_cache_entries) +
      " shader_cache_bytecode_bytes=" +
          std::to_string(snapshot.shader_cache_bytecode_bytes) +
      " preparations_in_flight=" +
          std::to_string(snapshot.preparations_in_flight) +
      " live_replacement_pipelines=" +
          std::to_string(snapshot.live_replacement_pipelines) +
      " active_devices=" + std::to_string(snapshot.active_devices) +
      " matched_shaders_total=" +
          std::to_string(snapshot.matched_shaders_total) +
      " prepared_shaders_total=" +
          std::to_string(snapshot.prepared_shaders_total) +
      " replacements_created_total=" +
          std::to_string(snapshot.replacements_created_total) +
      " replacements_failed_total=" +
          std::to_string(snapshot.replacements_failed_total) +
      " replacement_binds_total=" +
          std::to_string(snapshot.replacement_binds_total);
}

// The ReShade present callback uses this around every enabled-path operation.
// Its catch path deliberately allocates and logs nothing, so telemetry cannot
// propagate memory-pressure exceptions across the host callback boundary.
template <typename Work>
bool InvokeMemoryTelemetryNoThrow(Work&& work) noexcept {
  try {
    std::forward<Work>(work)();
    return true;
  } catch (...) {
    return false;
  }
}

// Process-local, session-only scheduling state. The caller must check enabled()
// before reading a clock; TryAcquireSample takes no lock unless a deadline is
// due. EmitIfCurrent lets a caller suppress a prepared log line if the user
// disabled telemetry while the scheduled process/runtime queries were running.
class MemoryTelemetryController {
 public:
  bool enabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
  }

  void SetEnabled(bool enabled) {
    if (!enabled) {
      // This store is deliberately before the lock: present callbacks stop at
      // their first enabled-state check without waiting for UI work.
      enabled_.store(false, std::memory_order_release);
      std::lock_guard lock(state_mutex_);
      if (!enabled_.load(std::memory_order_acquire)) ResetScheduleLocked();
      return;
    }

    std::lock_guard lock(state_mutex_);
    if (enabled_.load(std::memory_order_acquire)) return;
    ResetScheduleLocked();
    active_session_.store(
        session_counter_.fetch_add(1, std::memory_order_relaxed) + 1,
        std::memory_order_release);
    enabled_.store(true, std::memory_order_release);
  }

  std::optional<MemoryTelemetryTicket> TryAcquireSample(
      std::uint64_t monotonic_seconds) {
    const auto observed_session = ObserveDueSession(monotonic_seconds);
    if (!observed_session) return std::nullopt;
    return ClaimObservedDueSample(monotonic_seconds, *observed_session);
  }

  // This split makes the session boundary deterministic to test. Production
  // uses the two steps back-to-back; a caller that observed an older session
  // cannot claim or block a newer session in ClaimObservedDueSample().
  std::optional<std::uint64_t> ObserveDueSession(
      std::uint64_t monotonic_seconds) const noexcept {
    if (!enabled_.load(std::memory_order_acquire)) return std::nullopt;
    const std::uint64_t deadline =
        next_deadline_s_.load(std::memory_order_acquire);
    if (deadline != 0 && monotonic_seconds < deadline) return std::nullopt;
    return active_session_.load(std::memory_order_acquire);
  }

  std::optional<MemoryTelemetryTicket> ClaimObservedDueSample(
      std::uint64_t monotonic_seconds,
      std::uint64_t observed_session) {
    std::lock_guard lock(state_mutex_);
    if (!enabled_.load(std::memory_order_acquire) ||
        active_session_.load(std::memory_order_acquire) != observed_session)
      return std::nullopt;

    const std::uint64_t deadline =
        next_deadline_s_.load(std::memory_order_acquire);
    if (deadline != 0 && monotonic_seconds < deadline) return std::nullopt;

    const bool first_sample = !started_;
    if (first_sample) {
      started_ = true;
      session_start_s_ = monotonic_seconds;
      next_sample_ = 0;
    }
    const MemoryTelemetryTicket ticket{
        observed_session, next_sample_++, monotonic_seconds - session_start_s_,
        first_sample};
    const std::uint64_t next =
        monotonic_seconds > std::numeric_limits<std::uint64_t>::max() -
                kMemoryTelemetryIntervalSeconds
        ? std::numeric_limits<std::uint64_t>::max()
        : monotonic_seconds + kMemoryTelemetryIntervalSeconds;
    next_deadline_s_.store(next, std::memory_order_release);
    return ticket;
  }

  bool IsCurrent(const MemoryTelemetryTicket& ticket) const noexcept {
    return enabled_.load(std::memory_order_acquire) &&
        active_session_.load(std::memory_order_acquire) == ticket.session;
  }

  template <typename Emit>
  bool EmitIfCurrent(const MemoryTelemetryTicket& ticket, Emit&& emit) {
    std::lock_guard lock(state_mutex_);
    if (!enabled_.load(std::memory_order_acquire) ||
        active_session_.load(std::memory_order_acquire) != ticket.session)
      return false;
    std::forward<Emit>(emit)();
    return true;
  }

 private:
  void ResetScheduleLocked() {
    started_ = false;
    session_start_s_ = 0;
    next_sample_ = 0;
    next_deadline_s_.store(0, std::memory_order_release);
  }

  std::atomic<bool> enabled_{false};
  std::atomic<std::uint64_t> session_counter_{0};
  std::atomic<std::uint64_t> active_session_{0};
  std::atomic<std::uint64_t> next_deadline_s_{0};
  std::mutex state_mutex_;
  bool started_ = false;
  std::uint64_t session_start_s_ = 0;
  std::uint64_t next_sample_ = 0;
};

} // namespace wuwa_tfr

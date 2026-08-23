// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "memory_telemetry.hpp"
#include "test_check.hpp"

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void DisabledByDefaultProducesNoTicketOrLine() {
  wuwa_tfr::MemoryTelemetryController telemetry;
  std::vector<std::string> lines;
  CHECK(!telemetry.enabled());
  CHECK(!telemetry.TryAcquireSample(100));
  CHECK(lines.empty());
}

void EnableStartsImmediatelyAndUsesFixedCadence() {
  CHECK(1 + (5 * 60 * 60) / wuwa_tfr::kMemoryTelemetryIntervalSeconds == 1801);
  wuwa_tfr::MemoryTelemetryController telemetry;
  telemetry.SetEnabled(true);
  const auto first = telemetry.TryAcquireSample(100);
  CHECK(first.has_value());
  CHECK(first->session == 1);
  CHECK(first->sample == 0);
  CHECK(first->elapsed_s == 0);
  CHECK(first->schema_start);
  CHECK(!telemetry.TryAcquireSample(109));
  const auto second = telemetry.TryAcquireSample(110);
  CHECK(second.has_value());
  CHECK(second->session == first->session);
  CHECK(second->sample == 1);
  CHECK(second->elapsed_s == 10);
  CHECK(!second->schema_start);
}

void ConcurrentDeadlineClaimsOnlyOneSample() {
  wuwa_tfr::MemoryTelemetryController telemetry;
  telemetry.SetEnabled(true);
  constexpr std::size_t kCallers = 16;
  std::array<std::thread, kCallers> callers;
  std::atomic<std::size_t> samples{0};
  for (auto& caller : callers) {
    caller = std::thread([&] {
      if (telemetry.TryAcquireSample(100))
        samples.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto& caller : callers) caller.join();
  CHECK(samples.load(std::memory_order_relaxed) == 1);
}

void DisableSuppressesAndReenableStartsNewSession() {
  wuwa_tfr::MemoryTelemetryController telemetry;
  telemetry.SetEnabled(true);
  const auto first = telemetry.TryAcquireSample(100);
  CHECK(first.has_value());
  telemetry.SetEnabled(false);
  CHECK(!telemetry.enabled());
  CHECK(!telemetry.TryAcquireSample(110));
  bool emitted = false;
  CHECK(!telemetry.EmitIfCurrent(*first, [&] { emitted = true; }));
  CHECK(!emitted);

  telemetry.SetEnabled(true);
  const auto restarted = telemetry.TryAcquireSample(700);
  CHECK(restarted.has_value());
  CHECK(restarted->session == first->session + 1);
  CHECK(restarted->sample == 0);
  CHECK(restarted->elapsed_s == 0);
  CHECK(restarted->schema_start);
}

void StaleObservedSessionCannotBlockReenabledSession() {
  wuwa_tfr::MemoryTelemetryController telemetry;
  telemetry.SetEnabled(true);
  const auto stale_session = telemetry.ObserveDueSession(100);
  CHECK(stale_session.has_value());
  CHECK(*stale_session == 1);

  telemetry.SetEnabled(false);
  telemetry.SetEnabled(true);
  // This directly models a callback that observed session 1 and was paused
  // across disable/re-enable. It must not claim the new zero deadline.
  CHECK(!telemetry.ClaimObservedDueSample(100, *stale_session));
  const auto first_new_session = telemetry.TryAcquireSample(200);
  CHECK(first_new_session.has_value());
  CHECK(first_new_session->session == 2);
  CHECK(first_new_session->sample == 0);
  CHECK(first_new_session->elapsed_s == 0);
  const auto later = telemetry.TryAcquireSample(210);
  CHECK(later.has_value());
  CHECK(later->session == 2);
  CHECK(later->sample == 1);
}

void NoThrowBoundaryKeepsControllerUsable() {
  wuwa_tfr::MemoryTelemetryController telemetry;
  telemetry.SetEnabled(true);
  const auto first = telemetry.TryAcquireSample(100);
  CHECK(first.has_value());
  CHECK(!wuwa_tfr::InvokeMemoryTelemetryNoThrow([&] {
    telemetry.EmitIfCurrent(*first, [] {
      throw std::runtime_error("deliberate telemetry failure");
    });
  }));
  const auto later = telemetry.TryAcquireSample(110);
  CHECK(later.has_value());
  CHECK(later->sample == 1);
}

void FormattingIsStableAndMachineParseable() {
  const wuwa_tfr::MemoryTelemetryTicket ticket{9, 42, 1230, false};
  const wuwa_tfr::MemoryTelemetrySnapshot snapshot{
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  CHECK(wuwa_tfr::FormatMemoryTelemetryStart(
      wuwa_tfr::MemoryTelemetryTicket{9, 0, 0, true}) ==
      "schema=1 session=9 interval_s=10 sample=0 elapsed_s=0");
  CHECK(wuwa_tfr::FormatMemoryTelemetrySample(ticket, snapshot) ==
      "session=9 sample=42 elapsed_s=1230 working_set_bytes=1 "
      "private_commit_bytes=2 handle_count=3 shader_cache_entries=4 "
      "shader_cache_bytecode_bytes=5 preparations_in_flight=6 "
      "live_replacement_pipelines=7 active_devices=8 "
      "matched_shaders_total=9 prepared_shaders_total=10 "
      "replacements_created_total=11 replacements_failed_total=12 "
      "replacement_binds_total=13");
}

void TelemetryControlIsNotPersisted(int argc, char** argv) {
  CHECK(argc == 3);
  // argv[1] is production_module.cpp: the Production variant module that
  // owns this checkbox. Unlike the old combined addon.cpp, there is no
  // #if/#endif to bound the scan, so this checks everything from the
  // checkbox's own line to the end of the (short, single-purpose) file.
  std::ifstream addon_input(argv[1], std::ios::binary);
  const std::string addon(std::istreambuf_iterator<char>(addon_input), {});
  std::ifstream ini_input(argv[2], std::ios::binary);
  const std::string ini(std::istreambuf_iterator<char>(ini_input), {});
  const std::size_t control = addon.find("Log memory telemetry (10 s)");
  CHECK(control != std::string::npos);
  const std::string_view overlay = std::string_view(addon).substr(control);
  CHECK(overlay.find("SaveConfigFlag") == std::string_view::npos);
  CHECK(overlay.find("ConfigFlag") == std::string_view::npos);
  CHECK(ini.find("Telemetry") == std::string::npos);
}

} // namespace

int main(int argc, char** argv) {
  DisabledByDefaultProducesNoTicketOrLine();
  EnableStartsImmediatelyAndUsesFixedCadence();
  ConcurrentDeadlineClaimsOnlyOneSample();
  DisableSuppressesAndReenableStartsNewSession();
  StaleObservedSessionCannotBlockReenabledSession();
  NoThrowBoundaryKeepsControllerUsable();
  FormattingIsStableAndMachineParseable();
  TelemetryControlIsNotPersisted(argc, argv);
  return 0;
}

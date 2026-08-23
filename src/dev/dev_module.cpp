// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The Dev variant implementation of addon_variant.hpp. Owns Dev's own
// startup config, event registration, and overlay content; addon.cpp knows
// nothing about any of it beyond calling these five functions.

#include "addon_variant.hpp"

#include <imgui.h>
#include <reshade.hpp>

#include <string>

#include "addon_shared.hpp"
#include "dev/capture/manual_capture.hpp"
#include "dev/dev_events.hpp"
#include "dev/dev_inspection.hpp"
#include "dev/dev_overlay.hpp"
#include "dev/dev_runtime.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"

extern "C" __declspec(dllexport) const char* NAME =
    "WuwaTFR Dev (diagnostics build)";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Developer diagnostics build: runs the same replacement runtime as "
    "Production plus read-only trace/capture tools.";

namespace wuwa_tfr::variant {

void InitializeVariant() {
  dev::InitializeInspectionConfig();
  dev::InitializeDevRuntime();
}

void RegisterVariantEvents() {
  reshade::register_event<reshade::addon_event::create_pipeline>(
      dev::OnCreatePipeline);
  dev::RegisterDevEvents();
  dev::RegisterManualCaptureEvents();
}

void DrawVariantOverlay() {
  ImGui::TextUnformatted("Dev: FadePrimitiveRuntime + diagnostics/trace tools");
  ImGui::TextDisabled(
      "Dev runs its own instance of the same replacement runtime as Production, "
      "plus read-only diagnostics and trace/capture tools.");

  ImGui::Separator();
  ImGui::Text("DXIL callbacks: %llu",
      static_cast<unsigned long long>(
          dev::g_seen_shader_callbacks.load(std::memory_order_relaxed)));
  ImGui::Text("Unique DXIL shaders: %llu",
      static_cast<unsigned long long>(
          dev::g_unique_dxil_shaders.load(std::memory_order_relaxed)));
  ImGui::Text("Disassembly: success=%llu failure=%llu",
      static_cast<unsigned long long>(
          dev::g_disassembly_successes.load(std::memory_order_relaxed)),
      static_cast<unsigned long long>(
          dev::g_disassembly_failures.load(std::memory_order_relaxed)));
  ImGui::Text("Original IR dumps: %llu",
      static_cast<unsigned long long>(
          dev::g_dumped_shaders.load(std::memory_order_relaxed)));

  ImGui::Separator();
  dev::DrawFadePrimitiveTargetModes();
  ImGui::Separator();
  dev::DrawTraceOverlay();
  ImGui::Separator();
  dev::DrawManualCaptureOverlay();
}

void LogVariantStartup() {
  Log(reshade::log::level::info,
      std::string("loaded Dev research build") +
      (dev::g_diagnostic ? "; diagnostic=on" : "; diagnostic=off") +
      (dev::g_dump ? "; dump=all-unique-dxil" : "; dump=off") +
      (dev::g_dump ? "; dump-path=" + dev::g_dump_path.string() : "") +
      "; devtools=compiled" +
      "; config=" + ConfigPath().string());
}

void OnLastDeviceDestroyed() {
  dev::TeardownInspectionOnLastDevice();
}

}  // namespace wuwa_tfr::variant

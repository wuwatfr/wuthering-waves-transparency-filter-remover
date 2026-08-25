// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#ifdef _WIN32
#include <Windows.h>
#include <imgui.h>
#include <reshade.hpp>

#include "addon_variant.hpp"
#include "device_activity_state.hpp"
#include "wuwa_process.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "addon_shared.hpp"

using namespace reshade::api;

namespace {

bool ResolveAddonDirectory(HMODULE module) {
  wchar_t module_path[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(
      module, module_path, static_cast<DWORD>(std::size(module_path)));
  if (length == 0 || length >= std::size(module_path)) return false;

  const auto directory =
      std::filesystem::path(module_path, module_path + length).parent_path();
  if (directory.empty()) return false;
  wuwa_tfr::g_addon_directory = directory;
  return true;
}

bool IsWuwaProcess() {
  std::array<wchar_t, 32768> executable_path{};
  const DWORD length = GetModuleFileNameW(
      nullptr, executable_path.data(),
      static_cast<DWORD>(executable_path.size()));
  if (length == 0 || length >= executable_path.size()) return false;
  return wuwa_tfr::IsWuwaExecutable(
      std::filesystem::path(executable_path.data(),
                            executable_path.data() + length));
}

void DrawOverlay(effect_runtime*) {
  ImGui::TextUnformatted("WuwaTFR " WUWA_TFR_VERSION);
  ImGui::TextDisabled("Build: %s", WUWA_TFR_BUILD_COMMIT);
  wuwa_tfr::variant::DrawVariantOverlay();
}

} // namespace

namespace wuwa_tfr {

DeviceActivityState<DeviceIdentity> g_device_activity;
bool g_target_process = false;
std::filesystem::path g_addon_directory;

void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("[WuwaTFR] " + message).c_str());
}

std::filesystem::path ConfigPath() {
  if (g_addon_directory.empty()) return {};
  return g_addon_directory / L"WuwaTFR.ini";
}

bool ConfigFlag(const wchar_t* key, bool fallback) {
  const auto path = ConfigPath();
  if (path.empty() ||
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return fallback;
  return GetPrivateProfileIntW(
      L"General", key, fallback ? 1 : 0, path.c_str()) != 0;
}

void SaveConfigFlag(const wchar_t* key, bool value) {
  const auto path = ConfigPath();
  if (path.empty()) return;
  WritePrivateProfileStringW(
      L"General", key, value ? L"1" : L"0", path.c_str());
}

std::filesystem::path ConfigPathValue(const wchar_t* key) {
  const auto config_path = ConfigPath();
  if (config_path.empty() ||
      GetFileAttributesW(config_path.c_str()) == INVALID_FILE_ATTRIBUTES)
    return {};

  constexpr DWORD kBufferChars = 32768;
  std::vector<wchar_t> raw(kBufferChars);
  const DWORD raw_length = GetPrivateProfileStringW(
      L"General", key, L"", raw.data(), kBufferChars,
      config_path.c_str());
  if (raw_length == 0 || raw_length >= kBufferChars - 1) return {};

  std::vector<wchar_t> expanded(kBufferChars);
  const DWORD expanded_length = ExpandEnvironmentStringsW(
      raw.data(), expanded.data(), kBufferChars);
  if (expanded_length == 0 || expanded_length > kBufferChars) return {};

  std::filesystem::path result(expanded.data());
  return result.is_absolute() ? result : std::filesystem::path{};
}

static std::atomic<std::uint32_t> g_d3d12_device_count{0};

void OnInitDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;
  if (g_device_activity.Activate(DeviceKey(owner)))
    g_d3d12_device_count.fetch_add(1, std::memory_order_relaxed);
}

void OnDestroyDevice(device* owner) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12)
    return;

  auto teardown = g_device_activity.Deactivate(DeviceKey(owner));
  if (!teardown) return;

  std::uint32_t previous = g_d3d12_device_count.load(std::memory_order_acquire);
  while (previous != 0 &&
         !g_d3d12_device_count.compare_exchange_weak(
             previous, previous - 1,
             std::memory_order_acq_rel, std::memory_order_acquire)) {
  }
  if (previous != 1) return;
  variant::OnLastDeviceDestroyed();
}

}  // namespace wuwa_tfr

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH: {
      if (!ResolveAddonDirectory(module)) return FALSE;
      wuwa_tfr::g_target_process = IsWuwaProcess();

      wuwa_tfr::variant::InitializeVariant();

      if (!reshade::register_addon(module)) return FALSE;
      reshade::register_event<reshade::addon_event::init_device>(
          wuwa_tfr::OnInitDevice);
      reshade::register_event<reshade::addon_event::destroy_device>(
          wuwa_tfr::OnDestroyDevice);
      wuwa_tfr::variant::RegisterVariantEvents();
      reshade::register_overlay("WuwaTFR", DrawOverlay);

      if (wuwa_tfr::g_target_process) wuwa_tfr::variant::LogVariantStartup();
      break;
    }
    case DLL_PROCESS_DETACH:
      if (reserved == nullptr) reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
#endif

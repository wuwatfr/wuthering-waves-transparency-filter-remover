// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The compile-time-selected boundary between addon.cpp's generic bootstrap
// and the Production/Dev variant linked into a given target. CMake compiles
// exactly one of production/production_module.cpp or dev/dev_module.cpp per
// target (see add_wuwa_addon in CMakeLists.txt); addon.cpp calls these
// functions without an #if, and without knowing which one it got.

#pragma once

namespace wuwa_tfr::variant {

// Called once, after g_target_process/g_addon_directory are resolved but
// before reshade::register_addon(). Reads the variant's own config keys and
// prepares any state that must exist before ReShade can dispatch events.
void InitializeVariant();

// Called once, after reshade::register_addon() and after the generic
// init_device/destroy_device handlers are registered. Registers every other
// ReShade event handler the variant needs, including its own device and
// pipeline lifecycle. The variant owns its complete event set beyond the
// generic device-activity tracking.
void RegisterVariantEvents();

// Draws the variant-specific body of the "WuwaTFR" overlay panel, below the
// shared title/build-commit header addon.cpp itself draws.
void DrawVariantOverlay();

// Logs the variant-specific startup line, only when g_target_process is
// true. Called once, after every registration above has completed.
void LogVariantStartup();

// Called when the generic bootstrap detects that the last active D3D12
// device has just been destroyed. Neither variant currently owns any
// last-device-destroyed teardown state.
void OnLastDeviceDestroyed();

}  // namespace wuwa_tfr::variant

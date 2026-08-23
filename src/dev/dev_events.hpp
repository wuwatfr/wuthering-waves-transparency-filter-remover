// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev's ReShade event registration, called from dev/dev_module.cpp's
// RegisterVariantEvents(); addon.cpp itself never references this file.

#pragma once

#include <reshade.hpp>

namespace wuwa_tfr::dev {

// Registers every Dev-only reshade::register_event<...> handler except
// create_pipeline (registered directly by dev/dev_module.cpp's
// RegisterVariantEvents(), which calls this).
void RegisterDevEvents();

// The Dev-only tail of OnDestroyDevice: tears down this device's trace
// incarnation/pipeline state. (Fade-primitive replacement lifecycle is owned
// entirely by g_dev_antifade_runtime -- see dev/dev_runtime.hpp -- which is
// torn down independently via its own init_device/destroy_device handlers.)
void OnDestroyDeviceHook(reshade::api::device* owner);

}  // namespace wuwa_tfr::dev

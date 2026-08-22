// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Entry points addon.cpp calls into for the handful of genuinely-shared
// functions (OnInitDevice, OnDestroyDevice, DllMain) that must stay in
// addon.cpp because they also run always-compiled, Production-relevant code,
// but that also need to run a Dev-only step.

#pragma once

#include <reshade.hpp>

namespace wuwa_tfr::dev {

// Registers every Dev-only reshade::register_event<...> handler. Called from
// DllMain only when WUWA_TFR_DEVTOOLS is set.
void RegisterDevEvents();

// The Dev-only tail of OnInitDevice: records the device so the pipeline-
// replacement experiments can look it up later.
void OnInitDeviceHook(reshade::api::device* owner);

// The Dev-only tail of OnDestroyDevice: tears down this device's trace state
// and drains both pipeline-replacement experiments' replacement pipelines.
void OnDestroyDeviceHook(reshade::api::device* owner);

}  // namespace wuwa_tfr::dev

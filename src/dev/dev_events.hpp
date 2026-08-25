// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <reshade.hpp>

namespace wuwa_tfr::dev {

void RegisterDevEvents();

void OnDestroyDeviceHook(reshade::api::device* owner);

}  // namespace wuwa_tfr::dev

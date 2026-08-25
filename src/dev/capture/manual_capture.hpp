// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <reshade.hpp>

#include "dev/capture/manual_capture_state.hpp"

namespace wuwa_tfr::dev {

void RegisterManualCaptureEvents();

void StartManualCapture();

bool StopAndExportManualCapture();

void ClearManualCapture();

void DrawManualCaptureOverlay();

}  // namespace wuwa_tfr::dev

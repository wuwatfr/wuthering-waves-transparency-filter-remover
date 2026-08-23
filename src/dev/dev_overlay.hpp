// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The two Dev-only ImGui overlay panels. addon.cpp's shared DrawOverlay
// calls these; it contains no panel bodies of its own.

#pragma once

namespace wuwa_tfr::dev {

// Panel for g_dev_antifade_runtime (dev/dev_runtime.hpp): its enable toggle,
// telemetry snapshot, and read-only Fade Primitive v1 diagnostics.
void DrawFadePrimitiveTargetModes();

// Panel for the runtime differential trace, manual/shader-family SKIP, and
// TSV report export.
void DrawTraceOverlay();

}  // namespace wuwa_tfr::dev

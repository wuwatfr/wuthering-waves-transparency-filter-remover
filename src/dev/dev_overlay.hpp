// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// The two Dev-only ImGui overlay panels. addon.cpp's shared DrawOverlay
// calls these; it contains no panel bodies of its own.

#pragma once

namespace wuwa_tfr::dev {

// Panel for the live Fade Primitive execution-set experiment (subsystem 5).
void DrawFadePrimitiveTargetModes();

// Panel for the runtime differential trace, manual/shader-family SKIP, and
// TSV report export (subsystems 2/3/6/7).
void DrawTraceOverlay();

}  // namespace wuwa_tfr::dev

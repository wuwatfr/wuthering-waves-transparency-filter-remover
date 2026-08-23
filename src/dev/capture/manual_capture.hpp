// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev-only manual execution capture: an independent Start/Stop/Clear
// lifecycle and TSV export, layered on top of the existing runtime
// differential trace's own command-list observation
// (dev/trace/trace_state.hpp's CommandListTrace::recorded_draws). See
// dev/capture/manual_capture_state.hpp for the pure accumulation state this
// drives.
//
// Coexists with, and never mutates, the three-window (normal/partial-fade/
// full-fade) differential trace: it reads the same per-command-list
// CommandListTrace private data and reuses the same g_trace_mutex, but has
// its own session lifecycle (independent of g_trace_token) and its own
// Present-selection policy (independent of g_trace_swapchain/
// g_trace_frame_id) -- the swapchain is locked in to the first Present
// observed after Start(), mirroring the differential trace's own
// normal-window swapchain selection, and every other swapchain's Presents
// are ignored for the remainder of the session.
//
// Registered only into the Dev target (dev/dev_module.cpp); Production has
// no dependency on this directory.

#pragma once

#include <reshade.hpp>

#include "dev/capture/manual_capture_state.hpp"

namespace wuwa_tfr::dev {

// Registers this module's execute_command_list/present handlers. Does not
// register any draw handler: membership is decided solely at command-list
// submission time, reusing the differential trace's own recorded_draws.
void RegisterManualCaptureEvents();

void StartManualCapture();

// Stops accepting new observations, freezes the session, and writes a
// timestamped TSV under DumpPath outside the trace lock. Returns false if
// no session was active or the export failed (e.g. DumpPath unavailable).
bool StopAndExportManualCapture();

// Clears only the frozen manual-capture result. No-op while Capturing.
void ClearManualCapture();

// Small standalone Dev panel: Start/Stop+export/Clear plus state, session,
// Present, and record counters.
void DrawManualCaptureOverlay();

}  // namespace wuwa_tfr::dev

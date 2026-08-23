// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Dev's own instance of the shared Production runtime
// (fade_primitive_runtime.cpp/.hpp, compiled unmodified into both targets).
// g_dev_antifade_runtime is a second, independent object of the exact same
// class as Production's g_public_antifade_runtime: its own shader cache, DXC
// pool, and replacement-pipeline state, with no dependency on Production's
// globals. The functions below only forward arguments into it and gate on
// g_target_process; all matching, preparation, and replacement-lifecycle
// logic lives in the shared runtime class itself, not here.

#pragma once

#include <reshade.hpp>

#include "fade_primitive_runtime.hpp"

namespace wuwa_tfr::dev {

extern wuwa_tfr::FadePrimitiveRuntime g_dev_antifade_runtime;

// A second, independent instance of the exact same FadePrimitiveRuntime
// class, with PatchAllVerifiedFadePrimitiveInstancesToIdentity replaced by
// PatchPreFadeFMinOperandOneHypothesis (dev/dev_prefade_fmin_hypothesis.hpp)
// -- an experimental, investigation-only structural probe, never the
// Production identity-phi collapse. Its own shader cache, DXC pool, and
// replacement-pipeline state are entirely separate from
// g_dev_antifade_runtime's. Enabling it is mutually exclusive with
// g_dev_antifade_runtime at the overlay level (dev/dev_overlay.cpp): both
// independently match and prepare every observed shader regardless of
// enabled(), but only one may ever be enabled() -- and therefore bound -- at
// a time, or the two would race to bind competing replacements for the same
// application pipeline.
extern wuwa_tfr::FadePrimitiveRuntime g_dev_prefade_hypothesis_runtime;

// True for the duration of OnDestroyDevRuntimeDevice/OnInitDevRuntimePipeline/
// OnDestroyDevRuntimePipeline/OnBindDevRuntimePipeline's calls into
// g_dev_antifade_runtime, including any ReShade event nested inside them (the
// runtime's own internal create/destroy/bind of a replacement pipeline, and
// OnDestroyDevice's internal destroy_pipeline while draining replacements on
// device teardown). OnInitDevRuntimeDevice is the one forwarder that does NOT
// set this flag: FadePrimitiveRuntime::OnInitDevice only activates device
// tracking and never triggers a nested pipeline event, so there is nothing to
// shield there.
// dev/trace/trace_events.cpp's independent observers check this flag so they
// never mistake a replacement pipeline the runtime creates for itself for a
// genuine application pipeline. Registration order matters: RegisterDevEvents
// registers the trace handlers before the ones below, so the outer (real)
// event still reaches the trace handlers before this flag is ever set; only
// the nested/synthetic event triggered by the runtime's own internal call is
// shielded.
extern thread_local bool g_dev_runtime_internal_pipeline_event;

// Must run before any ReShade callback can reach g_dev_antifade_runtime.
// Called once, at the top of RegisterDevEvents().
void InitializeDevRuntime();

void OnInitDevRuntimeDevice(reshade::api::device* owner);
void OnDestroyDevRuntimeDevice(reshade::api::device* owner);
void OnInitDevRuntimePipeline(reshade::api::device* owner,
    reshade::api::pipeline_layout layout, std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline application_pipeline);
void OnDestroyDevRuntimePipeline(
    reshade::api::device* owner, reshade::api::pipeline application_pipeline);
void OnBindDevRuntimePipeline(reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline application_pipeline);

// The same five forwarders as above, for g_dev_prefade_hypothesis_runtime.
void OnInitPreFadeHypothesisDevice(reshade::api::device* owner);
void OnDestroyPreFadeHypothesisDevice(reshade::api::device* owner);
void OnInitPreFadeHypothesisPipeline(reshade::api::device* owner,
    reshade::api::pipeline_layout layout, std::uint32_t subobject_count,
    const reshade::api::pipeline_subobject* subobjects,
    reshade::api::pipeline application_pipeline);
void OnDestroyPreFadeHypothesisPipeline(
    reshade::api::device* owner, reshade::api::pipeline application_pipeline);
void OnBindPreFadeHypothesisPipeline(reshade::api::command_list* command_list,
    reshade::api::pipeline_stage stages,
    reshade::api::pipeline application_pipeline);

}  // namespace wuwa_tfr::dev

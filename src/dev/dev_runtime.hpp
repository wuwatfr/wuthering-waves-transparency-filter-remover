// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <reshade.hpp>

#include "fade_primitive_runtime.hpp"

namespace wuwa_tfr::dev {

extern wuwa_tfr::FadePrimitiveRuntime g_dev_antifade_runtime;

extern thread_local bool g_dev_runtime_internal_pipeline_event;

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

}  // namespace wuwa_tfr::dev

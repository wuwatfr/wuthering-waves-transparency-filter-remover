// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_runtime.hpp"

#include "production/addon_shared.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

namespace {
class ScopedInternalPipelineEvent {
 public:
  ScopedInternalPipelineEvent()
      : previous_(g_dev_runtime_internal_pipeline_event) {
    g_dev_runtime_internal_pipeline_event = true;
  }
  ~ScopedInternalPipelineEvent() {
    g_dev_runtime_internal_pipeline_event = previous_;
  }

 private:
  bool previous_;
};
}  // namespace

FadePrimitiveRuntime g_dev_antifade_runtime;
thread_local bool g_dev_runtime_internal_pipeline_event = false;

void InitializeDevRuntime() {
  g_dev_antifade_runtime.set_dxc_runtime_directory(g_addon_directory);
}

void OnInitDevRuntimeDevice(device* owner) {
  if (g_target_process) g_dev_antifade_runtime.OnInitDevice(owner);
}

void OnDestroyDevRuntimeDevice(device* owner) {
  if (!g_target_process) return;
  // OnDestroyDevice drains this device's replacement pipelines and may call
  // device::destroy_pipeline() internally, which re-fires destroy_pipeline;
  // guard it exactly like the pipeline-lifecycle forwarders below.
  ScopedInternalPipelineEvent guard;
  g_dev_antifade_runtime.OnDestroyDevice(owner);
}

void OnInitDevRuntimePipeline(device* owner, pipeline_layout layout,
    std::uint32_t subobject_count, const pipeline_subobject* subobjects,
    pipeline application_pipeline) {
  if (!g_target_process) return;
  ScopedInternalPipelineEvent guard;
  g_dev_antifade_runtime.OnInitPipeline(
      owner, layout, subobject_count, subobjects, application_pipeline);
}

void OnDestroyDevRuntimePipeline(device* owner, pipeline application_pipeline) {
  if (!g_target_process) return;
  ScopedInternalPipelineEvent guard;
  g_dev_antifade_runtime.OnDestroyPipeline(owner, application_pipeline);
}

void OnBindDevRuntimePipeline(command_list* command_list,
    pipeline_stage stages, pipeline application_pipeline) {
  if (!g_target_process) return;
  ScopedInternalPipelineEvent guard;
  g_dev_antifade_runtime.OnBindPipeline(
      command_list, stages, application_pipeline);
}

}  // namespace wuwa_tfr::dev

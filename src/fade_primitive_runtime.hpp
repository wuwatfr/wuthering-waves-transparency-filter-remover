// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#ifdef _WIN32
#include <reshade.hpp>

namespace wuwa_tfr {

// Production runtime for the fully verified fade primitive. It intentionally
// owns no capture, trace, or manual-target state.
class FadePrimitiveRuntime {
 public:
  FadePrimitiveRuntime();
  ~FadePrimitiveRuntime();
  FadePrimitiveRuntime(const FadePrimitiveRuntime&) = delete;
  FadePrimitiveRuntime& operator=(const FadePrimitiveRuntime&) = delete;

  void OnInitDevice(reshade::api::device* device);
  void OnDestroyDevice(reshade::api::device* device);
  void OnInitPipeline(reshade::api::device* device,
      reshade::api::pipeline_layout layout, std::uint32_t subobject_count,
      const reshade::api::pipeline_subobject* subobjects,
      reshade::api::pipeline pipeline);
  void OnDestroyPipeline(reshade::api::device* device,
      reshade::api::pipeline pipeline);
  void OnBindPipeline(reshade::api::command_list* command_list,
      reshade::api::pipeline_stage stages, reshade::api::pipeline pipeline);
  bool enabled() const;
  void set_enabled(bool enabled);

 private:
  struct Impl;
  Impl* impl_;
};

} // namespace wuwa_tfr
#endif

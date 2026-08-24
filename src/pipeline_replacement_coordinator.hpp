// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include "pipeline_generation_state.hpp"

#include <cstdint>
#include <utility>

namespace wuwa_tfr {

template <typename Owner, typename Pipeline>
class PipelineReplacementCoordinator {
 public:
  using State = PipelineGenerationState<Owner, Pipeline>;
  using InitResult = typename State::ReconcileResult;

  template <typename Build, typename Destroy>
  InitResult OnInit(Owner owner, std::uint64_t application_pipeline,
      std::uint64_t observed_shader_identity, Build&& build, Destroy&& destroy) {
    return state_.Reconcile(owner, application_pipeline, observed_shader_identity,
        std::forward<Build>(build), std::forward<Destroy>(destroy));
  }

  template <typename Destroy>
  void OnDestroyPipeline(Owner owner, std::uint64_t application_pipeline,
      Destroy&& destroy) {
    for (Pipeline& replacement : state_.DestroyPipeline(owner, application_pipeline))
      std::forward<Destroy>(destroy)(replacement);
  }

  template <typename Destroy>
  void OnDestroyOwner(Owner owner, Destroy&& destroy) {
    for (Pipeline& replacement : state_.DrainOwner(owner))
      std::forward<Destroy>(destroy)(replacement);
  }

  template <typename Bind>
  bool OnBind(Owner owner, std::uint64_t application_pipeline, Bind&& bind) {
    return state_.WithSelected(owner, application_pipeline,
        std::forward<Bind>(bind));
  }

  std::size_t Size() const { return state_.Size(); }
  std::size_t RetainedSize() const { return state_.RetainedSize(); }

 private:
  State state_;
};

}

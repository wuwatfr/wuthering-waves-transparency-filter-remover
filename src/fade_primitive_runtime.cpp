// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_runtime.hpp"

#ifdef _WIN32
#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"
#include "fade_primitive_detector.hpp"
#include "fade_primitive_runtime_observer.hpp"
#include "pipeline_replacement_coordinator.hpp"
#include "pixel_shader_identity.hpp"
#include "preparation_context_pool.hpp"
#include "single_flight_cache.hpp"
#include "target_dither_bypass.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wuwa_tfr {
namespace {
using namespace reshade::api;
using DeviceId = std::uintptr_t;

DeviceId DeviceKey(device* value) noexcept {
  return reinterpret_cast<DeviceId>(value);
}

class ScopedFlag {
 public:
  explicit ScopedFlag(bool& value) : value_(value), previous_(value) { value_ = true; }
  ~ScopedFlag() { value_ = previous_; }
 private:
  bool& value_;
  bool previous_;
};

thread_local bool g_internal_create = false;
thread_local bool g_internal_bind = false;
thread_local bool g_internal_destroy = false;

struct PreparedShader {
  bool matches = false;
  bool attempted = false;
  std::shared_ptr<const std::vector<std::uint8_t>> bytecode;
  std::string failure;
};

struct PreparedShaderPayloadBytes {
  std::size_t operator()(const PreparedShader& state) const noexcept {
    return state.bytecode ? state.bytecode->size() : 0;
  }
};

constexpr std::size_t kDxcContextPoolCapacity = 4;
} // namespace

struct FadePrimitiveRuntime::Impl {
  SingleFlightCache<std::uint64_t, PreparedShader, std::hash<std::uint64_t>,
      PreparedShaderPayloadBytes> prepared;
  std::filesystem::path dxc_runtime_directory;
  PreparationContextPool<DxcBridge> dxc_pool;
  std::atomic<std::uint32_t> device_count{0};
  DeviceActivityState<DeviceId> activity;
  PipelineReplacementCoordinator<DeviceId, pipeline> replacements;
  std::atomic<bool> enabled{false};
  FadePrimitiveRuntimeObserver* observer = nullptr;
  std::atomic<std::uint64_t> matched_shaders{0};
  std::atomic<std::uint64_t> prepared_shaders{0};
  std::atomic<std::uint64_t> replacements_created{0};
  std::atomic<std::uint64_t> replacements_failed{0};
  std::atomic<std::uint64_t> replacement_binds{0};

  Impl()
      : dxc_pool(kDxcContextPoolCapacity, [this] {
          return std::make_unique<DxcBridge>(dxc_runtime_directory);
        }) {}

  PreparedShader PrepareOne(std::uint64_t hash, const shader_desc& original) {
    PreparedShader state;
    state.attempted = true;
    std::optional<DxilInspectionOutput> inspected;
    FadePrimitiveDiagnostic diagnostic;
    std::optional<TargetDitherBypassResult> patched;
    try {
      auto dxc = dxc_pool.Acquire();
      if (!dxc) {
        state.failure = "DXC context allocation failed";
      } else if (!dxc->available()) {
        state.failure = dxc->init_error();
      } else {
        inspected = dxc->InspectShader(original.code, original.code_size);
        if (!inspected->success) {
          state.failure = inspected->error;
        } else {
          diagnostic = AnalyzeFadePrimitiveV1(inspected->original_ir);
          state.matches = !diagnostic.instances.empty();
          if (!state.matches) {
            state.failure = "no fully verified transparency-filter primitive";
          } else {
            matched_shaders.fetch_add(1, std::memory_order_relaxed);
            patched = PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
                inspected->original_ir);
            if (!patched->success ||
                patched->verified_instance_count != diagnostic.instances.size() ||
                patched->patched_instance_count != diagnostic.instances.size()) {
              state.failure = patched->error.empty() ?
                  "structural verification failed" : patched->error;
            } else {
              auto bytes = std::make_shared<std::vector<std::uint8_t>>();
              std::string error;
              DxilAssemblyValidationOutput result;
              if (!dxc->AssembleAndValidate(patched->llvm_ir, *bytes, error, result)) {
                state.failure = error;
              } else {
                state.bytecode = std::move(bytes);
                prepared_shaders.fetch_add(1, std::memory_order_relaxed);
              }
            }
          }
        }
      }
    } catch (const std::exception& exception) {
      state.failure = "preparation exception: " + std::string(exception.what());
    } catch (...) {
      state.failure = "preparation exception";
    }
    if (!state.bytecode)
      replacements_failed.fetch_add(1, std::memory_order_relaxed);

    if (observer) {
      FadePrimitiveRuntimeObserver::ShaderPreparationObservation observation;
      observation.original_shader_hash = hash;
      observation.original_bytecode_size = original.code_size;
      observation.inspection_succeeded = inspected.has_value() && inspected->success;
      if (inspected) {
        if (!inspected->success) observation.inspection_error = inspected->error;
      } else {
        observation.inspection_error = state.failure;
      }
      if (observation.inspection_succeeded)
        observation.original_ir = &inspected->original_ir;
      observation.fade_primitive = diagnostic;
      if (patched) {
        observation.pre_fade_evidence = patched->instance_evidence;
        observation.patch_succeeded = patched->success;
        if (!patched->success) observation.patch_failure = state.failure;
      }
      observation.prepared_succeeded = state.bytecode != nullptr;
      observation.prepared_failure = state.failure;
      observer->OnShaderPrepared(observation);
    }
    return state;
  }

  std::shared_ptr<const std::vector<std::uint8_t>> Prepare(
      std::uint64_t hash, const shader_desc& original) {
    const PreparedShader state = prepared.GetOrPrepare(
        hash,
        [&] { return PrepareOne(hash, original); },
        [&] {
          PreparedShader aborted;
          aborted.attempted = true;
          aborted.failure = "preparation aborted";
          replacements_failed.fetch_add(1, std::memory_order_relaxed);
          return aborted;
        });
    return state.bytecode;
  }
};

FadePrimitiveRuntime::FadePrimitiveRuntime() : impl_(new Impl()) {}
FadePrimitiveRuntime::~FadePrimitiveRuntime() { delete impl_; }

void FadePrimitiveRuntime::set_dxc_runtime_directory(
    std::filesystem::path addon_directory) {
  impl_->dxc_runtime_directory = std::move(addon_directory);
}

void FadePrimitiveRuntime::set_observer(FadePrimitiveRuntimeObserver* observer) {
  impl_->observer = observer;
}

void FadePrimitiveRuntime::OnInitDevice(device* owner) {
  if (!owner || owner->get_api() != device_api::d3d12) return;
  if (impl_->activity.Activate(DeviceKey(owner)))
    impl_->device_count.fetch_add(1, std::memory_order_relaxed);
}

void FadePrimitiveRuntime::OnDestroyDevice(device* owner) {
  if (!owner || owner->get_api() != device_api::d3d12) return;
  auto teardown = impl_->activity.Deactivate(DeviceKey(owner));
  if (!teardown) return;
  impl_->replacements.OnDestroyOwner(DeviceKey(owner), [owner](pipeline replacement) {
    ScopedFlag internal(g_internal_destroy);
    owner->destroy_pipeline(replacement);
  });
  if (impl_->device_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    impl_->dxc_pool.Drain();
  }
}

void FadePrimitiveRuntime::OnInitPipeline(device* owner, pipeline_layout layout,
    std::uint32_t count, const pipeline_subobject* subobjects, pipeline application) {
  if (g_internal_create || !owner || application.handle == 0) return;
  auto active = impl_->activity.Acquire(DeviceKey(owner));
  if (!active) return;
  const shader_desc* original = nullptr;
  std::uint64_t hash = 0;
  const bool has_pixel_shader =
      FindDxilPixelShader(count, subobjects, original, hash);
  if (impl_->observer) {
    FadePrimitiveRuntimeObserver::PipelineInitObservation observation;
    observation.device = owner;
    observation.application_pipeline = application.handle;
    observation.pixel_shader_identified = has_pixel_shader;
    observation.pixel_shader_hash = has_pixel_shader ? hash : 0;
    impl_->observer->OnPipelineInit(observation);
  }
  const auto destroy_replacement = [owner](pipeline replacement) {
    ScopedFlag internal(g_internal_destroy);
    owner->destroy_pipeline(replacement);
  };
  const auto init_result = impl_->replacements.OnInit(
      DeviceKey(owner), application.handle, has_pixel_shader ? hash : 0,
      [&]() -> std::optional<pipeline> {
        if (!has_pixel_shader) return std::nullopt;
        const auto bytecode = impl_->Prepare(hash, *original);
        if (!bytecode) return std::nullopt;
        std::vector<pipeline_subobject> replacement_subobjects(
            subobjects, subobjects + count);
        shader_desc replacement_shader = *original;
        replacement_shader.code = bytecode->data();
        replacement_shader.code_size = bytecode->size();
        bool replaced = false;
        for (auto& subobject : replacement_subobjects) {
          if (subobject.type == pipeline_subobject_type::pixel_shader &&
              subobject.data == original) {
            subobject.data = &replacement_shader;
            replaced = true;
          }
        }
        if (!replaced) {
          impl_->replacements_failed.fetch_add(1, std::memory_order_relaxed);
          return std::nullopt;
        }
        pipeline replacement{};
        ScopedFlag internal(g_internal_create);
        if (!owner->create_pipeline(layout, count, replacement_subobjects.data(),
                &replacement) || replacement.handle == 0) {
          impl_->replacements_failed.fetch_add(1, std::memory_order_relaxed);
          return std::nullopt;
        }
        return replacement;
      }, destroy_replacement);
  if (init_result == PipelineReplacementCoordinator<DeviceId, pipeline>::
          InitResult::Published)
    impl_->replacements_created.fetch_add(1, std::memory_order_relaxed);
}

void FadePrimitiveRuntime::OnDestroyPipeline(device* owner, pipeline application) {
  if (g_internal_destroy || !owner || application.handle == 0) return;
  auto active = impl_->activity.Acquire(DeviceKey(owner));
  if (!active) return;
  impl_->replacements.OnDestroyPipeline(DeviceKey(owner), application.handle,
      [owner](pipeline replacement) {
        ScopedFlag internal(g_internal_destroy);
        owner->destroy_pipeline(replacement);
      });
}

void FadePrimitiveRuntime::OnBindPipeline(command_list* list, pipeline_stage stages,
    pipeline application) {
  if (g_internal_bind || !impl_->enabled.load(std::memory_order_relaxed) ||
      !list || (stages & pipeline_stage::pixel_shader) != pipeline_stage::pixel_shader)
    return;
  device* owner = list->get_device();
  if (!owner) return;
  impl_->replacements.OnBind(DeviceKey(owner), application.handle,
      [&](pipeline replacement) {
        ScopedFlag internal(g_internal_bind);
        list->bind_pipeline(stages, replacement);
        impl_->replacement_binds.fetch_add(1, std::memory_order_relaxed);
      });
}

bool FadePrimitiveRuntime::enabled() const {
  return impl_->enabled.load(std::memory_order_relaxed);
}

void FadePrimitiveRuntime::set_enabled(bool enabled) {
  impl_->enabled.store(enabled, std::memory_order_relaxed);
}

FadePrimitiveRuntimeTelemetrySnapshot
FadePrimitiveRuntime::memory_telemetry_snapshot() const {
  const auto cache = impl_->prepared.GetSnapshot();
  const auto replacement_count = impl_->replacements.RetainedSize();
  return {
      static_cast<std::uint64_t>(cache.completed_entries),
      static_cast<std::uint64_t>(cache.retained_payload_bytes),
      static_cast<std::uint64_t>(cache.in_flight_entries),
      static_cast<std::uint64_t>(replacement_count),
      impl_->device_count.load(std::memory_order_relaxed),
      impl_->matched_shaders.load(std::memory_order_relaxed),
      impl_->prepared_shaders.load(std::memory_order_relaxed),
      impl_->replacements_created.load(std::memory_order_relaxed),
      impl_->replacements_failed.load(std::memory_order_relaxed),
      impl_->replacement_binds.load(std::memory_order_relaxed)};
}

} // namespace wuwa_tfr
#endif

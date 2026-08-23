// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_runtime.hpp"

#ifdef _WIN32
#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"
#include "fade_primitive_detector.hpp"
#include "pipeline_replacement_coordinator.hpp"
#include "preparation_context_pool.hpp"
#include "single_flight_cache.hpp"
#include "target_dither_bypass.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wuwa_tfr {
namespace {
using namespace reshade::api;
using DeviceId = std::uintptr_t;

DeviceId DeviceKey(device* value) noexcept {
  return reinterpret_cast<DeviceId>(value);
}

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool HasDxilChunk(const void* code, std::size_t size) {
  if (!code || size < 32) return false;
  const auto* bytes = static_cast<const std::uint8_t*>(code);
  if (std::memcmp(bytes, "DXBC", 4) != 0) return false;
  const auto read_u32 = [bytes](std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes + offset, sizeof(value));
    return value;
  };
  const std::uint32_t total_size = read_u32(24);
  const std::uint32_t chunk_count = read_u32(28);
  if (total_size != size || total_size < 32 ||
      chunk_count > (total_size - 32) / 4)
    return false;
  bool has_dxil = false;
  for (std::uint32_t i = 0; i < chunk_count; ++i) {
    const std::uint32_t offset = read_u32(32 + 4 * i);
    if (offset > total_size || total_size - offset < 8) return false;
    const std::uint32_t chunk_size = read_u32(offset + 4);
    if (chunk_size > total_size - offset - 8) return false;
    has_dxil = has_dxil || std::memcmp(bytes + offset, "DXIL", 4) == 0;
  }
  return has_dxil;
}

bool FindPixelShader(std::uint32_t count, const pipeline_subobject* subobjects,
    const shader_desc*& shader, std::uint64_t& hash) {
  shader = nullptr;
  hash = 0;
  if (!subobjects) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type != pipeline_subobject_type::pixel_shader ||
        !subobjects[i].data)
      continue;
    const auto& candidate = *static_cast<const shader_desc*>(subobjects[i].data);
    if (!HasDxilChunk(candidate.code, candidate.code_size)) continue;
    const std::uint64_t candidate_hash = Fnv1a64(candidate.code, candidate.code_size);
    if (shader && hash != candidate_hash) return false;
    shader = &candidate;
    hash = candidate_hash;
  }
  return shader != nullptr;
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
  // Lock order is activity -> single-flight cache -> DXC pool. The cache lock
  // is released before a context is acquired or any DXC call begins. Last-
  // device teardown holds activity exclusively, then drains the pool; it never
  // takes the cache lock, so it cannot wait in a lock cycle with a callback.
  SingleFlightCache<std::uint64_t, PreparedShader, std::hash<std::uint64_t>,
      PreparedShaderPayloadBytes> prepared;
  std::filesystem::path dxc_runtime_directory;
  PreparationContextPool<DxcBridge> dxc_pool;
  std::atomic<std::uint32_t> device_count{0};
  DeviceActivityState<DeviceId> activity;
  PipelineReplacementCoordinator<DeviceId, pipeline> replacements;
  std::atomic<bool> enabled{false};
  // These are cumulative runtime activity counters, not retained-object
  // gauges. Telemetry snapshots only load them and never increment them.
  std::atomic<std::uint64_t> matched_shaders{0};
  std::atomic<std::uint64_t> prepared_shaders{0};
  std::atomic<std::uint64_t> replacements_created{0};
  std::atomic<std::uint64_t> replacements_failed{0};
  std::atomic<std::uint64_t> replacement_binds{0};

  Impl()
      : dxc_pool(kDxcContextPoolCapacity, [this] {
          // DxcBridge owns its own module, COM interfaces, assembler, and
          // validator. A leased instance is never called concurrently.
          return std::make_unique<DxcBridge>(dxc_runtime_directory);
        }) {}

  PreparedShader PrepareOne(const shader_desc& original) {
    PreparedShader state;
    state.attempted = true;
    try {
      auto dxc = dxc_pool.Acquire();
      if (!dxc) {
        state.failure = "DXC context allocation failed";
      } else if (!dxc->available()) {
        state.failure = dxc->init_error();
      } else {
        const auto inspected = dxc->InspectShader(original.code, original.code_size);
        if (!inspected.success) {
          state.failure = inspected.error;
        } else {
          const auto diagnostic = AnalyzeFadePrimitiveV1(inspected.original_ir);
          state.matches = !diagnostic.instances.empty();
          if (!state.matches) {
            state.failure = "no fully verified transparency-filter primitive";
          } else {
            matched_shaders.fetch_add(1, std::memory_order_relaxed);
            const auto patched = PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(
                inspected.original_ir);
            if (!patched.success ||
                patched.verified_instance_count != diagnostic.instances.size() ||
                patched.patched_instance_count != diagnostic.instances.size()) {
              state.failure = patched.error.empty() ?
                  "structural verification failed" : patched.error;
            } else {
              auto bytes = std::make_shared<std::vector<std::uint8_t>>();
              std::string error;
              DxilAssemblyValidationOutput result;
              if (!dxc->AssembleAndValidate(patched.llvm_ir, *bytes, error, result)) {
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
    return state;
  }

  std::shared_ptr<const std::vector<std::uint8_t>> Prepare(
      std::uint64_t hash, const shader_desc& original) {
    const PreparedShader state = prepared.GetOrPrepare(
        hash,
        [&] { return PrepareOne(original); },
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
    // Deactivate holds activity exclusively, so all callers that can lease a
    // DXC context have returned. Drain also protects against future changes
    // that add a preparation path outside this callback.
    impl_->dxc_pool.Drain();
  }
}

void FadePrimitiveRuntime::OnInitPipeline(device* owner, pipeline_layout layout,
    std::uint32_t count, const pipeline_subobject* subobjects, pipeline application) {
  if (g_internal_create || !owner || application.handle == 0) return;
  auto active = impl_->activity.Acquire(DeviceKey(owner));
  if (!active) return;
  // D3D12 PSOs are immutable: the live (device, application handle) pair is
  // the canonical identity of all application pipeline state. A differing
  // observed shader hash for that same live handle is contradictory evidence,
  // so the coordinator disables replacement selection rather than replacing
  // or destroying an object that may still be referenced by command lists.
  const shader_desc* original = nullptr;
  std::uint64_t hash = 0;
  const bool has_pixel_shader = FindPixelShader(count, subobjects, original, hash);
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
  // GetSnapshot() releases the single-flight cache lock before this function
  // separately obtains the replacement-state lock via RetainedSize(). Do not
  // combine these two snapshots under nested locks.
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

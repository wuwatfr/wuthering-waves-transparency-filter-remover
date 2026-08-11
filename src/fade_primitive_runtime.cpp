// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_runtime.hpp"

#ifdef _WIN32
#include "device_activity_state.hpp"
#include "dxc_bridge.hpp"
#include "fade_primitive_detector.hpp"
#include "pipeline_replacement_state.hpp"
#include "target_dither_bypass.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
} // namespace

struct FadePrimitiveRuntime::Impl {
  std::mutex preparation_mutex;
  std::unordered_map<std::uint64_t, PreparedShader> prepared;
  std::unique_ptr<DxcBridge> dxc;
  std::filesystem::path dxc_runtime_directory;
  std::atomic<std::uint32_t> device_count{0};
  DeviceActivityState<DeviceId> activity;
  PipelineReplacementState<DeviceId, pipeline> replacements;
  std::atomic<bool> enabled{false};
  std::atomic<std::uint64_t> matched_shaders{0};
  std::atomic<std::uint64_t> prepared_shaders{0};
  std::atomic<std::uint64_t> replacements_created{0};
  std::atomic<std::uint64_t> replacements_failed{0};
  std::atomic<std::uint64_t> replacement_binds{0};

  std::shared_ptr<const std::vector<std::uint8_t>> Prepare(
      std::uint64_t hash, const shader_desc& original) {
    std::lock_guard lock(preparation_mutex);
    if (const auto entry = prepared.find(hash); entry != prepared.end())
      return entry->second.bytecode;
    PreparedShader state;
    state.attempted = true;
    if (!dxc)
      dxc = std::make_unique<DxcBridge>(dxc_runtime_directory);
    if (!dxc->available()) {
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
          const auto patched = PatchAllVerifiedFadePrimitiveInstancesToIdentity(
              inspected.original_ir);
          if (!patched.success ||
              patched.verified_instance_count != diagnostic.instances.size() ||
              patched.patched_instance_count != diagnostic.instances.size()) {
            state.failure = patched.error.empty() ? "structural verification failed" : patched.error;
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
    const auto inserted = prepared.emplace(hash, std::move(state));
    if (!inserted.first->second.bytecode)
      replacements_failed.fetch_add(1, std::memory_order_relaxed);
    return inserted.first->second.bytecode;
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
  for (const auto& item : impl_->replacements.DrainOwner(DeviceKey(owner))) {
    if (!item.replacements.final_antifade) continue;
    ScopedFlag internal(g_internal_destroy);
    owner->destroy_pipeline(*item.replacements.final_antifade);
  }
  if (impl_->device_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::lock_guard lock(impl_->preparation_mutex);
    impl_->dxc.reset();
  }
}

void FadePrimitiveRuntime::OnInitPipeline(device* owner, pipeline_layout layout,
    std::uint32_t count, const pipeline_subobject* subobjects, pipeline application) {
  if (g_internal_create || !owner || application.handle == 0) return;
  auto active = impl_->activity.Acquire(DeviceKey(owner));
  if (!active) return;
  const shader_desc* original = nullptr;
  std::uint64_t hash = 0;
  if (!FindPixelShader(count, subobjects, original, hash)) return;
  const auto bytecode = impl_->Prepare(hash, *original);
  if (!bytecode || impl_->replacements.WithSelected(DeviceKey(owner), application.handle,
          true, true, [](pipeline) {})) return;
  std::vector<pipeline_subobject> replacement_subobjects(subobjects, subobjects + count);
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
  if (!replaced) { impl_->replacements_failed.fetch_add(1, std::memory_order_relaxed); return; }
  pipeline replacement{};
  {
    ScopedFlag internal(g_internal_create);
    if (!owner->create_pipeline(layout, count, replacement_subobjects.data(), &replacement) ||
        replacement.handle == 0) {
      impl_->replacements_failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  const auto previous = impl_->replacements.PutFinalAntiFade(
      DeviceKey(owner), application.handle, replacement);
  if (previous) {
    ScopedFlag internal(g_internal_destroy);
    owner->destroy_pipeline(*previous);
  }
  impl_->replacements_created.fetch_add(1, std::memory_order_relaxed);
}

void FadePrimitiveRuntime::OnDestroyPipeline(device* owner, pipeline application) {
  if (g_internal_destroy || !owner || application.handle == 0) return;
  auto active = impl_->activity.Acquire(DeviceKey(owner));
  if (!active) return;
  const auto removed = impl_->replacements.Remove(DeviceKey(owner), application.handle);
  if (!removed.final_antifade) return;
  ScopedFlag internal(g_internal_destroy);
  owner->destroy_pipeline(*removed.final_antifade);
}

void FadePrimitiveRuntime::OnBindPipeline(command_list* list, pipeline_stage stages,
    pipeline application) {
  if (g_internal_bind || !impl_->enabled.load(std::memory_order_relaxed) ||
      !list || (stages & pipeline_stage::pixel_shader) != pipeline_stage::pixel_shader)
    return;
  device* owner = list->get_device();
  if (!owner) return;
  impl_->replacements.WithSelected(DeviceKey(owner), application.handle, true, true,
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

} // namespace wuwa_tfr
#endif

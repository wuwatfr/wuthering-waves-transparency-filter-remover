// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/capture/fade_control_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "dev/capture/descriptor_table_state.hpp"
#include "dev/capture/fade_control_snapshot.hpp"
#include "dev/capture/manual_capture_state.hpp"
#include "dev/dev_inspection.hpp"
#include "dev/diagnostics/dev_diagnostics.hpp"
#include "dev/resource_lifecycle_state.hpp"

using namespace reshade::api;

namespace wuwa_tfr::dev {

namespace {

std::mutex g_fade_control_mutex;
FadeControlAccumulator g_fade_control_accumulator;
FadeControlSnapshotAccumulator g_fade_control_snapshot_accumulator;
bool g_fade_control_session_enabled = false;

bool g_fade_control_pending_enabled = true;

FadeControlTrackerCapacityDiagnostics g_fade_control_runtime_taint;
FadeControlTrackerCapacityAccumulator g_fade_control_capture_diagnostics;

struct LayoutCbvRangeInfo {
  std::vector<DescriptorCbvRangeInfo> ranges;
  bool ranges_truncated = false;
};

struct LayoutPushConstantRangeInfo {
  std::vector<PushConstantRangeInfo> ranges;
  bool ranges_truncated = false;
};

constexpr std::size_t kMaxTrackedFadeControlLayouts = 4096;
constexpr std::size_t kMaxDescriptorRangesPerLayout = 256;
constexpr std::size_t kMaxPushConstantRangesPerLayout = 256;

std::unordered_map<wuwa_tfr::TraceLiveHandleKey, LayoutCbvRangeInfo,
    wuwa_tfr::TraceLiveHandleKeyHash>
    g_layout_push_cbv_ranges;

std::unordered_map<wuwa_tfr::TraceLiveHandleKey, LayoutCbvRangeInfo,
    wuwa_tfr::TraceLiveHandleKeyHash>
    g_layout_descriptor_cbv_ranges;

std::unordered_map<wuwa_tfr::TraceLiveHandleKey, LayoutPushConstantRangeInfo,
    wuwa_tfr::TraceLiveHandleKeyHash>
    g_layout_push_constant_ranges;

DescriptorSlotTable g_descriptor_table_slots;

struct MappedBufferInfo {
  std::byte* base = nullptr;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

constexpr std::size_t kMaxTrackedMappedBuffers = 4096;
std::unordered_map<wuwa_tfr::TraceLiveHandleKey, MappedBufferInfo,
    wuwa_tfr::TraceLiveHandleKeyHash>
    g_mapped_buffers;

bool IsCbvDescriptorType(descriptor_type type) noexcept {
  return type == descriptor_type::constant_buffer ||
      type == descriptor_type::constant_buffer_with_dynamic_offset;
}

void AppendDescriptorCbvRange(std::uint32_t param_index,
    const descriptor_range& range,
    std::vector<DescriptorCbvRangeInfo>& ranges, bool& truncated) {
  if (!IsCbvDescriptorType(range.type)) return;
  if (range.count == 0 || range.count == UINT32_MAX) return;
  if (range.array_size != 1) return;
  if (ranges.size() >= kMaxDescriptorRangesPerLayout) {
    truncated = true;
    return;
  }
  ranges.push_back(DescriptorCbvRangeInfo{param_index, range.binding,
      range.dx_register_space, range.dx_register_index, range.count});
}

void AppendPushConstantRange(std::uint32_t param_index,
    const constant_range& range, std::vector<PushConstantRangeInfo>& ranges,
    bool& truncated) {
  if (range.count == 0) return;
  if (ranges.size() >= kMaxPushConstantRangesPerLayout) {
    truncated = true;
    return;
  }
  ranges.push_back(PushConstantRangeInfo{param_index, range.dx_register_space,
      range.dx_register_index, range.count});
}

template <typename Map, typename Range>
void StoreLayoutRanges(Map& target, const wuwa_tfr::TraceLiveHandleKey& key,
    std::vector<Range>&& ranges, bool truncated) {
  if (ranges.empty() && !truncated) return;
  if (target.size() >= kMaxTrackedFadeControlLayouts && !target.contains(key)) {
    g_fade_control_runtime_taint.layout_map_loss = true;
    return;
  }
  target[key] = typename Map::mapped_type{std::move(ranges), truncated};
}

void OnInitFadeControlPipelineLayout(device* owner, std::uint32_t param_count,
    const pipeline_layout_param* params, pipeline_layout layout) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      !params || layout.handle == 0)
    return;
  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return;

  std::vector<DescriptorCbvRangeInfo> push_ranges;
  std::vector<DescriptorCbvRangeInfo> descriptor_ranges;
  std::vector<PushConstantRangeInfo> push_constant_ranges;
  bool push_ranges_truncated = false;
  bool descriptor_ranges_truncated = false;
  bool push_constant_ranges_truncated = false;
  for (std::uint32_t i = 0; i < param_count; ++i) {
    const auto& param = params[i];
    switch (param.type) {
      case pipeline_layout_param_type::push_descriptors:
        AppendDescriptorCbvRange(i, param.push_descriptors, push_ranges,
            push_ranges_truncated);
        break;
      case pipeline_layout_param_type::push_descriptors_with_ranges:
        for (std::uint32_t r = 0; r < param.descriptor_table.count; ++r)
          AppendDescriptorCbvRange(i, param.descriptor_table.ranges[r],
              push_ranges, push_ranges_truncated);
        break;
      case pipeline_layout_param_type::push_descriptors_with_ranges_and_flags:
        for (std::uint32_t r = 0; r < param.descriptor_table_with_flags.count;
             ++r)
          AppendDescriptorCbvRange(i,
              param.descriptor_table_with_flags.ranges[r], push_ranges,
              push_ranges_truncated);
        break;
      case pipeline_layout_param_type::descriptor_table:
        for (std::uint32_t r = 0; r < param.descriptor_table.count; ++r)
          AppendDescriptorCbvRange(i, param.descriptor_table.ranges[r],
              descriptor_ranges, descriptor_ranges_truncated);
        break;
      case pipeline_layout_param_type::descriptor_table_with_flags:
        for (std::uint32_t r = 0; r < param.descriptor_table_with_flags.count;
             ++r)
          AppendDescriptorCbvRange(i,
              param.descriptor_table_with_flags.ranges[r], descriptor_ranges,
              descriptor_ranges_truncated);
        break;
      case pipeline_layout_param_type::push_constants:
        AppendPushConstantRange(i, param.push_constants, push_constant_ranges,
            push_constant_ranges_truncated);
        break;
      default:
        break;
    }
  }

  const wuwa_tfr::TraceLiveHandleKey key{DeviceKey(owner), layout.handle};
  std::lock_guard lock(g_fade_control_mutex);
  if (push_ranges_truncated || descriptor_ranges_truncated ||
      push_constant_ranges_truncated) {
    g_fade_control_runtime_taint.descriptor_range_truncated = true;
  }
  StoreLayoutRanges(g_layout_push_cbv_ranges, key, std::move(push_ranges),
      push_ranges_truncated);
  StoreLayoutRanges(g_layout_descriptor_cbv_ranges, key,
      std::move(descriptor_ranges), descriptor_ranges_truncated);
  StoreLayoutRanges(g_layout_push_constant_ranges, key,
      std::move(push_constant_ranges), push_constant_ranges_truncated);
}

void OnDestroyFadeControlPipelineLayout(device* owner, pipeline_layout layout) {
  if (!owner || layout.handle == 0) return;
  const wuwa_tfr::TraceLiveHandleKey key{DeviceKey(owner), layout.handle};
  std::lock_guard lock(g_fade_control_mutex);
  g_layout_push_cbv_ranges.erase(key);
  g_layout_descriptor_cbv_ranges.erase(key);
  g_layout_push_constant_ranges.erase(key);
}

void OnMapFadeControlBuffer(device* owner, resource resource_handle,
    std::uint64_t offset, std::uint64_t size, map_access access,
    void** data) {
  if (!g_target_process || !owner || resource_handle.handle == 0 || !data ||
      !*data)
    return;
  if (access != map_access::read_only && access != map_access::read_write &&
      access != map_access::write_only)
    return;

  std::uint64_t resolved_size = size;
  if (resolved_size == 0 || resolved_size == UINT64_MAX) {
    resolved_size = owner->get_resource_desc(resource_handle).buffer.size;
  }
  if (resolved_size == 0) return;

  const wuwa_tfr::TraceLiveHandleKey key{
      DeviceKey(owner), resource_handle.handle};
  std::lock_guard lock(g_fade_control_mutex);
  if (g_mapped_buffers.size() >= kMaxTrackedMappedBuffers &&
      !g_mapped_buffers.contains(key)) {
    g_fade_control_runtime_taint.mapped_buffer_loss = true;
    return;
  }
  g_mapped_buffers[key] =
      MappedBufferInfo{static_cast<std::byte*>(*data), offset, resolved_size};
}

void OnUnmapFadeControlBuffer(device* owner, resource resource_handle) {
  if (!owner || resource_handle.handle == 0) return;
  std::lock_guard lock(g_fade_control_mutex);
  g_mapped_buffers.erase(
      wuwa_tfr::TraceLiveHandleKey{DeviceKey(owner), resource_handle.handle});
}

void OnDestroyFadeControlResource(device* owner, resource resource_handle) {
  if (resource_handle.handle == 0) return;
  std::lock_guard lock(g_fade_control_mutex);
  if (owner) {
    const wuwa_tfr::TraceLiveHandleKey key{
        DeviceKey(owner), resource_handle.handle};
    g_mapped_buffers.erase(key);
    InvalidateDescriptorTableSlotsForResource(
        g_descriptor_table_slots, DeviceKey(owner), resource_handle.handle);
  }
}

bool OnUpdateFadeControlDescriptorTables(device* owner, std::uint32_t count,
    const descriptor_table_update* updates) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      !updates)
    return false;
  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return false;

  std::lock_guard lock(g_fade_control_mutex);
  for (std::uint32_t u = 0; u < count; ++u) {
    const auto& update = updates[u];
    if (update.table.handle == 0 || update.count == 0 || !update.descriptors)
      continue;
    if (update.type != descriptor_type::constant_buffer &&
        update.type != descriptor_type::constant_buffer_with_dynamic_offset)
      continue;
    const auto* ranges = static_cast<const buffer_range*>(update.descriptors);
    for (std::uint32_t i = 0; i < update.count; ++i) {
      const DescriptorSlotKey key{{DeviceKey(owner), update.table.handle},
          update.binding + update.array_offset + i};
      if (ranges[i].buffer.handle == 0) {
        SetDescriptorTableSlot(g_descriptor_table_slots, key, std::nullopt);
        continue;
      }
      const auto incarnation = FindActiveResourceLifecycle(
          {DeviceKey(owner), ranges[i].buffer.handle}).incarnation_id;
      if (!SetDescriptorTableSlot(g_descriptor_table_slots, key,
              DescriptorSlotContent{ranges[i].buffer.handle, incarnation,
                  ranges[i].offset, ranges[i].size})) {
        g_fade_control_runtime_taint.descriptor_slot_loss = true;
      }
    }
  }
  return false;
}

bool OnCopyFadeControlDescriptorTables(
    device* owner, std::uint32_t count, const descriptor_table_copy* copies) {
  if (!g_target_process || !owner || owner->get_api() != device_api::d3d12 ||
      !copies)
    return false;
  auto active = g_device_activity.Acquire(DeviceKey(owner));
  if (!active) return false;

  const wuwa_tfr::DeviceIdentity device_key = DeviceKey(owner);
  std::lock_guard lock(g_fade_control_mutex);
  for (std::uint32_t c = 0; c < count; ++c) {
    const auto& copy = copies[c];
    if (copy.source_table.handle == 0 || copy.dest_table.handle == 0 ||
        copy.count == 0)
      continue;
    for (std::uint32_t i = 0; i < copy.count; ++i) {
      const DescriptorSlotKey source{{device_key, copy.source_table.handle},
          copy.source_binding + copy.source_array_offset + i};
      const DescriptorSlotKey dest{{device_key, copy.dest_table.handle},
          copy.dest_binding + copy.dest_array_offset + i};
      if (!CopyDescriptorTableSlot(g_descriptor_table_slots, source, dest))
        g_fade_control_runtime_taint.descriptor_slot_loss = true;
    }
  }
  return false;
}

void OnDestroyFadeControlDevice(device* owner) {
  if (!owner) return;
  const wuwa_tfr::DeviceIdentity device_key = DeviceKey(owner);
  std::lock_guard lock(g_fade_control_mutex);
  EraseDeviceOwnedLiveHandleEntries(g_mapped_buffers, device_key);
  EraseDeviceOwnedDescriptorTableSlots(g_descriptor_table_slots, device_key);
  EraseDeviceOwnedLiveHandleEntries(g_layout_push_cbv_ranges, device_key);
  EraseDeviceOwnedLiveHandleEntries(g_layout_descriptor_cbv_ranges, device_key);
  EraseDeviceOwnedLiveHandleEntries(g_layout_push_constant_ranges, device_key);
}

FadeControlValueSample Unavailable(std::uint16_t reason) noexcept {
  return FadeControlValueSample{false, 0, reason};
}

void TryCaptureFadeControlSnapshot(const FadeControlRecordKey& key,
    RecordedTraceDraw& draw, std::uint64_t cbv_offset,
    std::uint32_t predicate_vector, const MappedBufferInfo& mapped) {
  const auto window = ResolveFadeControlSnapshotWindow(cbv_offset,
      predicate_vector, kFadeControlSnapshotVectorRadius, mapped.offset,
      mapped.offset + mapped.size);
  if (!window.valid) return;

  FadeControlSnapshotRecord record;
  record.pipeline = draw.pipeline;
  record.cbv_offset = cbv_offset;
  record.mapped_range_offset = mapped.offset;
  record.mapped_range_size = mapped.size;
  record.window_start = window.start;
  record.window_size = window.size;
  const std::byte* source_bytes = mapped.base + (window.start - mapped.offset);
  std::memcpy(record.raw_bytes.data(), source_bytes, window.size);
  draw.pending_fade_snapshots.push_back(
      PendingFadeControlSnapshot{key, record});
}

void ObserveMappedCbvValue(const FadeControlRecordKey& key,
    RecordedTraceDraw& draw, wuwa_tfr::DeviceIdentity device,
    std::uint64_t resource_handle, std::uint64_t range_offset,
    std::uint64_t range_size, std::uint32_t vector_index,
    std::uint32_t component) {
  const std::uint64_t byte_offset =
      ResolveFadeControlByteOffset(range_offset, vector_index, component);

  if (range_size != UINT64_MAX &&
      !FadeControlByteOffsetInDeclaredCbvRange(
          byte_offset, range_offset, range_size)) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonDeclaredCbvRangeExceeded)});
    return;
  }

  const auto mapped_it = g_mapped_buffers.find(
      wuwa_tfr::TraceLiveHandleKey{device, resource_handle});
  if (mapped_it == g_mapped_buffers.end()) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonNotMapped)});
    return;
  }

  if (!FadeControlByteOffsetInMappedRegion(
          byte_offset, mapped_it->second.offset, mapped_it->second.size)) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonOutOfRange)});
    return;
  }

  std::uint32_t bits = 0;
  const std::byte* source_bytes =
      mapped_it->second.base + (byte_offset - mapped_it->second.offset);
  std::memcpy(&bits, source_bytes, sizeof(bits));
  draw.pending_fade_observations.push_back(
      {key, FadeControlValueSample{true, bits, 0}});

  if (key.role == FadeControlRole::Predicate) {
    TryCaptureFadeControlSnapshot(
        key, draw, range_offset, vector_index, mapped_it->second);
  }
}

bool TrySampleRootPushDescriptorsRoute(const CommandListTrace& trace,
    FadeControlRecordKey key, RecordedTraceDraw& draw,
    const FadeControlSamplingSource& source) {
  const auto layout_it = g_layout_push_cbv_ranges.find(
      wuwa_tfr::TraceLiveHandleKey{trace.device, trace.bound_layout});
  if (layout_it == g_layout_push_cbv_ranges.end() ||
      layout_it->second.ranges_truncated)
    return false;
  const auto slot = ResolveDescriptorTableCbvSlot(
      layout_it->second.ranges, source.cbuffer_space, source.cbuffer_register);
  if (!slot) return false;

  key.binding_route = FadeControlBindingRoute::RootPushDescriptors;
  const RootCbvKey binding_key{trace.bound_layout, slot->param_index, slot->slot};
  const auto binding_it = trace.root_cbv_bindings.find(binding_key);
  if (binding_it == trace.root_cbv_bindings.end()) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonBindingUnresolved)});
    return true;
  }

  const auto& binding = binding_it->second;
  key.runtime_resource_incarnation = binding.resource_incarnation;
  key.runtime_range_offset = binding.offset;
  ObserveMappedCbvValue(key, draw, trace.device, binding.resource_handle,
      binding.offset, binding.size, source.vector_index, source.component);
  return true;
}

bool TrySampleDescriptorTableRoute(const CommandListTrace& trace,
    FadeControlRecordKey key, RecordedTraceDraw& draw,
    const FadeControlSamplingSource& source) {
  const auto layout_it = g_layout_descriptor_cbv_ranges.find(
      wuwa_tfr::TraceLiveHandleKey{trace.device, trace.bound_layout});
  if (layout_it == g_layout_descriptor_cbv_ranges.end() ||
      layout_it->second.ranges_truncated)
    return false;
  const auto slot = ResolveDescriptorTableCbvSlot(
      layout_it->second.ranges, source.cbuffer_space, source.cbuffer_register);
  if (!slot) return false;

  key.binding_route = FadeControlBindingRoute::DescriptorTable;
  const auto table_it = trace.bound_descriptor_tables.find(
      BoundDescriptorTableKey{trace.bound_layout, slot->param_index});
  if (table_it == trace.bound_descriptor_tables.end()) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonBindingUnresolved)});
    return true;
  }
  if (table_it->second.dynamic_offsets_present) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonDynamicOffsetUnresolved)});
    return true;
  }

  const DescriptorSlotKey descriptor_key{
      {trace.device, table_it->second.table_handle}, slot->slot};
  const auto content =
      FindDescriptorTableSlot(g_descriptor_table_slots, descriptor_key);
  if (!content) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonDescriptorUnknown)});
    return true;
  }
  const auto current_incarnation = FindActiveResourceLifecycle(
      {trace.device, content->resource_handle}).incarnation_id;
  if (!DescriptorSlotContentIsCurrent(*content, current_incarnation)) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonStaleDescriptorBinding)});
    return true;
  }

  key.runtime_resource_incarnation = content->resource_incarnation;
  key.runtime_range_offset = content->offset;
  ObserveMappedCbvValue(key, draw, trace.device, content->resource_handle,
      content->offset, content->size, source.vector_index, source.component);
  return true;
}

bool TryReportPushConstantBackedSource(const CommandListTrace& trace,
    const FadeControlRecordKey& key, RecordedTraceDraw& draw,
    const FadeControlSamplingSource& source) {
  const auto layout_it = g_layout_push_constant_ranges.find(
      wuwa_tfr::TraceLiveHandleKey{trace.device, trace.bound_layout});
  if (layout_it == g_layout_push_constant_ranges.end() ||
      layout_it->second.ranges_truncated)
    return false;
  if (!ResolvePushConstantBackedParam(
          layout_it->second.ranges, source.cbuffer_space,
          source.cbuffer_register))
    return false;
  draw.pending_fade_observations.push_back(
      {key, Unavailable(kFadeControlReasonPushConstantBacked)});
  return true;
}

// Assumes g_fade_control_mutex is already held by the caller -- called once
// per prepared source for a Draw, all under one acquisition rather than
// one per role.
void SampleOneRole(const CommandListTrace& trace,
    const wuwa_tfr::TraceConcreteDrawKey& route, RecordedTraceDraw& draw,
    const FadeControlSamplingSource& source) {
  FadeControlRecordKey key;
  key.route = route;
  key.primitive_index = source.primitive_index;
  key.role = source.role;

  if (!source.resolved) {
    draw.pending_fade_observations.push_back(
        {key, Unavailable(kFadeControlReasonSourceUnresolved)});
    return;
  }

  key.cbuffer_space = source.cbuffer_space;
  key.cbuffer_register = source.cbuffer_register;
  key.vector_index = source.vector_index;
  key.component = source.component;

  if (TrySampleRootPushDescriptorsRoute(trace, key, draw, source)) return;
  if (TrySampleDescriptorTableRoute(trace, key, draw, source)) return;
  if (TryReportPushConstantBackedSource(trace, key, draw, source)) return;
  draw.pending_fade_observations.push_back(
      {key, Unavailable(kFadeControlReasonUnsupportedBindingRoute)});
}

std::string SampleFilenameStem(const std::string& timestamp) {
  return "manual-fade-controls-" + timestamp;
}

std::string SnapshotFilenameStem(const std::string& timestamp) {
  return "manual-fade-snapshots-" + timestamp;
}

std::string BytesToHex(const std::byte* bytes, std::uint64_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.resize(static_cast<std::size_t>(size) * 2);
  for (std::uint64_t i = 0; i < size; ++i) {
    const auto byte = static_cast<unsigned char>(bytes[i]);
    text[static_cast<std::size_t>(i) * 2] = kDigits[byte >> 4];
    text[static_cast<std::size_t>(i) * 2 + 1] = kDigits[byte & 0xF];
  }
  return text;
}

}  // namespace

void RegisterFadeControlRuntimeEvents() {
  reshade::register_event<reshade::addon_event::init_pipeline_layout>(
      OnInitFadeControlPipelineLayout);
  reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(
      OnDestroyFadeControlPipelineLayout);
  reshade::register_event<reshade::addon_event::map_buffer_region>(
      OnMapFadeControlBuffer);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(
      OnUnmapFadeControlBuffer);
  reshade::register_event<reshade::addon_event::destroy_resource>(
      OnDestroyFadeControlResource);
  reshade::register_event<reshade::addon_event::update_descriptor_tables>(
      OnUpdateFadeControlDescriptorTables);
  reshade::register_event<reshade::addon_event::copy_descriptor_tables>(
      OnCopyFadeControlDescriptorTables);
  reshade::register_event<reshade::addon_event::destroy_device>(
      OnDestroyFadeControlDevice);
}

bool FadeControlCapturePending() { return g_fade_control_pending_enabled; }

void SetFadeControlCapturePending(bool enabled) {
  g_fade_control_pending_enabled = enabled;
}

void StartFadeControlCapture(std::uint64_t session_id, bool enabled) {
  std::lock_guard lock(g_fade_control_mutex);
  g_fade_control_session_enabled = enabled;
  if (enabled) {
    g_fade_control_accumulator.Start(session_id);
    g_fade_control_snapshot_accumulator.Start(session_id);
    g_fade_control_capture_diagnostics.Start();
  }
}

bool StopFadeControlCapture() {
  std::lock_guard lock(g_fade_control_mutex);
  if (!g_fade_control_session_enabled) return false;
  g_fade_control_accumulator.Stop();
  g_fade_control_snapshot_accumulator.Stop();
  g_fade_control_capture_diagnostics.Stop();
  return true;
}

FadeControlDiagnosticCounters GetFadeControlDiagnosticCounters() {
  FadeControlDiagnosticCounters counters;
  std::lock_guard lock(g_fade_control_mutex);
  counters.enabled = g_fade_control_session_enabled;
  const auto& result = g_fade_control_accumulator.active()
      ? g_fade_control_accumulator.active_snapshot()
      : g_fade_control_accumulator.last_result();
  counters.control_sources = result.records.size();
  for (const auto& [key, record] : result.records) {
    (void)key;
    if (record.stats.has_available) ++counters.resolved_bindings;
    counters.sampled_values += record.stats.available_observations;
    counters.unavailable_values += record.stats.unavailable_observations;
  }
  counters.capacity_exceeded = result.capacity_exceeded;

  const auto& snapshot_result = g_fade_control_snapshot_accumulator.active()
      ? g_fade_control_snapshot_accumulator.active_snapshot()
      : g_fade_control_snapshot_accumulator.last_result();
  counters.snapshot_count = snapshot_result.snapshots.size();
  counters.snapshot_capacity_exceeded = snapshot_result.capacity_exceeded;

  const auto& tracker_capacity = g_fade_control_capture_diagnostics.active()
      ? g_fade_control_capture_diagnostics.active_diagnostics()
      : g_fade_control_capture_diagnostics.last_result();
  counters.descriptor_slot_capacity_loss = tracker_capacity.descriptor_slot_loss;
  counters.mapped_buffer_capacity_loss = tracker_capacity.mapped_buffer_loss;
  counters.layout_map_capacity_loss = tracker_capacity.layout_map_loss;
  counters.descriptor_range_truncated =
      tracker_capacity.descriptor_range_truncated;
  counters.resource_lifecycle_capacity_loss =
      tracker_capacity.resource_lifecycle_loss;
  return counters;
}

bool WriteFadeControlExport(
    const std::string& timestamp, std::filesystem::path& out_path) {
  FadeControlSnapshot snapshot;
  FadeControlTrackerCapacityDiagnostics tracker_capacity;
  {
    std::lock_guard lock(g_fade_control_mutex);
    if (!g_fade_control_session_enabled) return false;
    snapshot = g_fade_control_accumulator.last_result();
    tracker_capacity = g_fade_control_capture_diagnostics.last_result();
  }

  const auto directory = DumpDir();
  if (directory.empty()) return false;
  const auto filename = AllocateExportFilename(
      SampleFilenameStem(timestamp), ".tsv",
      [&directory](const std::string& candidate) {
        return std::filesystem::exists(directory / candidate);
      });
  if (!filename) return false;
  out_path = directory / *filename;

  std::ofstream report(out_path, std::ios::binary | std::ios::trunc);
  if (!report) return false;

  report << "format\twuwa_tfr_manual_fade_control_capture_v3\n";
  report << "capture_type\tmanual_targeted_fade_control_value_trace\n";
  report << "session_id\t" << snapshot.session_id << '\n';
  report << "record_count\t" << snapshot.records.size() << '\n';
  report << "capacity_exceeded\t"
         << static_cast<int>(snapshot.capacity_exceeded) << '\n';
  report << "tracker_descriptor_slot_capacity_loss\t"
         << static_cast<int>(tracker_capacity.descriptor_slot_loss) << '\n';
  report << "tracker_mapped_buffer_capacity_loss\t"
         << static_cast<int>(tracker_capacity.mapped_buffer_loss) << '\n';
  report << "tracker_layout_map_capacity_loss\t"
         << static_cast<int>(tracker_capacity.layout_map_loss) << '\n';
  report << "tracker_descriptor_range_truncated\t"
         << static_cast<int>(tracker_capacity.descriptor_range_truncated)
         << '\n';
  report << "tracker_resource_lifecycle_capacity_loss\t"
         << static_cast<int>(tracker_capacity.resource_lifecycle_loss) << '\n';
  report << "tracker_capacity_loss_semantics\t"
      "these_five_flags_report_whether_diagnostic_evidence_had_already_been_"
      "dropped_or_truncated_at_the_moment_a_draw_recorded_the_evidence_this_"
      "capture_admitted_the_first_four_cover_fade_controls_own_runtime_"
      "trackers_descriptor_slots_mapped_buffers_layout_cbv_range_maps_and_"
      "the_fifth_covers_the_canonical_resource_lifecycle_owner_shared_with_"
      "trace_pruning_incarnation_records_to_stay_inside_its_capacity_"
      "provenance_is_snapshotted_at_draw_record_time_and_or_ed_in_at_command_"
      "list_submission_only_for_draws_the_session_admits_so_loss_that_never_"
      "reached_admitted_evidence_and_loss_occurring_after_stop_are_both_"
      "excluded_a_true_flag_means_some_rows_reason_bits_1_not_mapped_or_128_"
      "descriptor_unknown_or_64_stale_descriptor_binding_in_this_export_may_"
      "reflect_diagnostic_evidence_loss_rather_than_a_genuine_runtime_miss_"
      "this_is_never_retroactively_attributed_to_individual_records_because_"
      "exact_row_level_causality_is_not_proven_see_unavailable_reason_bits_"
      "for_per_row_detail\n";
  report << "export_timestamp_local\t" << timestamp << '\n';
  report << "value_observation\t"
      "cpu_command_recording_time_observation_of_mapped_constant_buffer_"
      "memory_not_gpu_completion_not_proof_of_value_ultimately_consumed_"
      "by_gpu_if_application_violates_normal_upload_buffer_"
      "synchronization\n";
  report << "sampling_boundary\t"
      "sampled_once_per_recorded_draw_at_command_recording_time_not_at_"
      "queue_submission_time\n";
  report << "record_identity\t"
      "stable_draw_route_plus_pixel_shader_plus_primitive_index_plus_"
      "control_role_plus_proven_static_source_plus_resolved_runtime_cbv_"
      "binding\n";
  report << "control_source_semantics\t"
      "predicate_is_the_cbv_value_gating_entry_to_the_fade_arm_"
      "pre_fade_operand_one_is_the_matched_pre_fade_fmins_first_operand_"
      "the_exact_value_productions_patch_rewrites_to_1_0_"
      "pre_fade_operand_two_is_that_same_fmins_second_operand_left_"
      "byte_identical_by_the_patch_all_three_structurally_proven_from_"
      "dxil_or_absent_pre_fade_operand_one_two_are_only_ever_present_when_"
      "this_instance_also_reached_a_matched_pre_fade_fmin_analysis\n";
  report << "resolved_binding_scope\t"
      "all_five_d3d12_cbv_forms_resolved_push_descriptors_push_descriptors_"
      "with_ranges_push_descriptors_with_ranges_and_flags_descriptor_table_"
      "descriptor_table_with_flags_see_binding_route_column_for_which_"
      "mechanism_a_given_row_used_push_constants_are_not_a_cbv_and_are_"
      "never_sampled_see_reason_bit_256\n";
  report << "unavailable_reason_bits\t"
      "1_not_mapped_2_out_of_range_4_binding_unresolved_8_source_"
      "unresolved_16_unsupported_binding_route_32_dynamic_offset_"
      "unresolved_64_stale_descriptor_binding_128_descriptor_unknown_"
      "256_push_constant_backed_512_declared_cbv_range_exceeded\n";
  report << "declared_cbv_range_policy\t"
      "a_resolved_scalar_is_available_only_if_its_byte_offset_plus_4_bytes_"
      "fits_both_the_mapped_resources_real_extent_and_the_bound_cbvs_own_"
      "declared_size_when_that_size_is_known_d3d12_root_and_push_cbv_"
      "descriptors_carry_no_declared_size_at_all_so_this_check_is_a_no_op_"
      "for_the_root_push_descriptors_binding_route_and_only_the_mapped_"
      "resource_extent_applies_there_see_reason_bit_512_this_bound_is_"
      "never_applied_to_manual_fade_snapshots_tsv_which_stays_clamped_to_"
      "the_mapped_resource_extent_only_see_that_exports_cbv_range_caveat\n";
  report << "dynamic_offset_policy\t"
      "the_d3d12_addon_backend_always_passes_dynamic_offset_count_zero_to_"
      "bind_descriptor_tables_a_nonzero_count_is_still_treated_as_"
      "unresolved_rather_than_assumed_zero_see_reason_bit_32\n";
  report << "descriptor_table_staleness_policy\t"
      "descriptor_table_slot_content_is_cached_at_update_or_copy_"
      "descriptor_tables_time_and_rejected_at_sample_time_if_the_resource_"
      "was_destroyed_and_its_handle_reused_since_see_reason_bit_64\n";
  report << "descriptor_unknown_vs_binding_unresolved\t"
      "binding_unresolved_means_no_table_or_pushed_cbv_is_currently_bound_"
      "to_that_root_parameter_at_all_descriptor_unknown_bit_128_means_the_"
      "table_itself_is_bound_but_this_exact_table_relative_slot_was_never_"
      "observed_via_update_or_copy_descriptor_tables\n";
  report << "mapped_buffer_access_policy\t"
      "read_only_read_write_and_write_only_maps_are_all_observed_the_d3d12_"
      "buffer_map_hook_never_reports_read_only_or_write_discard_write_only_"
      "still_means_real_currently_valid_cpu_visible_memory_the_app_merely_"
      "declared_it_would_not_itself_read_back_this_is_the_persistent_"
      "write_only_upload_buffer_case\n";

  report << "device\tapplication_pso\tpso_incarnation\tpso_context_hash"
      "\tpixel_shader_hash"
      "\tdraw_kind\tgeometry_fingerprint\tpass_fingerprint"
      "\tprimitive_index\tcontrol_role"
      "\tcbuffer_space\tcbuffer_register\tcbuffer_vector_index\tcomponent"
      "\tbinding_route\truntime_resource_incarnation"
      "\truntime_range_offset\tresolved_byte_offset"
      "\tdraw_observations\tavailable_observations\tunavailable_observations"
      "\tfirst_bits\tlast_bits\tchanged\tfinite_min\tfinite_max"
      "\tdistinct_value_count\tdistinct_overflow\tsampled_values"
      "\tunavailable_reason_mask\n";

  for (const auto& [key, record] : snapshot.records) {
    const char* binding_route =
        key.binding_route == FadeControlBindingRoute::RootPushDescriptors
        ? "root_push_descriptors"
        : key.binding_route == FadeControlBindingRoute::DescriptorTable
            ? "descriptor_table"
            : "unresolved";

    report << Hex64(record.pipeline.device) << '\t'
           << Hex64(record.pipeline.application_pipeline) << '\t'
           << record.pipeline.incarnation_id << '\t'
           << Hex64(record.pipeline.context_hash) << '\t'
           << Hex64(record.pipeline.shader_hash) << '\t'
           << TraceDrawKindName(key.route.geometry.kind) << '\t'
           << Hex64(static_cast<std::uint64_t>(
                  wuwa_tfr::TraceGeometryKeyHash{}(key.route.geometry)))
           << '\t' << Hex64(key.route.pass_fingerprint) << '\t'
           << key.primitive_index << '\t'
           << (key.role == FadeControlRole::Predicate ? "predicate"
                  : key.role == FadeControlRole::PreFadeOperandOne
                      ? "pre_fade_operand_one"
                      : "pre_fade_operand_two")
           << '\t' << key.cbuffer_space << '\t' << key.cbuffer_register
           << '\t' << key.vector_index << '\t' << key.component << '\t'
           << binding_route << '\t' << key.runtime_resource_incarnation
           << '\t' << key.runtime_range_offset << '\t'
           << ResolveFadeControlByteOffset(
                  key.runtime_range_offset, key.vector_index, key.component)
           << '\t' << record.stats.draw_observations << '\t'
           << record.stats.available_observations << '\t'
           << record.stats.unavailable_observations << '\t';
    if (record.stats.has_available)
      report << Hex64(record.stats.first_bits) << '\t'
             << Hex64(record.stats.last_bits);
    else
      report << "-\t-";
    report << '\t' << static_cast<int>(record.stats.changed) << '\t';
    if (record.stats.has_finite)
      report << record.stats.finite_min << '\t' << record.stats.finite_max;
    else
      report << "-\t-";
    report << '\t' << record.stats.distinct_value_count << '\t'
           << static_cast<int>(record.stats.distinct_overflow) << '\t';
    for (std::size_t i = 0; i < record.stats.distinct_value_count; ++i) {
      if (i != 0) report << ';';
      report << Hex64(record.stats.distinct_values[i].raw_bits) << ':'
             << record.stats.distinct_values[i].count;
    }
    report << '\t' << static_cast<int>(record.stats.unavailable_reason_mask)
           << '\n';
  }

  report.flush();
  return static_cast<bool>(report);
}

bool WriteFadeControlSnapshotExport(
    const std::string& timestamp, std::filesystem::path& out_path) {
  FadeControlSnapshotSet snapshot_set;
  FadeControlTrackerCapacityDiagnostics tracker_capacity;
  {
    std::lock_guard lock(g_fade_control_mutex);
    if (!g_fade_control_session_enabled) return false;
    snapshot_set = g_fade_control_snapshot_accumulator.last_result();
    tracker_capacity = g_fade_control_capture_diagnostics.last_result();
  }

  const auto directory = DumpDir();
  if (directory.empty()) return false;
  const auto filename = AllocateExportFilename(
      SnapshotFilenameStem(timestamp), ".tsv",
      [&directory](const std::string& candidate) {
        return std::filesystem::exists(directory / candidate);
      });
  if (!filename) return false;
  out_path = directory / *filename;

  std::ofstream report(out_path, std::ios::binary | std::ios::trunc);
  if (!report) return false;

  report << "format\twuwa_tfr_manual_fade_control_snapshot_v2\n";
  report << "capture_type\tmanual_targeted_fade_predicate_cbv_byte_window\n";
  report << "session_id\t" << snapshot_set.session_id << '\n';
  report << "record_count\t" << snapshot_set.snapshots.size() << '\n';
  report << "capacity_exceeded\t"
         << static_cast<int>(snapshot_set.capacity_exceeded) << '\n';
  report << "tracker_descriptor_slot_capacity_loss\t"
         << static_cast<int>(tracker_capacity.descriptor_slot_loss) << '\n';
  report << "tracker_mapped_buffer_capacity_loss\t"
         << static_cast<int>(tracker_capacity.mapped_buffer_loss) << '\n';
  report << "tracker_layout_map_capacity_loss\t"
         << static_cast<int>(tracker_capacity.layout_map_loss) << '\n';
  report << "tracker_descriptor_range_truncated\t"
         << static_cast<int>(tracker_capacity.descriptor_range_truncated)
         << '\n';
  report << "tracker_resource_lifecycle_capacity_loss\t"
         << static_cast<int>(tracker_capacity.resource_lifecycle_loss) << '\n';
  report << "tracker_capacity_loss_semantics\t"
      "these_five_flags_report_whether_diagnostic_evidence_had_already_been_"
      "dropped_or_truncated_at_the_moment_a_draw_recorded_the_evidence_this_"
      "capture_admitted_the_first_four_cover_fade_controls_own_runtime_"
      "trackers_descriptor_slots_mapped_buffers_layout_cbv_range_maps_and_"
      "the_fifth_covers_the_canonical_resource_lifecycle_owner_shared_with_"
      "trace_pruning_incarnation_records_to_stay_inside_its_capacity_"
      "provenance_is_snapshotted_at_draw_record_time_and_or_ed_in_at_command_"
      "list_submission_only_for_draws_the_session_admits_so_loss_that_never_"
      "reached_admitted_evidence_and_loss_occurring_after_stop_are_both_"
      "excluded_this_is_never_retroactively_attributed_to_individual_"
      "snapshots_see_manual_fade_controls_tsv_for_per_row_unavailable_reason_"
      "detail\n";
  report << "export_timestamp_local\t" << timestamp << '\n';
  report << "value_observation\t"
      "cpu_command_recording_time_observation_of_mapped_constant_buffer_"
      "memory_not_gpu_completion_not_proof_of_value_ultimately_consumed_"
      "by_gpu_if_application_violates_normal_upload_buffer_"
      "synchronization\n";
  report << "scope\t"
      "predicate_role_sources_only_that_already_successfully_resolved_and_"
      "sampled_via_manual_fade_controls_tsv_pre_fade_operand_sources_and_"
      "unavailable_predicates_are_never_snapshotted\n";
  report << "dedup_policy\t"
      "at_most_one_snapshot_per_unique_route_primitive_control_role_static_"
      "source_resolved_runtime_binding_first_successful_capture_this_"
      "session_wins_and_is_never_overwritten_by_a_later_draw_of_the_same_"
      "identity\n";
  report << "vector_window_policy\t"
      "vectors_predicate_vector_minus_16_through_predicate_vector_plus_16_"
      "inclusive_16_bytes_per_vector_clamped_to_the_currently_mapped_"
      "regions_real_extent_mapped_range_offset_and_mapped_range_size_"
      "columns_record_exactly_what_that_extent_was\n";
  report << "cbv_range_caveat\t"
      "mapped_range_is_the_only_verified_real_extent_available_for_a_root_"
      "pushed_cbv_d3d12_reports_no_declared_size_at_all_so_the_window_may_"
      "extend_beyond_the_predicates_own_logical_cbuffer_into_neighboring_"
      "data_if_the_application_suballocates_multiple_cbvs_from_one_larger_"
      "mapped_resource\n";
  report << "record_identity\t"
      "identical_to_manual_fade_controls_tsv_stable_draw_route_plus_pixel_"
      "shader_plus_primitive_index_plus_predicate_source_plus_resolved_"
      "runtime_cbv_binding\n";

  report << "device\tapplication_pso\tpso_incarnation\tpso_context_hash"
      "\tpixel_shader_hash"
      "\tdraw_kind\tgeometry_fingerprint\tpass_fingerprint"
      "\tprimitive_index"
      "\tcbuffer_space\tcbuffer_register\tpredicate_vector_index\tcomponent"
      "\tbinding_route\truntime_resource_incarnation"
      "\tcbv_offset\tmapped_range_offset\tmapped_range_size"
      "\tsnapshot_byte_offset\tsnapshot_byte_size"
      "\traw_bytes_hex\tfloat32_values\n";

  for (const auto& [key, record] : snapshot_set.snapshots) {
    const char* binding_route =
        key.binding_route == FadeControlBindingRoute::RootPushDescriptors
        ? "root_push_descriptors"
        : key.binding_route == FadeControlBindingRoute::DescriptorTable
            ? "descriptor_table"
            : "unresolved";

    report << Hex64(record.pipeline.device) << '\t'
           << Hex64(record.pipeline.application_pipeline) << '\t'
           << record.pipeline.incarnation_id << '\t'
           << Hex64(record.pipeline.context_hash) << '\t'
           << Hex64(record.pipeline.shader_hash) << '\t'
           << TraceDrawKindName(key.route.geometry.kind) << '\t'
           << Hex64(static_cast<std::uint64_t>(
                  wuwa_tfr::TraceGeometryKeyHash{}(key.route.geometry)))
           << '\t' << Hex64(key.route.pass_fingerprint) << '\t'
           << key.primitive_index << '\t' << key.cbuffer_space << '\t'
           << key.cbuffer_register << '\t' << key.vector_index << '\t'
           << key.component << '\t' << binding_route << '\t'
           << key.runtime_resource_incarnation << '\t' << record.cbv_offset
           << '\t' << record.mapped_range_offset << '\t'
           << record.mapped_range_size << '\t' << record.window_start << '\t'
           << record.window_size << '\t'
           << BytesToHex(record.raw_bytes.data(), record.window_size) << '\t';

    for (std::uint64_t i = 0; i + 4 <= record.window_size; i += 4) {
      if (i != 0) report << ';';
      float value = 0.0f;
      std::memcpy(&value, record.raw_bytes.data() + i, sizeof(value));
      report << value;
    }
    report << '\n';
  }

  report.flush();
  return static_cast<bool>(report);
}

void SampleFadeControlValuesOnDraw(const CommandListTrace& trace,
    const wuwa_tfr::TraceConcreteDrawKey& route, RecordedTraceDraw& draw) {
  std::shared_ptr<const std::vector<FadeControlSamplingSource>> sources;
  {
    std::lock_guard lock(g_inspection_mutex);
    const auto it = g_inspections.find(draw.pipeline.shader_hash);
    if (it == g_inspections.end() || !it->second.fade_control_sampling_sources)
      return;
    sources = it->second.fade_control_sampling_sources;
  }
  if (sources->empty()) return;

  std::lock_guard lock(g_fade_control_mutex);
  for (const auto& source : *sources) SampleOneRole(trace, route, draw, source);
  FadeControlTrackerCapacityDiagnostics taint = g_fade_control_runtime_taint;
  taint.resource_lifecycle_loss |=
      ResourceLifecycleCapacityTaintSnapshot().evidence_dropped;
  MergeFadeControlTrackerCapacity(draw.pending_fade_tracker_taint, taint);
}

void CommitPendingFadeControlObservations(const RecordedTraceDraw& draw) {
  std::lock_guard lock(g_fade_control_mutex);
  CommitPendingFadeControlObservations(g_fade_control_accumulator,
      g_fade_control_snapshot_accumulator, g_fade_control_capture_diagnostics,
      draw.pipeline, draw.pending_fade_observations,
      draw.pending_fade_snapshots, draw.pending_fade_tracker_taint);
}

}  // namespace wuwa_tfr::dev

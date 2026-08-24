// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "dev/dev_inspection.hpp"

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <string>
#include <utility>

#include "dev/diagnostics/dev_diagnostics.hpp"
#include "addon_shared.hpp"

namespace wuwa_tfr::dev {

namespace {

// Same stable identity target_dither_bypass.cpp's own instance matching
// uses (function, merge SSA name, consumer): enough to re-associate a
// PreFadeFMinEvidence entry with the FadePrimitiveInstance it belongs to
// without relying on either vector's index or ordering.
bool SameFadePrimitiveInstance(const wuwa_tfr::FadePrimitiveInstance& left,
    const wuwa_tfr::FadePrimitiveInstance& right) noexcept {
  return left.function_identity == right.function_identity &&
      left.merge_value == right.merge_value && left.consumer == right.consumer;
}

bool WriteShaderDump(
    std::uint64_t hash,
    std::size_t bytecode_size,
    const std::string& original_ir,
    const wuwa_tfr::SpatialDitherDiagnostic& dither,
    const wuwa_tfr::FadePrimitiveDiagnostic& fade_primitive) {
  if (!g_dump || original_ir.empty()) return false;
  const auto directory = DumpDir();
  if (directory.empty()) return false;

  const std::string base = Hex64(hash);
  const auto ir_path = directory / (base + ".original.ll");
  const auto metadata_path = directory / (base + ".capture.meta.txt");

  std::ofstream ir_file(ir_path, std::ios::binary | std::ios::trunc);
  if (!ir_file) return false;
  ir_file.write(
      original_ir.data(), static_cast<std::streamsize>(original_ir.size()));
  if (!ir_file) return false;
  ir_file.close();

  std::ofstream metadata(metadata_path, std::ios::binary | std::ios::trunc);
  if (!metadata) return false;
  metadata << "format=wuwa_tfr_capture_v1\n";
  // The canonical runtime's own pipeline-initialization path, not this
  // module's former create_pipeline hook -- see dev_inspection.hpp's module
  // comment.
  metadata << "source=wuthering_waves_runtime_init_pipeline\n";
  metadata << "stage=original_pixel_shader\n";
  metadata << "selection=all_unique_dxil_when_dump_enabled\n";
  metadata << "analysis=independent_spatial_dither_diagnostic_v1\n";
  metadata << "mutation=none\n";
  metadata << "source_hash=" << base << "\n";
  metadata << "bytecode_size=" << bytecode_size << "\n";
  metadata << "discard_calls=" << dither.discard_calls << "\n";
  metadata << "strict_spatial_dither_discards="
           << dither.strict_spatial_dither_discards << "\n";
  metadata << "spatial_dither_classification="
           << wuwa_tfr::SpatialDitherClassificationName(
                  dither.classification)
           << "\n";
  metadata << "fade_primitive_detector=Fade Primitive v1\n";
  metadata << "fade_primitive_instances="
           << fade_primitive.instances.size() << "\n";
  for (std::size_t i = 0; i < fade_primitive.instances.size(); ++i) {
    metadata << "fade_primitive_instance_" << i << "_consumer="
             << wuwa_tfr::FadePrimitiveConsumerName(
                    fade_primitive.instances[i].consumer)
             << "\n";
  }
  metadata << "ir_file=" << base << ".original.ll\n";
  return static_cast<bool>(metadata);
}

bool EnvFlag(const wchar_t* name) {
  wchar_t buffer[16]{};
  const DWORD length = GetEnvironmentVariableW(
      name, buffer, static_cast<DWORD>(std::size(buffer)));
  if (length == 0 || length >= std::size(buffer)) return false;
  std::wstring value(buffer, length);
  std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
    return static_cast<wchar_t>(std::towlower(c));
  });
  return value == L"1" || value == L"true" || value == L"yes" ||
      value == L"on";
}

// The Dev-owned observer: every override reads only what the canonical
// runtime already computed and passed in. Nothing here re-derives a DXIL
// disassembly, re-runs AnalyzeFadePrimitiveV1()/AnalyzePreFadeFMinForInstance
// itself, or retains original_ir past the call -- everything this class
// keeps is a value copied out of the observation before it returns.
class InspectionObserverImpl final : public wuwa_tfr::FadePrimitiveRuntimeObserver {
 public:
  void OnShaderPrepared(const ShaderPreparationObservation& observation) override {
    g_unique_dxil_shaders.fetch_add(1, std::memory_order_relaxed);
    if (observation.inspection_succeeded)
      g_disassembly_successes.fetch_add(1, std::memory_order_relaxed);
    else
      g_disassembly_failures.fetch_add(1, std::memory_order_relaxed);

    InspectionRecord record;
    record.inspection_succeeded = observation.inspection_succeeded;
    record.inspection_error = observation.inspection_error;
    record.bytecode_size = observation.original_bytecode_size;
    record.patch_succeeded = observation.patch_succeeded;
    record.patch_failure = observation.patch_failure;
    record.prepared_succeeded = observation.prepared_succeeded;
    record.prepared_failure = observation.prepared_failure;

    record.fade_instances.reserve(observation.fade_primitive.instances.size());
    for (const auto& instance : observation.fade_primitive.instances) {
      FadeInstanceObservation entry;
      entry.instance = instance;
      // Associated by stable identity, never by a shared vector index:
      // pre_fade_evidence may be a strict prefix of fade_primitive.instances
      // (TargetDitherBypassResult::instance_evidence's own documented
      // semantics), so the two vectors are not guaranteed the same length.
      for (const auto& evidence : observation.pre_fade_evidence) {
        if (SameFadePrimitiveInstance(evidence.instance, instance)) {
          entry.pre_fade = evidence.analysis;
          break;
        }
      }
      record.fade_instances.push_back(std::move(entry));
    }

    // Genuinely Dev-only analysis, synchronous, on the observation's own
    // original_ir -- never retained beyond this scope.
    if (observation.inspection_succeeded && observation.original_ir) {
      const std::string& original_ir = *observation.original_ir;
      record.dither = wuwa_tfr::AnalyzeSpatialDitherDiagnostic(original_ir);

      // Diagnostic-only (space, register) enrichment of canonical evidence
      // the matcher itself already extracted -- never a second Fade
      // Primitive/gate/pre-Fade analysis pass, and never influences
      // fade_instances' presence/absence or patch eligibility above. The
      // shared !dx.resources walk (pre_fade_fmin_analysis.hpp's
      // ResolveCbvRangeId) is the single implementation both calls use.
      for (auto& fade_instance : record.fade_instances) {
        wuwa_tfr::ResolveGatePredicateCbvRegister(
            original_ir, fade_instance.instance.gate_predicate);
        if (fade_instance.pre_fade) {
          wuwa_tfr::ResolvePreFadeCbvRegisters(
              original_ir, fade_instance.instance, *fade_instance.pre_fade);
        }
      }

      if (record.dither.discard_calls != 0)
        g_discard_shader_count.fetch_add(1, std::memory_order_relaxed);
      if (record.dither.classification ==
          wuwa_tfr::SpatialDitherClassification::StrictSpatialDither) {
        g_strict_spatial_dither_count.fetch_add(1, std::memory_order_relaxed);
      } else if (record.dither.classification ==
          wuwa_tfr::SpatialDitherClassification::AmbiguousStrictSpatialDither) {
        g_ambiguous_spatial_dither_count.fetch_add(
            1, std::memory_order_relaxed);
      }
      if (!observation.fade_primitive.instances.empty()) {
        g_fade_primitive_shader_count.fetch_add(1, std::memory_order_relaxed);
        g_fade_primitive_instance_count.fetch_add(
            observation.fade_primitive.instances.size(),
            std::memory_order_relaxed);
      }
      record.dumped = WriteShaderDump(observation.original_shader_hash,
          observation.original_bytecode_size, original_ir, record.dither,
          observation.fade_primitive);
      if (record.dumped)
        g_dumped_shaders.fetch_add(1, std::memory_order_relaxed);
    }

    // The canonical runtime's own single-flight shader cache guarantees
    // this fires at most once per unique hash, so this lock only has to
    // protect g_inspections itself from concurrent insertion of different
    // hashes (and from concurrent readers) -- not from two callers racing
    // to build the record for the same hash, which cannot happen. Every
    // Dev-only analysis above therefore runs outside the lock.
    std::lock_guard lock(g_inspection_mutex);
    g_inspections.emplace(observation.original_shader_hash, std::move(record));
  }

  void OnPipelineInit(const PipelineInitObservation& observation) override {
    if (observation.pixel_shader_identified)
      g_seen_shader_callbacks.fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

std::mutex g_inspection_mutex;
std::unordered_map<std::uint64_t, InspectionRecord> g_inspections;

std::atomic<std::uint64_t> g_seen_shader_callbacks{0};
std::atomic<std::uint64_t> g_unique_dxil_shaders{0};
std::atomic<std::uint64_t> g_disassembly_successes{0};
std::atomic<std::uint64_t> g_disassembly_failures{0};
std::atomic<std::uint64_t> g_dumped_shaders{0};

bool g_diagnostic_config_flag = false;
bool g_dump = false;

wuwa_tfr::FadePrimitiveRuntimeObserver* InspectionObserver() {
  static InspectionObserverImpl instance;
  return &instance;
}

void InitializeInspectionConfig() {
  g_diagnostic_config_flag =
      ConfigFlag(L"Diagnostic", EnvFlag(L"WUWA_TFR_DIAGNOSTIC"));
  g_dump = ConfigFlag(L"Dump", EnvFlag(L"WUWA_TFR_DUMP"));
  g_dump_path = ConfigPathValue(L"DumpPath");
}

}  // namespace wuwa_tfr::dev

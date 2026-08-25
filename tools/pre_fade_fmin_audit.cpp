// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"
#include "target_dither_bypass.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct NormalizedCoordinate {
  bool resolved = false;
  std::int64_t byte_offset = 0;
  bool legacy_form = false;
};

NormalizedCoordinate NormalizeOperand(
    const wuwa_tfr::PreFadeOperandSource& source) {
  NormalizedCoordinate result;
  result.legacy_form = source.legacy_form;
  if (!source.resolved) return result;
  if (source.legacy_form) {
    if (source.row_resolved && source.component_resolved) {
      result.resolved = true;
      result.byte_offset = static_cast<std::int64_t>(source.row) * 16 +
          static_cast<std::int64_t>(source.component) * 4;
    }
    return result;
  }
  if (source.byte_offset_resolved) {
    result.resolved = true;
    result.byte_offset = static_cast<std::int64_t>(source.byte_offset);
  }
  return result;
}

NormalizedCoordinate NormalizeGate(
    const wuwa_tfr::FadePrimitiveGatePredicateEvidence& gate) {
  NormalizedCoordinate result;
  result.legacy_form = gate.legacy_form;
  if (!gate.resolved) return result;
  if (gate.legacy_form) {
    if (gate.row_resolved && gate.component_resolved) {
      result.resolved = true;
      result.byte_offset = static_cast<std::int64_t>(gate.row) * 16 +
          static_cast<std::int64_t>(gate.component) * 4;
    }
    return result;
  }
  if (gate.byte_offset_resolved) {
    result.resolved = true;
    result.byte_offset = static_cast<std::int64_t>(gate.byte_offset);
  }
  return result;
}

const char* FormName(const NormalizedCoordinate& coordinate) {
  return coordinate.legacy_form ? "legacy_row" : "byte_offset";
}

std::string CoordinateText(const NormalizedCoordinate& coordinate) {
  return coordinate.resolved ? std::to_string(coordinate.byte_offset) : "-";
}

std::string RegisterText(bool resolved, std::uint32_t space,
    std::uint32_t register_index) {
  if (!resolved) return "-";
  return "space" + std::to_string(space) + ":b" + std::to_string(register_index);
}

const char* RelateBinding(
    const wuwa_tfr::FadePrimitiveGatePredicateEvidence& gate,
    const wuwa_tfr::PreFadeOperandSource& operand) {
  if (!gate.resolved) return "gate_unresolved";
  if (!gate.handle_value.empty() && gate.handle_value == operand.handle_value)
    return "same_handle";
  if (gate.register_resolved && operand.register_resolved &&
      gate.cbuffer_space == operand.cbuffer_space &&
      gate.cbuffer_register == operand.cbuffer_register)
    return "same_register";
  return "non_comparable";
}

bool BindingIsComparable(std::string_view relation) {
  return relation == "same_handle" || relation == "same_register";
}

std::string DeltaText(const NormalizedCoordinate& from,
    const NormalizedCoordinate& to, bool comparable) {
  if (!comparable || !from.resolved || !to.resolved) return "-";
  return std::to_string(to.byte_offset - from.byte_offset);
}

const char* AdjacencyName(wuwa_tfr::PreFadeAdjacency adjacency) {
  switch (adjacency) {
    case wuwa_tfr::PreFadeAdjacency::SameRow: return "same_row";
    case wuwa_tfr::PreFadeAdjacency::CrossRow: return "cross_row";
    case wuwa_tfr::PreFadeAdjacency::NonAdjacent: return "non_adjacent";
    case wuwa_tfr::PreFadeAdjacency::Unknown: return "unknown";
  }
  return "unknown";
}

std::string TsvEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) out += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  return out;
}

bool SameInstanceIdentity(const wuwa_tfr::FadePrimitiveInstance& a,
    const wuwa_tfr::FadePrimitiveInstance& b) noexcept {
  return a.function_identity == b.function_identity &&
      a.merge_value == b.merge_value && a.consumer == b.consumer;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && (argc != 3 || std::string_view(argv[2]) != "--instances")) {
    std::cerr << "usage: pre_fade_fmin_audit <fresh-capture-directory> [--instances]\n";
    return 2;
  }
  const bool list_instances = argc == 3;
  const std::filesystem::path directory = argv[1];

  std::size_t scanned = 0;
  std::size_t shaders_with_primitive = 0;
  std::size_t verified_instances = 0;
  std::size_t qualifying_instances = 0;
  std::size_t instances_with_multiple_candidates = 0;
  std::size_t largest_candidate_count = 0;
  std::size_t shaders_with_shared_rewrite_range = 0;
  std::size_t shaders_patched = 0;
  std::map<std::string, std::size_t> adjacency_distribution;
  std::map<std::string, std::size_t> operand_delta_distribution;
  std::map<std::string, std::size_t> gate_to_operand1_distribution;
  std::map<std::string, std::size_t> gate_to_operand2_distribution;
  std::map<std::string, std::size_t> binding_relation_distribution;
  std::map<std::string, std::size_t> operand_form_distribution;
  std::map<std::string, std::size_t> status_distribution;
  std::map<std::string, std::size_t> fail_reason_distribution;
  std::map<std::string, std::size_t> patch_failure_distribution;
  std::vector<std::string> shared_rewrite_range_shaders;
  std::vector<std::string> multiple_candidate_shaders;
  std::vector<std::string> patch_failure_shaders;

  std::ostringstream instance_rows;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    constexpr std::string_view suffix = ".original.ll";
    if (!name.ends_with(suffix)) continue;
    ++scanned;
    const std::string hash = name.substr(0, name.size() - suffix.size());
    const std::string ir = ReadFile(entry.path());
    const wuwa_tfr::FadePrimitiveDiagnostic diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ir);
    if (diagnostic.instances.empty()) continue;
    ++shaders_with_primitive;

    const wuwa_tfr::TargetDitherBypassResult patched =
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir, diagnostic);

    std::map<std::pair<std::size_t, std::size_t>, std::size_t> rewrite_ranges;
    for (const wuwa_tfr::FadePrimitiveInstance& instance : diagnostic.instances) {
      ++verified_instances;
      wuwa_tfr::PreFadeFMinAnalysis analysis;
      bool reused_patch_analysis = false;
      for (const wuwa_tfr::PreFadeFMinEvidence& evidence : patched.instance_evidence) {
        if (SameInstanceIdentity(evidence.instance, instance)) {
          analysis = evidence.analysis;
          reused_patch_analysis = true;
          break;
        }
      }
      if (!reused_patch_analysis)
        analysis = wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, instance);
      wuwa_tfr::ResolvePreFadeCbvRegisters(ir, instance, analysis);
      wuwa_tfr::FadePrimitiveGatePredicateEvidence gate = instance.gate_predicate;
      wuwa_tfr::ResolveGatePredicateCbvRegister(ir, gate);
      const NormalizedCoordinate operand_one =
          NormalizeOperand(analysis.operand_one);
      const NormalizedCoordinate operand_two =
          NormalizeOperand(analysis.operand_two);
      const NormalizedCoordinate gate_coordinate = NormalizeGate(gate);
      const char* const binding_relation =
          RelateBinding(gate, analysis.operand_one);
      const bool comparable = BindingIsComparable(binding_relation);
      const std::string operand_delta =
          DeltaText(operand_one, operand_two, true);
      const std::string gate_to_operand1 =
          DeltaText(gate_coordinate, operand_one, comparable);
      const std::string gate_to_operand2 =
          DeltaText(gate_coordinate, operand_two, comparable);
      ++status_distribution[wuwa_tfr::PreFadeFMinStatusName(analysis.status)];
      if (wuwa_tfr::PreFadeFMinAnalysisIsStructurallyComplete(analysis)) {
        largest_candidate_count =
            std::max(largest_candidate_count, analysis.qualifying_fmin_count);
        if (analysis.qualifying_fmin_count > 1) ++instances_with_multiple_candidates;
      }
      if (analysis.success) {
        ++qualifying_instances;
        ++adjacency_distribution[AdjacencyName(analysis.adjacency)];
        ++rewrite_ranges[{analysis.operand_one.source_start, analysis.operand_one.source_end}];
        ++operand_delta_distribution[operand_delta];
        ++gate_to_operand1_distribution[gate_to_operand1];
        ++gate_to_operand2_distribution[gate_to_operand2];
        ++binding_relation_distribution[binding_relation];
        ++operand_form_distribution[std::string(FormName(operand_one)) + "/" +
            FormName(operand_two)];
      } else {
        ++fail_reason_distribution[analysis.error];
      }
      if (list_instances) {
        instance_rows << hash << "\t" << instance.function_identity << "\t"
            << wuwa_tfr::FadePrimitiveConsumerName(instance.consumer) << "\t"
            << (analysis.success ? "yes" : "no") << "\t"
            << wuwa_tfr::PreFadeFMinStatusName(analysis.status) << "\t"
            << analysis.qualifying_fmin_count << "\t"
            << AdjacencyName(analysis.adjacency) << "\t"
            << FormName(operand_one) << "\t" << FormName(operand_two) << "\t"
            << CoordinateText(operand_one) << "\t"
            << CoordinateText(operand_two) << "\t" << operand_delta << "\t"
            << RegisterText(analysis.operand_one.register_resolved,
                   analysis.operand_one.cbuffer_space,
                   analysis.operand_one.cbuffer_register)
            << "\t"
            << RegisterText(analysis.operand_two.register_resolved,
                   analysis.operand_two.cbuffer_space,
                   analysis.operand_two.cbuffer_register)
            << "\t" << FormName(gate_coordinate) << "\t"
            << CoordinateText(gate_coordinate) << "\t"
            << RegisterText(gate.register_resolved, gate.cbuffer_space,
                   gate.cbuffer_register)
            << "\t" << gate_to_operand1 << "\t" << gate_to_operand2 << "\t"
            << binding_relation << "\t"
            << TsvEscape(analysis.error) << "\n";
      }
    }
    bool shared_range = false;
    for (const auto& [range, count] : rewrite_ranges)
      if (count > 1) shared_range = true;
    if (shared_range) {
      ++shaders_with_shared_rewrite_range;
      shared_rewrite_range_shaders.push_back(hash);
    }

    if (patched.success) {
      ++shaders_patched;
    } else {
      ++patch_failure_distribution[patched.error];
      if (patch_failure_shaders.size() < 32) patch_failure_shaders.push_back(hash);
    }
  }

  std::cout << "Pre-Fade FMin audit (canonical analyzer)\n";
  std::cout << "total_pixel_shaders_scanned\t" << scanned << "\n";
  std::cout << "shaders_with_verified_primitive\t" << shaders_with_primitive << "\n";
  std::cout << "verified_fade_primitive_instances\t" << verified_instances << "\n";
  std::cout << "qualifying_pre_fade_fmin_instances\t" << qualifying_instances << "\n";
  std::cout << "instances_with_more_than_one_qualifying_candidate\t"
      << instances_with_multiple_candidates << "\n";
  std::cout << "largest_qualifying_candidate_count\t" << largest_candidate_count << "\n";
  std::cout << "shaders_with_two_instances_sharing_a_rewrite_range\t"
      << shaders_with_shared_rewrite_range << "\n";
  std::cout << "shaders_production_patch_succeeded\t" << shaders_patched << "\n";
  std::cout << "analysis_status_distribution\n";
  for (const auto& [name, count] : status_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "adjacency_distribution\n";
  for (const auto& [name, count] : adjacency_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "operand_form_distribution\n";
  for (const auto& [name, count] : operand_form_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "operand2_minus_operand1_distribution\n";
  for (const auto& [name, count] : operand_delta_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "gate_to_operand1_distribution\n";
  for (const auto& [name, count] : gate_to_operand1_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "gate_to_operand2_distribution\n";
  for (const auto& [name, count] : gate_to_operand2_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "gate_binding_relation_distribution\n";
  for (const auto& [name, count] : binding_relation_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "fail_closed_reason_distribution\n";
  for (const auto& [reason, count] : fail_reason_distribution)
    std::cout << reason << "\t" << count << "\n";
  std::cout << "production_patch_failure_distribution\n";
  for (const auto& [reason, count] : patch_failure_distribution)
    std::cout << reason << "\t" << count << "\n";
  if (!shared_rewrite_range_shaders.empty()) {
    std::cout << "shaders_sharing_a_rewrite_range\n";
    for (const std::string& hash : shared_rewrite_range_shaders)
      std::cout << hash << "\n";
  }
  if (!patch_failure_shaders.empty()) {
    std::cout << "production_patch_failure_examples\n";
    for (const std::string& hash : patch_failure_shaders) std::cout << hash << "\n";
  }
  if (list_instances) {
    std::cout << "instances\n";
    std::cout << "hash\tfunction\tconsumer\tqualified\tstatus\tqualifying_fmin_count"
        "\tadjacency\toperand1_form\toperand2_form"
        "\toperand1_coord\toperand2_coord\toperand2_minus_operand1"
        "\toperand1_binding\toperand2_binding"
        "\tgate_form\tgate_coord\tgate_binding"
        "\tgate_to_operand1\tgate_to_operand2\tgate_binding_relation\terror\n";
    std::cout << instance_rows.str();
  }
  return 0;
}

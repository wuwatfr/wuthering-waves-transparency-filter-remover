// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"
#include "target_dither_bypass.hpp"

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
        wuwa_tfr::PatchAllVerifiedFadePrimitiveInstancesPreFadeOperand(ir);

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
            << (analysis.operand_one.row_resolved ? "yes" : "no") << "\t"
            << (analysis.operand_one.component_resolved ? "yes" : "no") << "\t"
            << (analysis.operand_one.byte_offset_resolved ? "yes" : "no") << "\t"
            << (analysis.operand_one.register_resolved ? "yes" : "no") << "\t"
            << analysis.operand_one.cbuffer_space << "\t"
            << analysis.operand_one.cbuffer_register << "\t"
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
        "\tadjacency\toperand1_row_resolved\toperand1_component_resolved"
        "\toperand1_byte_offset_resolved\toperand1_register_resolved"
        "\toperand1_cbuffer_space\toperand1_cbuffer_register\terror\n";
    std::cout << instance_rows.str();
  }
  return 0;
}

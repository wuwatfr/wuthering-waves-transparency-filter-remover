// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors
//
// Offline regression-analysis tool: scans a directory of dumped
// `<hash>.original.ll` pixel shaders (the format written by Dev's capture
// tooling, dev/dev_inspection.cpp's WriteCapture), and reports the canonical
// pre-Fade FMin analysis (pre_fade_fmin_analysis.hpp) for every already
// v1-verified Fade Primitive instance found. Never linked into either addon
// target; Production and Dev runtime behavior do not depend on this tool.
//
// Intended for regression analysis after a Wuthering Waves update: verified
// Fade Primitive population, qualifying pre-Fade FMin coverage, adjacency
// distribution, and any new fail-closed/outlier shapes.

#include "fade_primitive_detector.hpp"
#include "pre_fade_fmin_analysis.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

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
  std::map<std::string, std::size_t> adjacency_distribution;
  std::map<std::string, std::size_t> fail_reason_distribution;

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
    for (const wuwa_tfr::FadePrimitiveInstance& instance : diagnostic.instances) {
      ++verified_instances;
      const wuwa_tfr::PreFadeFMinAnalysis analysis =
          wuwa_tfr::AnalyzePreFadeFMinForInstance(ir, instance);
      if (analysis.success) {
        ++qualifying_instances;
        ++adjacency_distribution[AdjacencyName(analysis.adjacency)];
      } else {
        ++fail_reason_distribution[analysis.error];
      }
      if (list_instances) {
        instance_rows << hash << "\t" << instance.function_identity << "\t"
            << wuwa_tfr::FadePrimitiveConsumerName(instance.consumer) << "\t"
            << (analysis.success ? "yes" : "no") << "\t"
            << analysis.qualifying_fmin_count << "\t"
            << AdjacencyName(analysis.adjacency) << "\t"
            << TsvEscape(analysis.error) << "\n";
      }
    }
  }

  std::cout << "Pre-Fade FMin audit (canonical analyzer)\n";
  std::cout << "total_pixel_shaders_scanned\t" << scanned << "\n";
  std::cout << "shaders_with_verified_primitive\t" << shaders_with_primitive << "\n";
  std::cout << "verified_fade_primitive_instances\t" << verified_instances << "\n";
  std::cout << "qualifying_pre_fade_fmin_instances\t" << qualifying_instances << "\n";
  std::cout << "adjacency_distribution\n";
  for (const auto& [name, count] : adjacency_distribution)
    std::cout << name << "\t" << count << "\n";
  std::cout << "fail_closed_reason_distribution\n";
  for (const auto& [reason, count] : fail_reason_distribution)
    std::cout << reason << "\t" << count << "\n";
  if (list_instances) {
    std::cout << "instances\n";
    std::cout << "hash\tfunction\tconsumer\tqualified\tqualifying_fmin_count\tadjacency\terror\n";
    std::cout << instance_rows.str();
  }
  return 0;
}

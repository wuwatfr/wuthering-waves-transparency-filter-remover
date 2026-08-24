// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

std::vector<std::string> Split(std::string_view text, char delimiter) {
  std::vector<std::string> fields;
  for (std::size_t start = 0;;) {
    const std::size_t end = text.find(delimiter, start);
    fields.emplace_back(text.substr(start, end - start));
    if (end == std::string_view::npos) return fields;
    start = end + 1;
  }
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Presence {
  bool known = false;
  bool normal = false;
  bool partial = false;
  bool full = false;
};

std::unordered_map<std::string, Presence> ReadSubmissionPresence(
    const std::filesystem::path& path) {
  std::unordered_map<std::string, Presence> result;
  std::ifstream input(path);
  std::string line;
  std::unordered_map<std::string, std::size_t> columns;
  while (std::getline(input, line)) {
    const auto fields = Split(line, '\t');
    if (fields.empty() || fields.front() != "device") continue;
    for (std::size_t i = 0; i < fields.size(); ++i) columns.emplace(fields[i], i);
    break;
  }
  const auto shader = columns.find("shader_hash");
  const auto normal = columns.find("normal_submissions");
  const auto partial = columns.find("partial_submissions");
  const auto full = columns.find("full_submissions");
  if (shader == columns.end() || normal == columns.end() ||
      partial == columns.end() || full == columns.end())
    return result;
  while (std::getline(input, line)) {
    const auto fields = Split(line, '\t');
    const std::size_t max_column = std::max({shader->second, normal->second,
        partial->second, full->second});
    if (fields.size() <= max_column) continue;
    try {
      auto& presence = result[fields[shader->second]];
      presence.known = true;
      presence.normal |= std::stoull(fields[normal->second]) != 0;
      presence.partial |= std::stoull(fields[partial->second]) != 0;
      presence.full |= std::stoull(fields[full->second]) != 0;
    } catch (const std::exception&) {
    }
  }
  return result;
}

std::string MaskName(const Presence& presence) {
  std::string mask;
  if (presence.normal) mask += 'N';
  if (presence.partial) mask += 'P';
  if (presence.full) mask += 'F';
  return mask.empty() ? "none" : mask;
}

struct Match {
  std::string hash;
  wuwa_tfr::FadePrimitiveDiagnostic diagnostic;
};

} // namespace

int main(int argc, char** argv) {
  if (argc != 2 && (argc != 3 || std::string_view(argv[2]) != "--instances")) {
    std::cerr << "usage: fade_primitive_audit <fresh-capture-directory> [--instances]\n";
    return 2;
  }
  const bool list_instances = argc == 3;
  const std::filesystem::path directory = argv[1];
  std::vector<Match> matches;
  std::size_t scanned = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    constexpr std::string_view suffix = ".original.ll";
    if (!name.ends_with(suffix)) continue;
    ++scanned;
    const auto diagnostic = wuwa_tfr::AnalyzeFadePrimitiveV1(ReadFile(entry.path()));
    if (!diagnostic.instances.empty())
      matches.push_back({name.substr(0, name.size() - suffix.size()), diagnostic});
  }
  std::sort(matches.begin(), matches.end(), [](const Match& left, const Match& right) {
    return left.hash < right.hash;
  });

  const auto presence = ReadSubmissionPresence(
      directory / "concrete-submission-trace.tsv");
  std::map<std::size_t, std::size_t> instance_distribution;
  std::map<std::string, std::size_t> consumer_distribution;
  std::map<std::string, std::size_t> presence_distribution;
  std::size_t presence_unavailable = 0;
  std::size_t transition_population = 0;

  for (const Match& match : matches) {
    ++instance_distribution[match.diagnostic.instances.size()];
    for (const auto& instance : match.diagnostic.instances)
      ++consumer_distribution[wuwa_tfr::FadePrimitiveConsumerName(instance.consumer)];
    const auto found = presence.find(match.hash);
    if (found == presence.end() || !found->second.known) {
      ++presence_unavailable;
      continue;
    }
    ++presence_distribution[MaskName(found->second)];
    if (!found->second.normal && found->second.partial && found->second.full)
      ++transition_population;
  }

  std::cout << "Fade Primitive v1\n";
  std::cout << "total_pixel_shaders_scanned\t" << scanned << "\n";
  std::cout << "shaders_with_verified_primitive\t" << matches.size() << "\n";
  std::cout << "instances_per_shader\n";
  for (const auto& [instances, count] : instance_distribution)
    std::cout << instances << "\t" << count << "\n";
  std::cout << "downstream_consumers_per_instance\n";
  for (const auto& [consumer, count] : consumer_distribution)
    std::cout << consumer << "\t" << count << "\n";
  std::cout << "execution_presence_for_matched_shaders\n";
  for (const auto& [mask, count] : presence_distribution)
    std::cout << mask << "\t" << count << "\n";
  std::cout << "trace_metadata_unavailable\t" << presence_unavailable << "\n";
  std::cout << "N0_P1_F1\t" << transition_population << "\n";
  if (list_instances) {
    std::cout << "verified_instances\n";
    for (const Match& match : matches) {
      for (std::size_t i = 0; i < match.diagnostic.instances.size(); ++i) {
        std::cout << "instance\t" << match.hash << "\t" << i << "\t"
                  << wuwa_tfr::FadePrimitiveConsumerName(
                         match.diagnostic.instances[i].consumer)
                  << "\n";
      }
    }
  }
  return 0;
}

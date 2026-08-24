// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "fade_primitive_detector.hpp"
#include "trace_submission_presence.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string MaskName(const wuwa_tfr::SubmissionPresence& presence) {
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

constexpr std::string_view kUsage =
    "usage: fade_primitive_audit <fresh-capture-directory> [--instances] "
    "[--concrete-trace <path>]\n";

struct ParsedArgs {
  bool valid = false;
  std::filesystem::path directory;
  bool list_instances = false;
  std::optional<std::string> concrete_trace;
};

ParsedArgs ParseArgs(int argc, char** argv) {
  ParsedArgs parsed;
  if (argc < 2) return parsed;
  parsed.directory = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--instances") {
      parsed.list_instances = true;
    } else if (arg == "--concrete-trace") {
      if (i + 1 >= argc) return {};
      parsed.concrete_trace = argv[++i];
    } else {
      return {};
    }
  }
  parsed.valid = true;
  return parsed;
}

} // namespace

int main(int argc, char** argv) {
  const ParsedArgs args = ParseArgs(argc, argv);
  if (!args.valid) {
    std::cerr << kUsage;
    return 2;
  }
  const bool list_instances = args.list_instances;
  const std::filesystem::path directory = args.directory;
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

  const auto source = wuwa_tfr::ChoosePresenceSource(directory,
      args.concrete_trace, [](const std::filesystem::path& path) {
        return std::filesystem::exists(path);
      });
  std::unordered_map<std::string, wuwa_tfr::SubmissionPresence> presence;
  if (source.kind == wuwa_tfr::PresenceSourceKind::None) {
    std::cerr << "note: no concrete-submission-trace available (pass "
                 "--concrete-trace <path>, or export one via the Trace "
                 "overlay); execution presence will be unavailable for all "
                 "shaders\n";
  } else {
    const auto read = wuwa_tfr::ReadSubmissionPresenceTsv(source.path);
    if (!read.ok) {
      std::cerr << "error: concrete-submission-trace file is missing or "
                    "invalid: " << source.path << "\n";
      if (source.kind == wuwa_tfr::PresenceSourceKind::Explicit) return 2;
      std::cerr << "note: execution presence will be unavailable for all "
                    "shaders\n";
    } else {
      presence = std::move(read.presence);
    }
  }
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

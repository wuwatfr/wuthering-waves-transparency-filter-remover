// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "trace_submission_presence.hpp"

#include <algorithm>
#include <fstream>
#include <string_view>
#include <vector>

namespace wuwa_tfr {

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

}  // namespace

SubmissionPresenceReadResult ReadSubmissionPresenceTsv(
    const std::filesystem::path& path) {
  SubmissionPresenceReadResult result;
  std::ifstream input(path);
  if (!input) return result;

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
  result.ok = true;
  while (std::getline(input, line)) {
    const auto fields = Split(line, '\t');
    const std::size_t max_column = std::max({shader->second, normal->second,
        partial->second, full->second});
    if (fields.size() <= max_column) continue;
    try {
      auto& presence = result.presence[fields[shader->second]];
      presence.known = true;
      presence.normal |= std::stoull(fields[normal->second]) != 0;
      presence.partial |= std::stoull(fields[partial->second]) != 0;
      presence.full |= std::stoull(fields[full->second]) != 0;
    } catch (const std::exception&) {
    }
  }
  return result;
}

PresenceSourceChoice ChoosePresenceSource(const std::filesystem::path& directory,
    const std::optional<std::string>& explicit_arg,
    const std::function<bool(const std::filesystem::path&)>& file_exists) {
  if (explicit_arg)
    return PresenceSourceChoice{PresenceSourceKind::Explicit, *explicit_arg};
  const auto legacy = directory / "concrete-submission-trace.tsv";
  if (file_exists(legacy))
    return PresenceSourceChoice{PresenceSourceKind::Legacy, legacy};
  return PresenceSourceChoice{PresenceSourceKind::None, {}};
}

}  // namespace wuwa_tfr

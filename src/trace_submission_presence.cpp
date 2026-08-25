// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "trace_submission_presence.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
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

bool ParseWholeFieldU64(std::string_view text, std::uint64_t& value) noexcept {
  if (text.empty()) return false;
  constexpr std::uint64_t limit = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t parsed = 0;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (parsed > (limit - digit) / 10) return false;
    parsed = parsed * 10 + digit;
  }
  value = parsed;
  return true;
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
  const std::size_t max_column = std::max({shader->second, normal->second,
      partial->second, full->second});
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto fields = Split(line, '\t');
    if (fields.size() <= max_column) return {};
    const std::string& shader_hash = fields[shader->second];
    std::uint64_t normal_count = 0;
    std::uint64_t partial_count = 0;
    std::uint64_t full_count = 0;
    if (shader_hash.empty() ||
        !ParseWholeFieldU64(fields[normal->second], normal_count) ||
        !ParseWholeFieldU64(fields[partial->second], partial_count) ||
        !ParseWholeFieldU64(fields[full->second], full_count))
      return {};
    auto& presence = result.presence[shader_hash];
    presence.known = true;
    presence.normal |= normal_count != 0;
    presence.partial |= partial_count != 0;
    presence.full |= full_count != 0;
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

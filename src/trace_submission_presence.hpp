// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace wuwa_tfr {

struct SubmissionPresence {
  bool known = false;
  bool normal = false;
  bool partial = false;
  bool full = false;
};

struct SubmissionPresenceReadResult {
  std::unordered_map<std::string, SubmissionPresence> presence;
  // False if the file could not be opened, no header row with all four
  // required columns was ever found, or any non-empty data row after that
  // header was malformed -- as opposed to a valid header followed by zero
  // data rows, which is `true` with an empty map. A malformed row invalidates
  // the whole file: `presence` is then empty, never a partially parsed map.
  bool ok = false;
};

SubmissionPresenceReadResult ReadSubmissionPresenceTsv(
    const std::filesystem::path& path);

enum class PresenceSourceKind { Explicit, Legacy, None };

struct PresenceSourceChoice {
  PresenceSourceKind kind = PresenceSourceKind::None;
  std::filesystem::path path;
};

// Picks which concrete-submission-trace file to read: an explicitly supplied
// path always wins, even if it turns out not to exist -- the caller must
// then fail loudly rather than silently falling back. Otherwise, the legacy
// fixed-name file in `directory` is used if present.
PresenceSourceChoice ChoosePresenceSource(const std::filesystem::path& directory,
    const std::optional<std::string>& explicit_arg,
    const std::function<bool(const std::filesystem::path&)>& file_exists);

}  // namespace wuwa_tfr

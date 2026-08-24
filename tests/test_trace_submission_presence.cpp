// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#include "trace_submission_presence.hpp"

#include <fstream>
#include <optional>

#include "test_check.hpp"

using wuwa_tfr::ChoosePresenceSource;
using wuwa_tfr::PresenceSourceKind;
using wuwa_tfr::ReadSubmissionPresenceTsv;

namespace {

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

int main() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      "wuwa_tfr_trace_submission_presence_tests";
  std::filesystem::create_directories(dir);

  // A valid file, header plus data rows: presence is OR-accumulated across
  // every row sharing the same shader_hash.
  {
    const auto path = dir / "valid.tsv";
    WriteFile(path,
        "format\tsomething\n"
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\textra\n"
        "1\taaa\t0\t1\t0\tx\n"
        "1\taaa\t0\t0\t1\tx\n"
        "1\tbbb\t0\t0\t0\tx\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(result.ok);
    CHECK(result.presence.size() == 2);
    const auto aaa = result.presence.at("aaa");
    CHECK(aaa.known);
    CHECK(!aaa.normal);
    CHECK(aaa.partial);
    CHECK(aaa.full);
    const auto bbb = result.presence.at("bbb");
    CHECK(bbb.known);
    CHECK(!bbb.normal && !bbb.partial && !bbb.full);
  }

  // A missing file: ok is false, not merely an empty result -- callers must
  // be able to distinguish "no evidence" from "nothing captured yet".
  {
    const auto result = ReadSubmissionPresenceTsv(dir / "does-not-exist.tsv");
    CHECK(!result.ok);
    CHECK(result.presence.empty());
  }

  // A file that exists but never has a usable header (missing a required
  // column): also reported as not ok, not a silently-empty valid read.
  {
    const auto path = dir / "malformed.tsv";
    WriteFile(path,
        "format\tsomething\n"
        "device\tshader_hash\tnormal_submissions\n"  // missing partial/full
        "1\taaa\t1\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(!result.ok);
    CHECK(result.presence.empty());
  }

  // A valid header followed by zero data rows: this IS ok, just empty --
  // distinct from the malformed/missing cases above.
  {
    const auto path = dir / "empty.tsv";
    WriteFile(path,
        "format\tsomething\n"
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(result.ok);
    CHECK(result.presence.empty());
  }

  // ChoosePresenceSource: an explicit path always wins, even over an
  // existing legacy file, and even if the explicit path itself does not
  // exist -- the caller is expected to fail loudly on that, not silently
  // fall back to legacy.
  {
    const auto always_exists = [](const std::filesystem::path&) { return true; };
    const auto choice =
        ChoosePresenceSource(dir, std::string("explicit.tsv"), always_exists);
    CHECK(choice.kind == PresenceSourceKind::Explicit);
    CHECK(choice.path == "explicit.tsv");
  }
  {
    const auto never_exists = [](const std::filesystem::path&) { return false; };
    const auto choice =
        ChoosePresenceSource(dir, std::string("explicit.tsv"), never_exists);
    CHECK(choice.kind == PresenceSourceKind::Explicit);
  }

  // No explicit path, legacy file present: Legacy is chosen.
  {
    const auto legacy_exists = [](const std::filesystem::path& path) {
      return path.filename() == "concrete-submission-trace.tsv";
    };
    const auto choice =
        ChoosePresenceSource(dir, std::nullopt, legacy_exists);
    CHECK(choice.kind == PresenceSourceKind::Legacy);
    CHECK(choice.path == dir / "concrete-submission-trace.tsv");
  }

  // No explicit path, no legacy file: None.
  {
    const auto never_exists = [](const std::filesystem::path&) { return false; };
    const auto choice =
        ChoosePresenceSource(dir, std::nullopt, never_exists);
    CHECK(choice.kind == PresenceSourceKind::None);
  }

  std::filesystem::remove_all(dir);
  std::puts("test_trace_submission_presence: all tests passed");
  return 0;
}

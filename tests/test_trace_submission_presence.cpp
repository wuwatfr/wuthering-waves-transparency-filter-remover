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

  // A malformed value in any required numeric field invalidates the whole
  // file. std::stoull would have thrown only after `known` and the earlier
  // bits were already written, leaving partially trusted evidence behind.
  {
    const std::string header =
        "format\tsomething\n"
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n";
    const char* malformed_rows[] = {
        "1\taaa\tzzz\t0\t0\n",   // not a number at all
        "1\taaa\t0\tzzz\t0\n",   // malformed in the second numeric field
        "1\taaa\t0\t0\tzzz\n",   // malformed in the last numeric field
        "1\taaa\t\t0\t0\n",      // empty required field
        "1\taaa\t-1\t0\t0\n",    // stoull would wrap this to a huge non-zero
        "1\taaa\t 1\t0\t0\n",    // leading space
        "1\t\t1\t0\t0\n",        // empty shader_hash
    };
    for (const char* row : malformed_rows) {
      const auto path = dir / "malformed-row.tsv";
      WriteFile(path, header + row);
      const auto result = ReadSubmissionPresenceTsv(path);
      CHECK(!result.ok);
      CHECK(result.presence.empty());
    }
  }

  // A numeric prefix with trailing junk must be rejected, not silently
  // truncated to the prefix the way std::stoull does.
  {
    const auto path = dir / "trailing-junk.tsv";
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t5junk\t0\t0\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(!result.ok);
    CHECK(result.presence.empty());
  }

  // Overflowing an unsigned 64-bit count is malformed, not a saturated or
  // wrapped value.
  {
    const auto path = dir / "overflow.tsv";
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t18446744073709551616\t0\t0\n");  // UINT64_MAX + 1
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(!result.ok);
    CHECK(result.presence.empty());

    // ...while exactly UINT64_MAX still parses and counts as present.
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t18446744073709551615\t0\t0\n");
    const auto at_limit = ReadSubmissionPresenceTsv(path);
    CHECK(at_limit.ok);
    CHECK(at_limit.presence.at("aaa").normal);
  }

  // Too few columns to supply the required fields: a non-empty data row that
  // cannot be parsed is malformed evidence, not a row to skip past.
  {
    const auto path = dir / "short-row.tsv";
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t0\t0\n");  // full_submissions column absent
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(!result.ok);
    CHECK(result.presence.empty());
  }

  // An earlier fully valid row followed by a malformed one yields no usable
  // evidence at all: the good row must not survive as partial truth.
  {
    const auto path = dir / "good-then-malformed.tsv";
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t1\t1\t1\n"
        "1\tbbb\t1\tzzz\t0\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(!result.ok);
    CHECK(result.presence.empty());
    CHECK(result.presence.find("aaa") == result.presence.end());
  }

  // Blank lines between data rows are not data rows and stay harmless.
  {
    const auto path = dir / "blank-lines.tsv";
    WriteFile(path,
        "device\tshader_hash\tnormal_submissions\tpartial_submissions"
        "\tfull_submissions\n"
        "1\taaa\t1\t0\t0\n"
        "\n"
        "1\taaa\t0\t0\t1\n");
    const auto result = ReadSubmissionPresenceTsv(path);
    CHECK(result.ok);
    CHECK(result.presence.size() == 1);
    const auto aaa = result.presence.at("aaa");
    CHECK(aaa.known && aaa.normal && !aaa.partial && aaa.full);
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

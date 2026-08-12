// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <array>
#include <cwctype>
#include <filesystem>
#include <string_view>

namespace wuwa_tfr {

inline bool EqualsCaseInsensitive(std::wstring_view left,
                                  std::wstring_view right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (std::towlower(left[index]) != std::towlower(right[index])) return false;
  }
  return true;
}

inline bool IsWuwaExecutable(const std::filesystem::path& executable) {
  constexpr std::array<std::wstring_view, 4> kRequiredSuffix = {
      L"Client",
      L"Binaries",
      L"Win64",
      L"Client-Win64-Shipping.exe",
  };

  auto component = executable.end();
  for (auto expected = kRequiredSuffix.rbegin();
       expected != kRequiredSuffix.rend(); ++expected) {
    if (component == executable.begin()) return false;
    --component;
    if (!EqualsCaseInsensitive(component->native(), *expected)) return false;
  }
  return true;
}

}  // namespace wuwa_tfr

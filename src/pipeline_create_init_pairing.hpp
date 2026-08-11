// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 WuwaTFR contributors

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace wuwa_tfr {

// ReShade may emit init_pipeline without a matching create_pipeline callback
// (for example when loading from a pipeline library), and may emit init more
// than once for the same handle. Only a paired create/init sequence represents
// a new application pipeline whose replacement mappings should be rebuilt.
struct PipelineCreateIdentity {
  std::uintptr_t device = 0;
  std::uint64_t layout = 0;
  std::uint64_t pixel_shader_hash = 0;
  std::size_t pixel_shader_size = 0;

  bool Valid() const noexcept {
    return device != 0 && pixel_shader_hash != 0 && pixel_shader_size != 0;
  }

  friend bool operator==(
      const PipelineCreateIdentity&,
      const PipelineCreateIdentity&) = default;
};

class PipelineCreateInitPairing {
 public:
  void MarkCreate(PipelineCreateIdentity identity) noexcept {
    pending_identity_ = identity.Valid()
        ? std::optional<PipelineCreateIdentity>(identity)
        : std::nullopt;
  }

  bool ConsumeInit(PipelineCreateIdentity identity) noexcept {
    const bool matches =
        pending_identity_.has_value() && identity.Valid() &&
        *pending_identity_ == identity;
    pending_identity_.reset();
    return matches;
  }

 private:
  std::optional<PipelineCreateIdentity> pending_identity_;
};

} // namespace wuwa_tfr

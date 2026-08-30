#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/nanoavb/nanoavb_aem_symbols.hpp"

#include <optional>
#include <string>
#include <vector>

// The model store: a directory of models persisted as
// the raw READ_DESCRIPTOR response bytes in the standard's own `.aem` blob
// layout — vendor-neutral, byte-verifiable against a live device — keyed
// by fingerprint, with the derived symbol table stored alongside so it is
// regenerated only for unseen fingerprints (§6.3). Any relational or
// handle-space view is a derived index, rebuildable at any time; nothing
// here is on the runtime value path.
//
//   <dir>/<fingerprint>.aem           the blob
//   <dir>/<fingerprint>.symbols.tsv   generator-versioned symbol table

namespace statusbar::nanoavb {

class AemModelStore
{
  public:
    explicit AemModelStore(std::string directory);

    /// Fingerprints the descriptor set, writes the blob and its derived
    /// symbol table (skipping both when the fingerprint is already
    /// stored), and returns the fingerprint. Empty optional on I/O error.
    std::optional<std::string> store(std::span<AemRawDescriptor const> descriptors);

    bool contains(std::string_view fingerprint) const;
    std::optional<std::vector<std::uint8_t>> load_blob(std::string_view fingerprint) const;

    /// The stored symbol table; regenerating instead would risk resolving
    /// an old project through a newer derivation rule (§6.3).
    std::optional<std::vector<AemSymbol>> load_symbols(std::string_view fingerprint) const;

    /// Stored fingerprints, sorted.
    std::vector<std::string> list() const;

    std::string const& directory() const noexcept { return directory_; }

  private:
    std::string blob_path(std::string_view fingerprint) const;
    std::string symbols_path(std::string_view fingerprint) const;

    std::string directory_;
};

}  // namespace statusbar::nanoavb

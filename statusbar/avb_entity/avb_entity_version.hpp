// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

// Build version baked into the entities' ENTITY descriptor firmware_version
// -- what controllers (macOS box info, Hive-style tools) display as the
// device firmware, and what the runtime patches into the served descriptor
// (avb_entity_descriptor_helpers.hpp). The build injects STATUSBAR_AVB_VERSION
// from the repo release tag (see CMakeLists.txt's version block); a build
// without it gets an obvious dev marker rather than a plausible-but-wrong
// number.
#ifndef STATUSBAR_AVB_VERSION
#    define STATUSBAR_AVB_VERSION "0.0.0-dev"
#endif

namespace statusbar::avb_entity {

inline constexpr std::string_view build_version = STATUSBAR_AVB_VERSION;

}  // namespace statusbar::avb_entity

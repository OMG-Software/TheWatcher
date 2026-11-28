#pragma once

#include <string_view>

// meson.build is the single source of truth for the version; it passes it to
// every C++ target as -DTHEWATCHER_VERSION="x.y.z".
#ifndef THEWATCHER_VERSION
#error "THEWATCHER_VERSION must be defined by the build (see meson.build)."
#endif

namespace thewatcher
{
// Release version of the server, agent and common library (semantic version).
inline constexpr std::string_view kVersion = THEWATCHER_VERSION;
} // namespace thewatcher

common_sources = [
  'secure_file.cpp',
  # Stays cross-platform: windows_service.cpp has #ifdef _WIN32 stubs
  # that return "only available on Windows" on Linux/BSD so the
  # server/agent main.cpp call sites still link.
  'windows_service.cpp',
  'signal_guard.cpp',
]

# nlohmann_json comes in via collector_config.hpp's REST-side macros
# (REST stays JSON).
common_lib = static_library(
  'watcher_common',
  common_sources,
  include_directories: root_inc,
  dependencies: [
    cbor_dep,
    sodium_dep,
    json_dep,
    platform_deps,
  ],
)

common_dep = declare_dependency(
  link_with: common_lib,
  include_directories: root_inc,
  dependencies: [
    cbor_dep,
    sodium_dep,
    json_dep,
    platform_deps,
  ],
)

agent_lib = static_library(
  'watcher_agent_lib',
  [
    'agent.cpp',
    'config.cpp',
    'enrollment.cpp',
  ],
  include_directories: root_inc,
  dependencies: [
    collectors_dep,
    common_dep,
    cppzmq_dep,
    sodium_dep,
    platform_deps,
  ],
)

agent_dep = declare_dependency(
  link_with: agent_lib,
  include_directories: root_inc,
  dependencies: [
    collectors_dep,
    common_dep,
    cppzmq_dep,
    sodium_dep,
    platform_deps,
  ],
)

agent_exe = executable(
  'TheWatcherAgent',
  'main.cpp',
  dependencies: agent_dep,
  install: true,
)

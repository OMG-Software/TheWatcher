libpq_dep = dependency('libpq', required: false, static: false)

server_store_sources = [
  'runbook.cpp',
  'status_engine.cpp',
  'store_sqlite.cpp',
  'webhook_security.cpp',
]
server_store_cpp_args = []
server_store_extra_deps = []

if libpq_dep.found()
  server_store_sources += ['store_postgres.cpp']
  server_store_cpp_args += ['-DHAVE_LIBPQ']
  server_store_extra_deps += [libpq_dep]
  message('PostgreSQL support enabled (libpq found)')
else
  message('PostgreSQL support disabled (libpq not found)')
endif

server_config_lib = static_library(
  'watcher_server_config',
  ['config.cpp'],
  include_directories: root_inc,
  dependencies: [
    common_dep,
    sodium_dep,
    cbor_dep,
    platform_deps,
  ],
)

server_config_dep = declare_dependency(
  link_with: server_config_lib,
  include_directories: root_inc,
  dependencies: [
    common_dep,
    sodium_dep,
    cbor_dep,
    platform_deps,
  ],
)

server_store_lib = static_library(
  'watcher_server_store',
  server_store_sources,
  cpp_args: server_store_cpp_args,
  include_directories: root_inc,
  dependencies: [
    common_dep,
    sqlite_dep,
    cbor_dep,
    sodium_dep,
    http_dep,
    json_dep,
    platform_deps,
  ] + server_store_extra_deps,
)

server_store_dep = declare_dependency(
  link_with: server_store_lib,
  include_directories: root_inc,
  compile_args: server_store_cpp_args,
  dependencies: [
    common_dep,
    sqlite_dep,
    cbor_dep,
    sodium_dep,
    http_dep,
    json_dep,
    platform_deps,
  ] + server_store_extra_deps,
)

server_lib = static_library(
  'watcher_server_lib',
  [
    'api.cpp',
    'report_generator.cpp',
    'server.cpp',
    'zap_handler.cpp',
  ],
  include_directories: root_inc,
  dependencies: [
    server_config_dep,
    server_store_dep,
    common_dep,
    cppzmq_dep,
    http_dep,
    sodium_dep,
    json_dep,
    cbor_dep,
    sqlite_dep,
    platform_deps,
  ],
)

server_dep = declare_dependency(
  link_with: server_lib,
  include_directories: root_inc,
  dependencies: [
    server_config_dep,
    server_store_dep,
    common_dep,
    cppzmq_dep,
    http_dep,
    sodium_dep,
    json_dep,
    cbor_dep,
    sqlite_dep,
    platform_deps,
  ],
)

executable(
  'TheWatcherServer',
  'main.cpp',
  dependencies: server_dep,
  install: true,
)

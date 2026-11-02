server_agent_integration_test = executable(
  'server_agent_integration_test',
  ['server_agent_integration_test.cpp'],
  include_directories: root_inc,
  dependencies: [
    agent_dep,
    server_dep,
    common_dep,
    catch2_dep,
    http_dep,
    cppzmq_dep,
    sodium_dep,
    cbor_dep,
    json_dep,
    platform_deps,
  ],
)
test('server_agent_integration', server_agent_integration_test, timeout: 180, is_parallel: false)

server_identity_test = executable(
  'server_identity_test',
  ['server_identity_test.cpp'],
  include_directories: root_inc,
  dependencies: [
    agent_dep,
    server_dep,
    common_dep,
    catch2_dep,
    http_dep,
    cppzmq_dep,
    sodium_dep,
    cbor_dep,
    json_dep,
    platform_deps,
  ],
)
test('server_identity', server_identity_test, timeout: 180, is_parallel: false)

server_collector_config_security_test = executable(
  'server_collector_config_security_test',
  ['server_collector_config_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [
    agent_dep,
    server_dep,
    common_dep,
    catch2_dep,
    http_dep,
    cppzmq_dep,
    sodium_dep,
    cbor_dep,
    json_dep,
    platform_deps,
  ],
)
test('server_collector_config_security', server_collector_config_security_test, timeout: 180, is_parallel: false)

server_access_scope_test = executable(
  'server_access_scope_test',
  ['server_access_scope_test.cpp'],
  include_directories: root_inc,
  dependencies: [
    agent_dep,
    server_dep,
    common_dep,
    catch2_dep,
    http_dep,
    cppzmq_dep,
    sodium_dep,
    cbor_dep,
    json_dep,
    platform_deps,
  ],
)
test('server_access_scope', server_access_scope_test, timeout: 180, is_parallel: false)

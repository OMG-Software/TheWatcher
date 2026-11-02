config_test = executable(
  'agent_config_test',
  ['config_test.cpp'],
  include_directories: root_inc,
  dependencies: [agent_dep, catch2_dep, cbor_dep, platform_deps],
)
test('agent_config', config_test)

collector_contract_test = executable(
  'collector_contract_test',
  ['collector_contract_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('collector_contract', collector_contract_test)

enrollment_test = executable(
  'enrollment_test',
  ['enrollment_test.cpp'],
  include_directories: root_inc,
  dependencies: [agent_dep, catch2_dep, cppzmq_dep, sodium_dep, platform_deps],
)
test('enrollment', enrollment_test, is_parallel: false)

enrollment_security_test = executable(
  'enrollment_security_test',
  ['enrollment_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [agent_dep, catch2_dep, cppzmq_dep, sodium_dep, platform_deps],
)
test('enrollment_security', enrollment_security_test, is_parallel: false)

log_collector_test = executable(
  'log_collector_test',
  ['log_collector_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('log_collector', log_collector_test)

log_collector_security_test = executable(
  'log_collector_security_test',
  ['log_collector_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('log_collector_security', log_collector_security_test)

disk_collector_test = executable(
  'disk_collector_test',
  ['disk_collector_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('disk_collector', disk_collector_test)

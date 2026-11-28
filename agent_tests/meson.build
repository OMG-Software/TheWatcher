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

outbox_test = executable(
  'outbox_test',
  ['outbox_test.cpp'],
  include_directories: root_inc,
  dependencies: [catch2_dep, platform_deps],
)
test('outbox', outbox_test)

agent_limits_test = executable(
  'agent_limits_test',
  ['agent_limits_test.cpp'],
  include_directories: root_inc,
  dependencies: [agent_dep, catch2_dep, cbor_dep, cppzmq_dep, platform_deps],
)
test('agent_limits', agent_limits_test, is_parallel: false)

agent_runtime_test = executable(
  'agent_runtime_test',
  ['agent_runtime_test.cpp'],
  include_directories: root_inc,
  dependencies: [agent_dep, catch2_dep, cbor_dep, cppzmq_dep, sodium_dep, platform_deps],
)
test('agent_runtime', agent_runtime_test, is_parallel: false, timeout: 180)

process_collector_test = executable(
  'process_collector_test',
  ['process_collector_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('process_collector', process_collector_test)

cpu_collector_test = executable(
  'cpu_collector_test',
  ['cpu_collector_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('cpu_collector', cpu_collector_test)

memory_collector_test = executable(
  'memory_collector_test',
  ['memory_collector_test.cpp'],
  include_directories: root_inc,
  dependencies: [collectors_dep, common_dep, catch2_dep, platform_deps],
)
test('memory_collector', memory_collector_test)

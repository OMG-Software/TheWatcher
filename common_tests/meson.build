crypto_test = executable(
  'crypto_test',
  ['crypto_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, platform_deps],
)
test('crypto', crypto_test)

protocol_test = executable(
  'protocol_test',
  ['protocol_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, cbor_dep, platform_deps],
)
test('protocol', protocol_test)

protocol_security_test = executable(
  'protocol_security_test',
  ['protocol_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, cbor_dep, platform_deps],
)
test('protocol_security', protocol_security_test)

singlelog_test = executable(
  'singlelog_test',
  ['singlelog_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, platform_deps],
)
test('singlelog', singlelog_test)

windows_service_test = executable(
  'windows_service_test',
  ['windows_service_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, platform_deps],
)
test('windows_service', windows_service_test)

zmq_retry_test = executable(
  'zmq_retry_test',
  ['zmq_retry_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, cppzmq_dep, platform_deps],
)
test('zmq_retry', zmq_retry_test)

signal_guard_test = executable(
  'signal_guard_test',
  ['signal_guard_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, platform_deps],
)
test('signal_guard', signal_guard_test)

stop_relay_test = executable(
  'stop_relay_test',
  ['stop_relay_test.cpp'],
  include_directories: root_inc,
  dependencies: [common_dep, catch2_dep, platform_deps],
)
test('stop_relay', stop_relay_test)

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

server_config_test = executable(
  'server_config_test',
  ['config_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_config_dep, catch2_dep, cbor_dep, platform_deps],
)
test('server_config', server_config_test)

store_test = executable(
  'store_test',
  ['store_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_store_dep, catch2_dep, sqlite_dep, platform_deps],
)
test('store', store_test)

status_engine_test = executable(
  'status_engine_test',
  ['status_engine_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_store_dep, catch2_dep, cbor_dep, sqlite_dep, platform_deps],
)
test('status_engine', status_engine_test)

webhook_security_test = executable(
  'webhook_security_test',
  ['webhook_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_store_dep, catch2_dep, platform_deps],
)
test('webhook_security', webhook_security_test)

zap_handler_test = executable(
  'zap_handler_test',
  ['zap_handler_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, cppzmq_dep, sodium_dep, platform_deps],
)
test('zap_handler', zap_handler_test)

server_lifecycle_test = executable(
  'server_lifecycle_test',
  ['server_lifecycle_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, cppzmq_dep, sqlite_dep, platform_deps],
)
test('server_lifecycle', server_lifecycle_test, is_parallel: false)

server_transport_test = executable(
  'server_transport_test',
  ['server_transport_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, cppzmq_dep, sqlite_dep, platform_deps],
)
test('server_transport', server_transport_test, is_parallel: false)

zap_identity_test = executable(
  'zap_identity_test',
  ['zap_identity_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, cppzmq_dep, sodium_dep, platform_deps],
)
test('zap_identity', zap_identity_test, is_parallel: false)

server_enrollment_security_test = executable(
  'server_enrollment_security_test',
  ['server_enrollment_security_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, cppzmq_dep, sqlite_dep, sodium_dep, platform_deps],
)
# The tests wait out real 10-second enrollment rate-limit windows, so the
# suite needs a longer timeout than meson's 30s default.
test('server_enrollment_security', server_enrollment_security_test, timeout: 120, is_parallel: false)

login_rate_limit_test = executable(
  'login_rate_limit_test',
  ['login_rate_limit_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, http_dep, cppzmq_dep, sqlite_dep, platform_deps],
)
test('login_rate_limit', login_rate_limit_test, is_parallel: false)

api_server_test = executable(
  'api_server_test',
  ['api_server_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_dep, common_dep, catch2_dep, http_dep, cppzmq_dep, sqlite_dep, platform_deps],
)
test('api_server', api_server_test, is_parallel: false)

store_user_guard_test = executable(
  'store_user_guard_test',
  ['store_user_guard_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_store_dep, catch2_dep, sqlite_dep, platform_deps],
)
test('store_user_guard', store_user_guard_test)

server_config_permissions_test = executable(
  'server_config_permissions_test',
  ['server_config_permissions_test.cpp'],
  include_directories: root_inc,
  dependencies: [server_config_dep, catch2_dep, platform_deps],
)
test('server_config_permissions', server_config_permissions_test)

if libpq_dep.found()
  store_postgres_test = executable(
    'store_postgres_test',
    ['store_postgres_test.cpp'],
    cpp_args: ['-DHAVE_LIBPQ'],
    include_directories: root_inc,
    dependencies: [server_store_dep, catch2_dep, platform_deps],
  )
  test(
    'store_postgres',
    store_postgres_test,
    env: ['THEWATCHER_POSTGRES_TEST_DSN=host=localhost dbname=thewatcher_test user=thewatcher_test password=thewatcher_test'],
  )
endif

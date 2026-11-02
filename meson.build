project(
  'thewatcher',
  ['c', 'cpp'],
  version: '0.8.0',
  default_options: [
    'cpp_std=c++20',
    'default_library=static',
    'prefer_static=true',
    'warning_level=3',
    'werror=true',
    'libcbor:warning_level=0',
    'libcbor:werror=false',
    'sqlite3:warning_level=0',
    'sqlite3:werror=false',
    'nlohmann_json:warning_level=0',
    'nlohmann_json:werror=false',
    'cpp-httplib:warning_level=0',
    'cpp-httplib:werror=false',
    'catch2:warning_level=0',
    'catch2:werror=false',
    'cppzmq:warning_level=0',
    'cppzmq:werror=false',
    'libsodium:warning_level=0',
    'libsodium:werror=false',
    'libzmq:warning_level=0',
    'libzmq:werror=false',
    'force_fallback_for=libcbor,sqlite3,nlohmann_json,cpp-httplib,catch2-with-main,cppzmq',
  ],
)

cpp = meson.get_compiler('cpp')
root_inc = include_directories('.')
third_party_default_options = [
  'warning_level=0',
  'werror=false',
]

add_project_arguments(
  '-DCBOR_BUILD_STATIC=1',
  language: ['c', 'cpp'],
)

if host_machine.system() == 'windows'
  # Prevent <windows.h> from defining min/max as macros; target Win10+ APIs.
  add_project_arguments(
    '-DNOMINMAX',
    '-D_WIN32_WINNT=0x0A00',
    '-DWIN32_LEAN_AND_MEAN',
    language: ['c', 'cpp'],
  )
endif

# libc++ (default on the BSDs and macOS) still gates std::jthread /
# std::stop_token behind -fexperimental-library even though the
# implementation has shipped. Gate the flag on compiler acceptance so it
# is a no-op on libstdc++/gcc.
if host_machine.system() in ['freebsd', 'netbsd', 'openbsd', 'dragonfly', 'darwin']
  if cpp.has_argument('-fexperimental-library')
    add_project_arguments('-fexperimental-library', language: 'cpp')
  endif
endif

thread_dep = dependency('threads')

platform_deps = [thread_dep]

if host_machine.system() == 'windows'
  platform_deps += declare_dependency(link_args: [
    'ws2_32.lib',
    'iphlpapi.lib',
    'shell32.lib',
    'advapi32.lib',
    'pdh.lib',
    'psapi.lib',
  ])
endif

# Local static deps: subprojects/<name>.wrap downloads source archives and builds
# the libraries in this build tree. System/vcpkg packages are not used.
cbor_dep   = dependency('libcbor',          fallback: ['libcbor',       'libcbor_dep'],
                        default_options: third_party_default_options)
sqlite_dep = dependency('sqlite3',          fallback: ['sqlite3',       'sqlite3_dep'],
                        default_options: third_party_default_options)
json_dep   = dependency('nlohmann_json',    fallback: ['nlohmann_json', 'nlohmann_json_dep'],
                        default_options: third_party_default_options)
# cpp-httplib's meson_options.txt defaults `tls`, `zstd`, and `brotli` to
# `auto`, so on a host with libssl-dev / libzstd-dev / libbrotli-dev it
# silently links those libraries into our server. We use plain HTTP only
# (REST API is internal, HTTPS termination is a deployment concern), and
# the auto-discovered OpenSSL is what drags libjitterentropy.a in on
# Ubuntu 24.04+ where libcrypto.a gained that transitive dep.
http_dep   = dependency('cpp-httplib',      fallback: ['cpp-httplib',   'cpp_httplib_dep'],
                        default_options: third_party_default_options +
                          ['tls=disabled', 'zstd=disabled', 'brotli=disabled'])
catch2_dep = dependency('catch2-with-main', fallback: ['catch2',        'catch2_with_main_dep'],
                        default_options: third_party_default_options)

# Prefer system-installed zeromq + libsodium (e.g. FreeBSD pkg install zeromq libsodium)
# and fall back to building from source via cmake subproject when not available.
zmq_system    = dependency('libzmq',    required: false, static: false, method: 'pkg-config')
sodium_system = dependency('libsodium', required: false, static: false, method: 'pkg-config')

if zmq_system.found() and sodium_system.found()
  sodium_dep = sodium_system
  zmq_dep    = declare_dependency(dependencies: [zmq_system, sodium_system])
  meson.override_dependency('libzmq',  zmq_dep)
  meson.override_dependency('zeromq',  zmq_dep)
  # zmq.hpp ships alongside zmq.h in the same package; reuse the include path.
  cppzmq_dep = declare_dependency(dependencies: [zmq_system])
else
  libsodium_project = subproject(
    'libsodium',
    default_options: third_party_default_options + ['default_library=static'],
  )
  sodium_dep = libsodium_project.get_variable('libsodium_dep')
  libsodium_cmake_include_path = meson.current_build_dir() / 'subprojects' / 'zeromq-4.3.5' / 'libsodium-include'

  python = import('python').find_installation()
  run_command(
    python,
    files('scripts/copy_headers.py'),
    meson.current_build_dir() / 'subprojects' / 'libsodium-1.0.20',
    libsodium_cmake_include_path,
    check: true,
  )

  cmake = import('cmake')
  libzmq_options = cmake.subproject_options()
  libzmq_options.set_override_option('warning_level', '0')
  libzmq_options.set_override_option('werror', 'false')
  libzmq_options.add_cmake_defines({
    'BUILD_SHARED': 'OFF',
    'BUILD_STATIC': 'ON',
    'BUILD_TESTS': 'OFF',
    'ENABLE_CURVE': 'ON',
    'WITH_LIBSODIUM': 'ON',
    'WITH_LIBSODIUM_STATIC': 'ON',
    # WSS transport is unused -- TheWatcher's wire crypto is CURVE over
    # libsodium. (libzmq's TLS uses GnuTLS, not OpenSSL; this is not
    # what stops OpenSSL being linked. See the cpp-httplib block above
    # for the actual control point.)
    'WITH_TLS': 'OFF',
    'CMAKE_POSITION_INDEPENDENT_CODE': 'ON',
    'CMAKE_POLICY_VERSION_MINIMUM': '3.5',
    'ENABLE_PRECOMPILED': 'OFF',
    'SODIUM_INCLUDE_DIRS': libsodium_cmake_include_path,
    'SODIUM_LIBRARIES': libsodium_project.get_variable('libsodium_library_path'),
  })
  libzmq_project = cmake.subproject('libzmq', options: libzmq_options)
  zmq_dep = declare_dependency(
    dependencies: [
      libzmq_project.dependency('libzmq-static'),
      sodium_dep,
    ],
  )
  meson.override_dependency('libzmq', zmq_dep)
  meson.override_dependency('zeromq', zmq_dep)

  cppzmq_dep = dependency('cppzmq', fallback: ['cppzmq', 'cppzmq_dep'],
                          default_options: third_party_default_options)
endif

subdir('common')
subdir('agent/collectors')
subdir('agent')
subdir('server')

subdir('common_tests')
subdir('agent_tests')
subdir('server_tests')
subdir('integration_tests')

project(
  'seaside',
  'c',
  'cpp',
  version: '0.1.0-dev',
  license: 'MIT',
  license_files: 'LICENSE',
  meson_version: '>=1.3.0',
  default_options: ['c_std=c17', 'cpp_std=c++23', 'warning_level=3', 'werror=true', 'wrap_mode=nodownload'],
)

if host_machine.system() != 'linux'
  error('Seaside targets Arch Linux; use a Linux build environment.')
endif

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wlroots = dependency('wlroots-0.20', version: ['>=0.20.2', '<0.21.0'], include_type: 'system')
wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm')
threads = dependency('threads')
python = import('python').find_installation('python3')
gtest = dependency('gtest_main', version: '>=1.14.0', method: 'pkg-config')
tomlplusplus = dependency('tomlplusplus', version: ['>=3.4.0', '<3.5.0'], method: 'pkg-config')
xkbcommon = dependency('xkbcommon', method: 'pkg-config')

cpp = meson.get_compiler('cpp')
if get_option('cpp_std') != 'c++23'
  error('Reconfigure the legacy build with -Dcpp_std=c++23; the core now requires C++23.')
endif
if not cpp.compiles('''
  #include <expected>
  #include <thread>
  int main() { std::expected<int, int> value{1}; std::jthread worker; return *value - 1; }
''', args: ['-std=c++23'], name: 'C++23 expected and jthread')
  error('Seaside requires C++23 std::expected and std::jthread support.')
endif

core = static_library(
  'seaside-core',
  ['src/server.cpp', 'src/server_c.cpp', 'src/wlroots_bridge.c'],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman, threads],
  install: false,
)

config_foundation = static_library(
  'seaside-config-foundation',
  ['src/config.cpp', 'src/bindings.cpp', 'src/config_store.cpp',
   'src/config_transaction.cpp', 'src/work_queue.cpp', 'src/state.cpp'],
  include_directories: include_directories('src'),
  dependencies: [tomlplusplus, xkbcommon, wayland_server, threads],
  cpp_args: ['-DTOML_ENABLE_UNRELEASED_FEATURES=0'],
  install: false,
)

headless_fixture = executable(
  'seaside-headless-fixture',
  'tests/integration/headless.c',
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, threads],
  link_with: core,
  install: false,
)

drm_probe = executable(
  'seaside-probe-drm',
  'tools/probe-drm.c',
  dependencies: libdrm,
  install: false,
)

drm_report_fixture = executable(
  'seaside-probe-report-fixture',
  'tests/probe/report-fixture.c',
  dependencies: libdrm,
  install: false,
)

test('headless-lifecycle', headless_fixture, suite: 'integration', timeout: 30)

wrapped_functions = [
  'wl_display_create', 'wl_display_destroy',
  'seaside_wlr_allocator_create', 'seaside_wlr_renderer_init_display',
  'seaside_wlr_compositor_create', 'seaside_wlr_backend_start',
  'seaside_wlr_backend_destroy', 'seaside_wlr_allocator_destroy',
  'seaside_wlr_renderer_destroy',
]
wrap_arguments = []
foreach function : wrapped_functions
  wrap_arguments += '-Wl,--wrap=' + function
endforeach
server_tests = executable(
  'seaside-server-tests',
  ['tests/unit/server_test.cpp', 'tests/unit/wlroots_fixture.c'],
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [wlroots, wayland_server, pixman, threads, gtest],
  link_with: core,
  link_args: wrap_arguments,
  install: false,
)
test('server-ownership', server_tests, suite: 'unit', timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '1', 'SEASIDE_REQUIREMENTS': '2.2,12.2b'},
)
config_tests = executable(
  'seaside-config-tests',
  ['tests/unit/config_test.cpp', 'tests/unit/bindings_test.cpp', 'tests/unit/config_store_test.cpp',
   'tests/unit/config_transaction_test.cpp'],
  include_directories: include_directories('src'),
  dependencies: [tomlplusplus, xkbcommon, wayland_server, threads, gtest],
  cpp_args: ['-DTOML_ENABLE_UNRELEASED_FEATURES=0', '-DSEASIDE_SOURCE_ROOT="' + meson.project_source_root() + '"'],
  link_with: config_foundation,
  install: false,
)
test('part2-config', config_tests, suite: ['unit', 'part2'], timeout: 60,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '2', 'SEASIDE_REQUIREMENTS': '8.1,8.2,8.3,9'},
)
state_tests = executable(
  'seaside-state-tests',
  ['tests/unit/work_queue_test.cpp', 'tests/unit/state_test.cpp'],
  include_directories: include_directories('src'),
  dependencies: [tomlplusplus, xkbcommon, wayland_server, threads, gtest],
  link_with: config_foundation,
  install: false,
)
test('part2-state-workers', state_tests, suite: ['unit', 'part2'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '2', 'SEASIDE_REQUIREMENTS': '2.2,8.1,12.2b'},
)
test(
  'code-quality', python,
  args: [files('tools/check-code'), '--build-dir', meson.current_build_dir()],
  suite: 'quality', timeout: 240, is_parallel: false,
  env: {'SEASIDE_TEST_PART': '2', 'SEASIDE_REQUIREMENTS': '14.3'},
)
test(
  'runner',
  python,
  args: files('tests/runner/test_runner.py'),
  env: {'SEASIDE_HEADLESS_FIXTURE': headless_fixture.full_path(),
        'SEASIDE_TEST_PART': '2', 'SEASIDE_REQUIREMENTS': '12.2b'},
  depends: headless_fixture,
  suite: 'runner',
  timeout: 90,
)
test('drm-capability-observation', drm_probe, suite: 'hardware', timeout: 10, is_parallel: false)
test(
  'drm-probe-contract',
  python,
  args: [
    files('tests/probe/test_probe.py'),
    '--probe', drm_probe.full_path(),
    '--fixture', drm_report_fixture.full_path(),
  ],
  suite: 'unit',
  depends: [drm_probe, drm_report_fixture],
  timeout: 15,
)

summary({
  'wlroots API family': wlroots.version(),
  'Wayland': wayland_server.version(),
  'Protocol XML package': wayland_protocols.version(),
  'libdrm': libdrm.version(),
  'Fixture renderer': 'pixman, offscreen tests only',
  'Installed product targets': 'none in part 1',
  'C++ standard': 'C++23',
  'GoogleTest': gtest.version(),
  'TOML parser (strict 1.0)': tomlplusplus.version(),
  'XKB keysyms': xkbcommon.version(),
}, section: 'Part 1 build baseline')

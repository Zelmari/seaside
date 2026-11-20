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
test('server-ownership', server_tests, suite: 'unit', timeout: 30)
test(
  'code-quality', python,
  args: [files('tools/check-code'), '--build-dir', meson.current_build_dir()],
  suite: 'quality', timeout: 120, is_parallel: false,
)
test(
  'runner',
  python,
  args: files('tests/runner/test_runner.py'),
  env: {'SEASIDE_HEADLESS_FIXTURE': headless_fixture.full_path()},
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
}, section: 'Part 1 build baseline')

project(
  'seaside',
  'c',
  version: '0.1.0-dev',
  meson_version: '>=1.3.0',
  default_options: ['c_std=c17', 'warning_level=3', 'werror=true'],
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

core = static_library(
  'seaside-core',
  'src/server.c',
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman],
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
}, section: 'Part 1 build baseline')

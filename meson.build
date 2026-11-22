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

fs = import('fs')
archive_revision = fs.read('packaging/source-revision').strip()
if archive_revision.startswith('$Format:')
  archive_revision = 'unknown'
endif
git = find_program('git', required: false)
in_checkout = git.found() and fs.exists('.git')
package_version = get_option('package_version')
build_info_data = configuration_data()
build_info_data.set_quoted('SEASIDE_BUILD_VERSION', package_version == '' ? meson.project_version() : package_version)
revision_placeholder = 'SEASIDE_REVISION_PLACEHOLDER'
build_info_data.set_quoted('SEASIDE_BUILD_REVISION', in_checkout ? revision_placeholder : archive_revision)
build_info = configure_file(input: 'src/build_info.hpp.in',
  output: in_checkout ? 'build_info.hpp.in' : 'build_info.hpp',
  configuration: build_info_data,
)
if in_checkout
  build_info = vcs_tag(input: build_info, output: 'build_info.hpp',
    command: [git, 'rev-parse', '--verify', 'HEAD'], fallback: archive_revision,
    replace_string: revision_placeholder,
  )
endif

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')

wlroots = dependency('wlroots-0.20', version: ['>=0.20.2', '<0.21.0'], include_type: 'system')
wayland_server = dependency('wayland-server')
wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
wlr_protocols = dependency('wlr-protocols', method: 'pkg-config')
pixman = dependency('pixman-1')
libdrm = dependency('libdrm')
libinput = dependency('libinput')
threads = dependency('threads')
python = import('python').find_installation('python3')
gtest = dependency('gtest_main', version: '>=1.14.0', method: 'pkg-config')
tomlplusplus = dependency('tomlplusplus', version: ['>=3.4.0', '<3.5.0'], method: 'pkg-config')
nlohmann_json = dependency('nlohmann_json', version: '>=3.11.0', method: 'pkg-config')
xkbcommon = dependency('xkbcommon', method: 'pkg-config')
xau = dependency('xau', method: 'pkg-config')
systemd = dependency('libsystemd', method: 'pkg-config')
pipewire = dependency('libpipewire-0.3', method: 'pkg-config', include_type: 'system')
libpng = dependency('libpng')
turbojpeg = dependency('libturbojpeg', version: '>=3.0.0')
libwebp = dependency('libwebp')
pangocairo = dependency('pangocairo', include_type: 'system')
glib = dependency('glib-2.0', include_type: 'system')
find_program('dbus-daemon')
find_program('pipewire')
wayland_scanner = find_program('wayland-scanner')
layer_xml = wlr_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/wlr-layer-shell-unstable-v1.xml'
layer_server = custom_target(
  'layer-shell-server-header', input: layer_xml, output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
layer_client = custom_target(
  'layer-shell-client-header', input: layer_xml, output: 'wlr-layer-shell-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
layer_client_code = custom_target(
  'layer-shell-client-code', input: layer_xml, output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
idle_inhibit_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml'
idle_inhibit_client = custom_target(
  'idle-inhibit-client-header', input: idle_inhibit_xml, output: 'idle-inhibit-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
idle_inhibit_code = custom_target(
  'idle-inhibit-client-code', input: idle_inhibit_xml, output: 'idle-inhibit-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
display_protocols = []
foreach protocol : ['stable/presentation-time/presentation-time', 'staging/fractional-scale/fractional-scale-v1']
  name = protocol.split('/')[-1]
  xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / (protocol + '.xml')
  display_protocols += custom_target(
    name + '-client-header', input: xml, output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  display_protocols += custom_target(
    name + '-private-code', input: xml, output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach
xdg_shell_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_client_header = custom_target(
  'xdg-shell-client-header', input: xdg_shell_xml, output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_shell_private_code = custom_target(
  'xdg-shell-private-code', input: xdg_shell_xml, output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
activation_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'staging/xdg-activation/xdg-activation-v1.xml'
activation_client_header = custom_target(
  'xdg-activation-client-header', input: activation_xml, output: 'xdg-activation-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
activation_private_code = custom_target(
  'xdg-activation-private-code', input: activation_xml, output: 'xdg-activation-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
seat_protocols = []
foreach name : ['pointer-constraints', 'relative-pointer', 'primary-selection']
  xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable' / name / (name + '-unstable-v1.xml')
  seat_protocols += custom_target(
    name + '-client-header', input: xml, output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  seat_protocols += custom_target(
    name + '-private-code', input: xml, output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

foreach protocol : ['staging/cursor-shape/cursor-shape-v1', 'stable/tablet/tablet-v2']
  name = protocol.split('/')[-1]
  xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / (protocol + '.xml')
  seat_protocols += custom_target(
    name + '-client-header', input: xml, output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  seat_protocols += custom_target(
    name + '-private-code', input: xml, output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

capture_source_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml'
capture_source_server = custom_target(
  'capture-source-server-header', input: capture_source_xml,
  output: 'ext-image-capture-source-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
capture_source_code = custom_target(
  'capture-source-private-code', input: capture_source_xml,
  output: 'ext-image-capture-source-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
foreign_toplevel_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'
foreign_toplevel_code = custom_target(
  'foreign-toplevel-private-code', input: foreign_toplevel_xml,
  output: 'ext-foreign-toplevel-list-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
foreign_toplevel_client = custom_target(
  'foreign-toplevel-client-header', input: foreign_toplevel_xml,
  output: 'ext-foreign-toplevel-list-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
legacy_capture_xml = wlr_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/wlr-screencopy-unstable-v1.xml'
legacy_capture_server = custom_target(
  'legacy-capture-server-header', input: legacy_capture_xml,
  output: 'wlr-screencopy-unstable-v1-server-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)
legacy_capture_client = custom_target(
  'legacy-capture-client-header', input: legacy_capture_xml,
  output: 'wlr-screencopy-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
legacy_capture_code = custom_target(
  'legacy-capture-private-code', input: legacy_capture_xml,
  output: 'wlr-screencopy-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
capture_foundation = static_library(
  'seaside-capture-foundation',
  ['src/output_capture.c', 'src/legacy_capture.c', capture_source_server, capture_source_code,
   foreign_toplevel_code, legacy_capture_server, legacy_capture_code],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman, libdrm], install: false,
)

capture_copy_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'
capture_source_client = custom_target(
  'capture-source-client-header', input: capture_source_xml,
  output: 'ext-image-capture-source-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
capture_copy_client = custom_target(
  'capture-copy-client-header', input: capture_copy_xml,
  output: 'ext-image-copy-capture-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
capture_copy_code = custom_target(
  'capture-copy-private-code', input: capture_copy_xml,
  output: 'ext-image-copy-capture-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
xdg_output_xml = wayland_protocols.get_variable(pkgconfig: 'pkgdatadir') / 'unstable/xdg-output/xdg-output-unstable-v1.xml'
xdg_output_client = custom_target(
  'xdg-output-client-header', input: xdg_output_xml,
  output: 'xdg-output-unstable-v1-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
xdg_output_code = custom_target(
  'xdg-output-private-code', input: xdg_output_xml,
  output: 'xdg-output-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
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

work_foundation = static_library(
  'seaside-work-foundation', 'src/work_queue.cpp',
  include_directories: include_directories('src'),
  dependencies: [wayland_server, threads], install: false,
)

core = static_library(
  'seaside-core',
  ['src/server.cpp', 'src/server_c.cpp', 'src/wlroots_bridge.c',
   'src/scene.cpp', 'src/scene_bridge.c', 'src/layer_scene.c', layer_server,
   'src/window_capture.c', 'src/render_assets.cpp'],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman, threads, libdrm, libpng, turbojpeg, libwebp, pangocairo, glib],
  link_with: work_foundation,
  install: false,
)

capture_test = executable(
  'seaside-output-capture-test',
  ['tests/integration/output_capture.c', capture_source_client, capture_copy_client, capture_copy_code,
   xdg_output_client, xdg_output_code, xdg_shell_client_header, xdg_shell_private_code,
   foreign_toplevel_client, legacy_capture_client],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, libdrm, threads],
  link_with: [capture_foundation, core], install: false,
)
test('part3-output-capture', capture_test, suite: ['integration', 'part3'], timeout: 25,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3.1,7,12.2b'},
)
test('part7-selected-capture', capture_test, args: ['part7'], suite: ['integration', 'part7'],
  timeout: 25, env: {'SEASIDE_TEST_PART': '7', 'SEASIDE_REQUIREMENTS': '7.2,11,12.2b'},
)

tty_dependencies = [wlroots, wayland_server, pixman, libdrm, libinput, xkbcommon,
                    meson.get_compiler('c').find_library('m')]
tty_foundation = static_library(
  'seaside-tty-foundation', ['src/tty_drm.c', 'src/tty_input.c', 'src/cursor_layout.c'],
  include_directories: include_directories('src'),
  dependencies: tty_dependencies, install: false,
)

# GCC's data-flow warnings differ between debug, -O2 and -O3. Exercise the DRM
# cursor path at both shipped optimization levels during ordinary test builds.
foreach level : ['2', '3']
  if get_option('optimization') != level
    static_library(
      'seaside-tty-compile-o' + level, 'src/tty_drm.c',
      dependencies: tty_dependencies, override_options: ['optimization=' + level],
      install: false,
    )
  endif
endforeach

cursor_layout_tests = executable(
  'seaside-cursor-layout-tests', 'tests/unit/cursor_layout_test.cpp',
  include_directories: include_directories('src'),
  dependencies: gtest, link_with: tty_foundation, install: false,
)
test('part6-cursor-layout', cursor_layout_tests, suite: ['unit', 'part6'], timeout: 10,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '5.2,6.2,12.2b'},
)

drm_mode_tests = executable(
  'seaside-part8-drm-modes', 'tests/unit/part8_drm_modes.c',
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, libdrm, libinput, pixman],
  link_with: tty_foundation, install: false,
)
test('part8-drm-modes', drm_mode_tests, suite: ['unit', 'part8'], timeout: 10,
  env: {'SEASIDE_TEST_PART': '8', 'SEASIDE_REQUIREMENTS': '5.2,8.2'},
)

tty_fixture = executable(
  'seaside-tty-fixture', ['tests/integration/tty_fixture.cpp', 'tests/integration/tty_terminal.cpp'],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman, libdrm, threads, xkbcommon],
  link_with: [core, tty_foundation], install: false,
)
test('part3-tty-guard', python, args: [files('tests/integration/test_tty_guard.py'), tty_fixture],
  suite: ['integration', 'part3'], timeout: 15,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3,12.2b'},
)

tty_input_test = executable(
  'seaside-tty-input-test', ['tests/unit/tty_input_test.c', seat_protocols],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, xkbcommon],
  link_with: tty_foundation, install: false,
)
test('part3-tty-input', tty_input_test, args: ['keyboard'], suite: ['unit', 'part3'], timeout: 10,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3,12.2b'},
)
test('part4-pointer-seat', tty_input_test, args: ['pointer'], suite: ['integration', 'part4'], timeout: 10,
  env: {'SEASIDE_TEST_PART': '4', 'SEASIDE_REQUIREMENTS': '4.4,6.2,12.2b'},
)
test('preview2-cursor-input', tty_input_test, args: ['cursor'], suite: ['integration', 'preview2'], timeout: 10,
  env: {'SEASIDE_TEST_PART': '8', 'SEASIDE_REQUIREMENTS': '6.2,12.2b'},
)
test('part7-data-device', tty_input_test, args: ['data-device'], suite: ['integration', 'part7'], timeout: 15,
  env: {'SEASIDE_TEST_PART': '7', 'SEASIDE_REQUIREMENTS': '7.3,11,12.2b'},
)

tty_terminal_test = executable(
  'seaside-tty-terminal-tests', ['tests/unit/tty_terminal_test.cpp', 'tests/integration/tty_terminal.cpp'],
  include_directories: include_directories('tests/unit', 'tests/integration'),
  dependencies: [gtest, threads], install: false,
)
test('part3-tty-terminal-child', tty_terminal_test, suite: ['unit', 'part3'], timeout: 15,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '2.1,3,12.2b'},
)

config_foundation = static_library(
  'seaside-config-foundation',
  ['src/config.cpp', 'src/bindings.cpp', 'src/config_store.cpp',
   'src/config_transaction.cpp', 'src/live_config.cpp', 'src/state.cpp'],
  include_directories: include_directories('src'),
  dependencies: [tomlplusplus, xkbcommon, wayland_server, threads],
  cpp_args: ['-DTOML_ENABLE_UNRELEASED_FEATURES=0'],
  link_with: work_foundation,
  install: false,
)

desktop_foundation = static_library(
  'seaside-desktop-foundation', ['src/desktop.cpp', 'src/layout.cpp', 'src/window_manager.cpp'],
  include_directories: include_directories('src'), install: false,
)

window_manager_tests = executable(
  'seaside-window-manager-tests', 'tests/unit/window_manager_test.cpp',
  include_directories: include_directories('src'),
  dependencies: gtest, link_with: [desktop_foundation, config_foundation], install: false,
)
test('part4-window-policy', window_manager_tests, suite: ['unit', 'part4'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '4', 'SEASIDE_REQUIREMENTS': '2.2,4,5,12.2b'},
)

layout_tests = executable(
  'seaside-layout-tests', 'tests/unit/layout_test.cpp',
  include_directories: include_directories('src'),
  dependencies: gtest, link_with: [desktop_foundation, config_foundation], install: false,
)
test('part5-layout-engines', layout_tests, suite: ['unit', 'part5'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '5', 'SEASIDE_REQUIREMENTS': '4.2,4.3,4.5,12.2b'},
)

layout_policy_tests = executable(
  'seaside-layout-policy-tests', 'tests/unit/part5_window_manager_test.cpp',
  include_directories: include_directories('src'),
  dependencies: gtest, link_with: [desktop_foundation, config_foundation], install: false,
)
test('part5-layout-policy', layout_policy_tests, suite: ['unit', 'part5'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '5', 'SEASIDE_REQUIREMENTS': '4.1,4.5,5,8.2,12.2b'},
)

session_foundation = static_library(
  'seaside-session-foundation',
  ['src/session_activation.cpp', 'src/session_bus.cpp', 'src/session_pipewire.cpp',
   'src/session_portal.cpp', 'src/session_services.cpp', 'src/session_systemd.cpp',
   'src/startup_gate.cpp', 'src/satellite.cpp'],
  include_directories: include_directories('src'),
  dependencies: [systemd, pipewire, wayland_server, threads, xau],
  install: false,
)

executable(
  'seaside-session-check', 'tests/integration/session_services.cpp',
  include_directories: include_directories('src'),
  dependencies: [systemd, pipewire, wayland_server, threads],
  link_with: session_foundation, install: false,
)

startup_gate_tests = executable(
  'seaside-startup-gate-tests',
  'tests/unit/startup_gate_test.cpp',
  include_directories: include_directories('src'),
  dependencies: [systemd, pipewire, wayland_server, threads, gtest],
  link_with: session_foundation,
  link_args: ['-Wl,--wrap=wl_event_loop_add_timer', '-Wl,--wrap=wl_event_source_timer_update',
              '-Wl,--wrap=wl_event_loop_add_idle'],
  install: false,
)
test('part3-startup-gate', startup_gate_tests, suite: ['unit', 'part3'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3,12.2b'},
)

session_bus_tests = executable(
  'seaside-session-bus-tests', 'tests/unit/session_bus_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [systemd, wayland_server, threads, gtest],
  link_with: session_foundation,
  link_args: ['-Wl,--wrap=write', '-Wl,--wrap=wl_event_source_timer_update'],
  install: false,
)
test('part3-session-bus', session_bus_tests, suite: ['unit', 'part3'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '2.1,3.1,12.2b'},
)
session_pipewire_tests = executable(
  'seaside-session-pipewire-tests', 'tests/unit/session_pipewire_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [pipewire, wayland_server, threads, gtest],
  link_with: session_foundation,
  link_args: ['-Wl,--wrap=wl_event_source_timer_update', '-Wl,--wrap=wl_event_loop_add_fd'],
  install: false,
)
test('part3-session-pipewire', session_pipewire_tests, suite: ['unit', 'part3'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '2.1,3.1,12.2b'},
)

session_portal_tests = executable(
  'seaside-session-portal-tests', 'tests/unit/session_portal_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [systemd, wayland_server, threads, gtest],
  link_with: session_foundation, install: false,
)
test('part3-session-portal', session_portal_tests, suite: ['unit', 'part3'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3.1,12.2b'},
)
session_activation_tests = executable(
  'seaside-session-activation-tests', 'tests/unit/session_activation_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [systemd, wayland_server, threads, gtest],
  link_with: session_foundation,
  link_args: ['-Wl,--wrap=access'],
  install: false,
)
test('part3-session-activation', session_activation_tests, suite: ['unit', 'part3', 'part9'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3.1,12.2b'},
)

satellite_tests = executable(
  'seaside-satellite-tests', 'tests/unit/satellite_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [wayland_server, xau, gtest],
  link_with: session_foundation, install: false,
)
test('part7-satellite', satellite_tests, suite: ['unit', 'part7'], timeout: 20,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '7', 'SEASIDE_REQUIREMENTS': '3,11,12.2b'},
)

executable(
  'seaside-part7-x11-client', 'tests/integration/part7_x11_client.c',
  dependencies: dependency('xcb', method: 'pkg-config'), install: false,
)

ipc_foundation = static_library(
  'seaside-ipc-foundation', ['src/ipc.cpp', 'src/ipc_protocol.cpp'],
  include_directories: include_directories('src'),
  dependencies: [nlohmann_json, wayland_server],
  link_with: config_foundation, install: false,
)
seasidectl = executable(
  'seasidectl', ['src/seasidectl.cpp', build_info],
  include_directories: include_directories('src'),
  dependencies: nlohmann_json, link_with: ipc_foundation, install: true,
)
ipc_tests = executable(
  'seaside-ipc-tests', 'tests/unit/ipc_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [gtest, nlohmann_json, wayland_server], link_with: ipc_foundation, install: false,
)
test('part6-ipc', ipc_tests, suite: ['unit', 'part6', 'part9'], timeout: 20,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '10,12.2b'},
)
test('part6-cli', python, args: [files('tests/integration/test_seasidectl.py'), seasidectl],
  suite: ['integration', 'part6'], timeout: 30,
  env: {'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '10,12.2b'},
)

idle_foundation = static_library(
  'seaside-idle-foundation', ['src/idle_power.cpp', 'src/idle_inhibit.c'],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server], install: false,
)
idle_tests = executable(
  'seaside-part9-idle-tests',
  ['tests/unit/idle_power_test.cpp', idle_inhibit_client, idle_inhibit_code],
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [gtest, wlroots, wayland_server, wayland_client],
  link_with: [idle_foundation, core], install: false,
)
test('part9-idle-power', idle_tests, suite: ['unit', 'part9'], timeout: 20,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '9', 'SEASIDE_REQUIREMENTS': '7.4,11,12.2b'},
)

trace_foundation = static_library(
  'seaside-trace-foundation', 'src/trace.cpp',
  include_directories: include_directories('src'),
  dependencies: [nlohmann_json, wayland_server, threads],
  link_with: [work_foundation, config_foundation], install: false,
)
trace_tests = executable(
  'seaside-part9-trace-tests', 'tests/unit/trace_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [gtest, nlohmann_json, wayland_server, threads],
  link_with: trace_foundation, install: false,
)
test('part9-trace', trace_tests, suite: ['unit', 'part9'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '9', 'SEASIDE_REQUIREMENTS': '10.5,10.6,12.1,12.2b'},
)

executable('seaside-part9-panel',
  ['tests/integration/part9_panel.c', layer_client, layer_client_code, xdg_shell_private_code],
  dependencies: wayland_client, install: false,
)

seaside = executable(
  'seaside', ['src/session_entry.cpp', 'src/session_control.cpp', 'src/output_config.cpp', build_info],
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, pixman, libdrm, systemd, pipewire, xkbcommon, xau, threads, nlohmann_json],
  link_with: [core, tty_foundation, config_foundation, capture_foundation, session_foundation,
              desktop_foundation, ipc_foundation, idle_foundation, trace_foundation],
  install: true,
)
install_data('packaging/seaside-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
install_data('packaging/black.png', install_dir: get_option('datadir') / 'seaside' / 'wallpapers')
install_data('packaging/xdg-desktop-portal-wlr/Seaside',
  install_dir: get_option('sysconfdir') / 'xdg' / 'xdg-desktop-portal-wlr',
)
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses' / 'seaside')
install_data('Specification/SEASIDE_CONFIG_EXAMPLE.toml',
  rename: 'config.example.toml', install_dir: get_option('datadir') / 'doc' / 'seaside',
)
test('part3-session-entry-guard', python,
  args: [files('tests/integration/test_session_entry.py'), seaside],
  suite: ['integration', 'part3'], timeout: 15,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3,12.2b'},
)
test('part10-installed-payload', python,
  args: [files('tests/packaging/test_installed_payload.py'), meson.project_source_root(), meson.project_build_root()],
  depends: [seaside, seasidectl], suite: ['integration', 'part10'], timeout: 45,
  env: {'SEASIDE_TEST_PART': '10', 'SEASIDE_REQUIREMENTS': '3,11.1,12.2b'},
)
test('part10-worker-assembly', python,
  args: files('tests/session/test_part10_worker.py'),
  suite: ['runner', 'part10'], timeout: 15,
  env: {'SEASIDE_TEST_PART': '10', 'SEASIDE_REQUIREMENTS': '12.2b'},
)

headless_fixture = executable(
  'seaside-headless-fixture',
  'tests/integration/headless.c',
  include_directories: include_directories('src'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, threads],
  link_with: core,
  install: false,
)

part3_fixture = executable(
  'seaside-part3-fixture',
  ['tests/integration/part3.cpp', 'tests/integration/part3_bridge.c', 'tests/integration/part8_frame_fault.c',
   xdg_shell_client_header, xdg_shell_private_code, display_protocols],
  include_directories: include_directories('src', 'tests/integration'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, libdrm, threads],
  link_args: ['-Wl,--wrap=wlr_output_commit_state'],
  link_with: core, install: false,
)
test('part3-headless-session', part3_fixture, suite: ['integration', 'part3'], timeout: 30,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '2.1,2.2,3,12.2b'},
)
test('part8-display-protocols', part3_fixture, args: ['--display-protocols'],
  suite: ['integration', 'part8'], timeout: 20,
  env: {'SEASIDE_TEST_PART': '8', 'SEASIDE_REQUIREMENTS': '6.1,8.2,11,12.2b'},
)

appearance_tests = executable(
  'seaside-appearance-tests',
  ['tests/unit/render_assets_test.cpp', 'tests/integration/part6_appearance.cpp',
   'tests/integration/part3_bridge.c'],
  include_directories: include_directories('src', 'tests/unit', 'tests/integration'),
  dependencies: [gtest, wlroots, wayland_server, pixman, libdrm, libpng, turbojpeg, libwebp],
  link_with: core, install: false,
)
test('part6-scene-appearance', appearance_tests, suite: ['integration', 'part6'], timeout: 20,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '2,8.1,12.2b'},
)

part6_output_tests = executable(
  'seaside-part6-output-tests',
  ['tests/integration/part6_outputs.cpp', 'tests/integration/part3_bridge.c',
   'src/output_config.cpp', 'src/session_control.cpp'],
  include_directories: include_directories('src', 'tests/unit', 'tests/integration'),
  dependencies: [gtest, wlroots, wayland_server, pixman, libdrm, nlohmann_json],
  link_with: [desktop_foundation, core, tty_foundation, config_foundation, ipc_foundation],
  install: false,
)
test('part8-output-policy', part6_output_tests, suite: ['integration', 'part6', 'part8'], timeout: 20,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '8', 'SEASIDE_REQUIREMENTS': '5.2,5.3,6.1,8.1,8.2,10.6,12.2b'},
)

part4_fixture = executable(
  'seaside-part4-fixture',
  ['tests/integration/part4.cpp', 'tests/integration/part3_bridge.c',
   xdg_shell_client_header, xdg_shell_private_code, activation_client_header, activation_private_code,
   layer_client, layer_client_code],
  include_directories: include_directories('src', 'tests/integration', 'tests/unit'),
  dependencies: [wlroots, wayland_server, wayland_client, pixman, libdrm, xkbcommon, threads, gtest],
  link_with: [desktop_foundation, core, tty_foundation, config_foundation], install: false,
)
test('part4-headless-desktop', part4_fixture, suite: ['integration', 'part4'], timeout: 30,
  env: {'SEASIDE_TEST_PART': '4', 'SEASIDE_REQUIREMENTS': '2.2,4,5,12.2b'},
)
test('part5-headless-dwindle', part4_fixture, args: ['dwindle'], suite: ['integration', 'part5'],
  timeout: 30, env: {'SEASIDE_TEST_PART': '5', 'SEASIDE_REQUIREMENTS': '4.1,4.2,4.5,6.2,12.2b'},
)
test('part5-headless-scrolling', part4_fixture, args: ['scrolling'], suite: ['integration', 'part5'],
  timeout: 30, env: {'SEASIDE_TEST_PART': '5', 'SEASIDE_REQUIREMENTS': '4.3,4.4,5,6.2,12.2b'},
)
test('part6-headless-attention', part4_fixture, args: ['attention'], suite: ['integration', 'part6'],
  timeout: 30, env: {'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '6.1,8.1,10.4,12.2b'},
)
test('part8-fullscreen-aspect', part4_fixture, args: ['fullscreen-aspect'],
  suite: ['integration', 'part8'], timeout: 30,
  env: {'SEASIDE_TEST_PART': '8', 'SEASIDE_REQUIREMENTS': '6.1,6.2,7.2,12.2b'},
)
test('part9-layer-shell', part4_fixture, args: ['layer-shell'], suite: ['integration', 'part9'],
  timeout: 30, env: {'SEASIDE_TEST_PART': '9', 'SEASIDE_REQUIREMENTS': '4.4,6.1,7.1,11,12.2b'},
)

test('part6-binding-reload-during-drag', part4_fixture, args: ['binding-reload'],
  suite: ['integration', 'part6'], timeout: 20,
  env: {'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '4.5,8.1,8.3,12.2b'},
)

portal_smoke = executable(
  'seaside-portal-smoke', ['tests/integration/portal_smoke.cpp', 'tests/integration/part3_bridge.c'],
  include_directories: include_directories('src', 'tests/integration'),
  dependencies: [wlroots, wayland_server, pixman, libdrm, systemd, pipewire, threads],
  link_with: [core, capture_foundation, session_foundation], install: false,
)
portal_stream = executable('seaside-portal-stream', 'tests/integration/portal_stream.cpp',
  dependencies: [systemd, pipewire], install: false,
)
part7_native_client = executable('seaside-part7-native-client',
  ['tests/integration/part7_native_client.c', xdg_shell_client_header, xdg_shell_private_code],
  dependencies: wayland_client, install: false,
)
test('part3-real-portals', python,
  args: [files('tests/integration/test_real_portals.py'), portal_smoke,
         files('packaging/seaside-portals.conf')],
  suite: ['integration', 'part3'], timeout: 35,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3.1,7,12.2b'},
)
test('part3-real-portals-zero-outputs', python,
  args: [files('tests/integration/test_real_portals.py'), portal_smoke,
         files('packaging/seaside-portals.conf'), '--no-outputs'],
  suite: ['integration', 'part3'], timeout: 35,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '3.1,5.2,7.2,12.2b'},
)
test('part7-headless-portal-stream', python,
  args: [files('tests/integration/test_real_portals.py'), portal_smoke,
         files('packaging/seaside-portals.conf'), '--stream-receiver', portal_stream,
         '--native-client', part7_native_client],
  suite: ['integration', 'part7'], timeout: 45,
  env: {'SEASIDE_TEST_PART': '7', 'SEASIDE_REQUIREMENTS': '7.2,11,12.2b'},
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
server_loop_tests = executable(
  'seaside-server-loop-tests', 'tests/unit/server_loop_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [wlroots, wayland_server, pixman, threads, gtest],
  link_with: core, install: false,
)
test('part3-server-loop', server_loop_tests, suite: ['unit', 'part3'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '2.1,3,12.2b'},
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
live_config_tests = executable(
  'seaside-live-config-tests', 'tests/unit/live_config_test.cpp',
  include_directories: include_directories('src', 'tests/unit'),
  dependencies: [tomlplusplus, xkbcommon, wayland_server, threads, gtest],
  link_with: config_foundation, install: false,
)
test('part6-live-config', live_config_tests, suite: ['unit', 'part6'], timeout: 30,
  env: {'SEASIDE_GTEST': '1', 'SEASIDE_TEST_PART': '6', 'SEASIDE_REQUIREMENTS': '8.1,8.2,8.3,9'},
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
  suite: 'quality', timeout: 480, is_parallel: false,
  env: {'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '14.3'},
)
test(
  'runner',
  python,
  args: files('tests/runner/test_runner.py'),
  env: {'SEASIDE_HEADLESS_FIXTURE': headless_fixture.full_path(),
        'SEASIDE_TEST_PART': '3', 'SEASIDE_REQUIREMENTS': '12.2b'},
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
  'Installed product targets': 'seaside TTY session, seasidectl',
  'C++ standard': 'C++23',
  'GoogleTest': gtest.version(),
  'TOML parser (strict 1.0)': tomlplusplus.version(),
  'JSONL IPC library': nlohmann_json.version(),
  'Wallpaper codecs': [libpng.version(), turbojpeg.version(), libwebp.version()],
  'Configuration banner PangoCairo': pangocairo.version(),
  'XKB keysyms': xkbcommon.version(),
  'D-Bus client API': systemd.version(),
  'PipeWire client API': pipewire.version(),
}, section: 'Foundation build baseline')

# chimera-core-neshawk: the C++ NesHawk in miniBox's sandbox, as a Chimera core.
#
# One build description, three flavors, chosen by how it is configured:
#
#   guest (core.wbx)   sh waterbox/setup-guest.sh
#                      ninja -C build/meson-guest core.wbx
#   native reference   meson setup build/meson-native
#                      ninja -C build/meson-native
#   the core's own     the same native configure: cpuFuzz and friends, plus the
#   tools              ROM-free 6502 regression that guards against drift.
#
# Both flavors compile the SAME waterbox.cpp over the SAME header-only core;
# only emulibc and the C++ runtime underneath them differ. That is what makes
# the equivalence gate a proof rather than a smoke test.
project('QuickerNesHawk', 'c', 'cpp',
  version : '0.1.0',
  license : 'MIT',
  default_options : ['cpp_std=c++17', 'warning_level=2',
                     'optimization=2', 'debug=false'])

# Header-only core. Consumers (e.g. jaffarPlus) pull this dependency and #include "source/nes.hpp".
nesHawkDependency = declare_dependency(
  include_directories : include_directories('source'))

# miniBox provides the guest kit headers for the guest and the host library the
# sandbox drivers load. It lives outside this tree, so it arrives as a path.
#
# Only two things need it: the guest (core.wbx) and the sandbox drivers that load
# the guest. The header-only core, the native reference and the standalone tools
# are self-contained, so a plain `meson setup build` with no miniBox in sight
# still configures them and still runs the cpuFuzz regression. Without one the
# drivers are simply not defined, and anything that asks for them by name fails
# loudly rather than silently comparing against nothing.
mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  endif
elif not import('fs').is_dir(mb)
  error('-Dminibox_dir=' + mb + ' is not a directory')
endif

# NESHAWK_FULL_AV is the important one: the core defaults to HEADLESS, which
# compiles out audio synthesis and the pixel pipeline entirely (they have no
# feedback into emulation, which is why the botting builds drop them). A
# frontend needs both, and they cost roughly a third of the frame time.
#
# Exceptions are ON, unlike most waterbox guests: the core reports an unloadable
# rom - unsupported mapper, truncated file - by throwing, and Init catches it and
# fails cleanly instead of emulating nonsense. RTTI stays off; catch-by-type does
# not need it.
core_args = ['-DNESHAWK_FULL_AV', '-fexceptions', '-fno-rtti']

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl + libstdc++ toolchain arrives through the cross file
  # (waterbox/setup-guest.sh); the link recipe is the guest kit's - --no-relax
  # for the large code model, cxxglue and emulibc from the miniBox build, and
  # the four weak pthread symbols musl's static libstdc++ expects to find.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', files('waterbox/waterbox.cpp'),
    name_suffix: 'wbx',
    include_directories: include_directories('source', 'waterbox'),
    cpp_args: core_args,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference and the sandbox drivers -------------------------
  # run-native is the same core built for the host; run-wbx and run-tooling
  # drive the guest through miniBox's host library, which is what they compare
  # against.
  executable('run-native', files('waterbox/run-native.cpp'),
    include_directories: include_directories('source', 'waterbox'),
    cpp_args: core_args)

  if mb == ''
    warning('no miniBox checkout: run-wbx and run-tooling are not built. ' +
            'Pass -Dminibox_dir=<miniBox checkout> to build the sandbox drivers.')
  else
    mbhost = mb / 'build/meson-cpp/source/host'
    foreach driver : ['run-wbx', 'run-tooling']
      executable(driver, files('waterbox' / driver + '.c'),
        c_args: ['-I' + mb / 'source/host'],
        link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
    endforeach
  endif

  # ---- the core's own tools -------------------------------------------------
  # Standalone drivers. Building them is itself a compile-check of the translated core; cpuFuzz is
  # additionally exercised by the ROM-free regression test below. The replay/state tools need a NES
  # ROM at runtime (not shipped) so they are built but not run in CI.
  cpuFuzz = executable('cpuFuzz', 'tools/cpuFuzz.cpp')
  executable('nesReplay',    'tools/nesReplay.cpp')
  executable('stateTest',    'tools/stateTest.cpp')
  executable('resyncReplay', 'tools/resyncReplay.cpp')

  # Deterministic 6502 regression: cpuFuzz's per-cycle trace must match the checked-in golden.
  # No ROM, no .NET, no BizHawk - a fast guard against behavioral drift in the CPU core.
  test('cpufuzz-regression',
    find_program('test/run-cpufuzz-regression.sh'),
    args : [cpuFuzz, files('test/cpuFuzz.golden')])
endif

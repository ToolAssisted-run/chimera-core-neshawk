project('QuickerNesHawk', 'cpp',
  version : '0.1.0',
  license : 'MIT',
  default_options : ['cpp_std=c++17', 'warning_level=2', 'buildtype=release'])

# Header-only core. Consumers (e.g. jaffarPlus) pull this dependency and #include "source/nes.hpp".
nesHawkDependency = declare_dependency(
  include_directories : include_directories('source'))

# Standalone drivers. Building them is itself a compile-check of the translated core; cpuFuzz is
# additionally exercised by the ROM-free regression test below. The replay/state tools need a NES
# ROM at runtime (not shipped) so they are built but not run in CI.
cpuFuzz = executable('cpuFuzz', 'tools/cpuFuzz.cpp')
executable('nesReplay',    'tools/nesReplay.cpp')
executable('stateTest',    'tools/stateTest.cpp')
executable('resyncReplay', 'tools/resyncReplay.cpp')

# Deterministic 6502 regression: cpuFuzz's per-cycle trace must match the checked-in golden.
# No ROM, no .NET, no BizHawk — a fast guard against behavioral drift in the CPU core.
test('cpufuzz-regression',
  find_program('test/run-cpufuzz-regression.sh'),
  args : [cpuFuzz, files('test/cpuFuzz.golden')])

#!/bin/sh
# Builds core.wbx - QuickerNesHawk as a miniHawk waterbox core - plus the drivers the gate uses
# (run-wbx against the sandbox, run-native against the same sources built for the host, run-tooling
# for the optional ABI groups).
#
# Prereq: a miniBox checkout built WITH the C++ guest toolchain, since this core is C++:
#   meson setup <miniBox>/build/meson-cpp -Dguest_cpp=true
#   ninja -C <miniBox>/build/meson-cpp
#
# Usage: ./build-core.sh [-m <miniBox dir>] [-o <output dir>]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
mb="${MINIBOX_DIR:-$HOME/miniBox}"
out="$here/bin"
while getopts "m:o:" opt; do
	case "$opt" in
		m) mb="$OPTARG" ;;
		o) out="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
mb="$(cd "$mb" && pwd)"
mbuild="$mb/build/meson-cpp"
sr="$mbuild/guest-sysroot"
gccver="$(gcc -dumpfullversion)"

[ -f "$sr/lib/libstdc++.a" ] || {
	echo "miniBox C++ guest toolchain missing at $sr." >&2
	echo "Run: meson setup $mbuild $mb -Dguest_cpp=true && ninja -C $mbuild" >&2
	exit 1
}

# The link globs $out/obj/*.o, so a stale object from a renamed or removed source would be linked
# in silently. Start from an empty object dir.
rm -rf "$out/obj"
mkdir -p "$out/obj"

# NESHAWK_FULL_AV is the important one: the core defaults to HEADLESS, which compiles out audio
# synthesis and the pixel pipeline entirely (they have no feedback into emulation, which is why the
# botting builds drop them). A frontend needs both, and they cost roughly a third of the frame time.
#
# Exceptions are ON, unlike most waterbox guests: the core reports an unloadable rom - unsupported
# mapper, truncated file - by throwing, and Init catches it and fails cleanly instead of emulating
# nonsense. RTTI stays off; catch-by-type does not need it.
cflags="-fvisibility=hidden -mcmodel=large -mstack-protector-guard=global \
	-fno-pic -fno-pie -fcf-protection=none -O2 -DNDEBUG"
cxxflags="$cflags -std=c++17 -fexceptions -fno-rtti -DNESHAWK_FULL_AV"
cxxincs="-I$sr/include/c++/$gccver -I$sr/include/c++/$gccver/x86_64-linux-musl"
incs="-I$mb/extern/emulibc -I$mb/source/guest/include -I$mb/extern/jsmn \
	-I$root/source -I$here"
specs="-specs $sr/lib/musl-gcc.specs"

g++ $specs $cxxflags $cxxincs $incs -c -o "$out/obj/waterbox.o" "$here/waterbox.cpp"

# The link recipe (library order, --no-relax, the weak pthread pulls) comes from miniBox's guest
# kit; see source/guest/meson.build there for why each is needed.
ldflags="-static -no-pie -Wl,--eh-frame-hdr,-O2,--no-relax -T $mb/source/guest/linkscript.T \
	-Wl,-u,pthread_once -Wl,-u,pthread_cond_wait -Wl,-u,pthread_cond_broadcast -Wl,-u,pthread_key_create"
g++ $specs -mcmodel=large -fno-pic -fno-pie $ldflags -o "$out/core.wbx" \
	"$out"/obj/*.o "$mbuild/source/guest/cxxglue.c.o" "$mbuild/source/guest/emulibc.c.o" \
	-L"$sr/lib" -lstdc++ -lgcc -lgcc_eh -lc
echo "built $out/core.wbx"

# drivers for the gate
gcc -O2 -Wall -I"$mb/source/host" -o "$out/run-wbx" "$here/run-wbx.c" \
	"$mbuild/source/host/libminiboxhost.so" -Wl,-rpath,"$mbuild/source/host"
g++ -O2 -Wall -std=c++17 -DNESHAWK_FULL_AV -o "$out/run-native" "$here/run-native.cpp"
gcc -O2 -Wall -I"$mb/source/host" -o "$out/run-tooling" "$here/run-tooling.c" \
	"$mbuild/source/host/libminiboxhost.so" -Wl,-rpath,"$mbuild/source/host"
echo "built $out/run-wbx, $out/run-native and $out/run-tooling"

# the package the frontend loads: core.wbx (fixed name) + waterbox.config
cp "$here/waterbox.config" "$out/waterbox.config"
echo "package files ready: $out/core.wbx + $out/waterbox.config"

#!/bin/bash
# The frontend half of the gate: load the package in miniHawk (EmuHawk under Mono, on a private
# Xvfb display), emulate a fixed number of frames with nothing pressed, and require the resulting
# work RAM to be byte-identical to the native reference run. Then change two sync settings the way
# the settings dialog would, and require the machine to change accordingly.
#
# The core-level gate (../run-gate.sh) proves the sandbox changes no emulation. This one proves the
# whole frontend path does not either: package discovery, the generic waterbox adapter, the mounted
# settings channel and the input chain all sit between EmuHawk and the same machine.
#
# Usage:
#   ./run-frontend.sh [--minihawk-root <path>] [--frames N] [rom...]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
frames=600
minihawk_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--minihawk-root) minihawk_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

roms=("$@")
[ ${#roms[@]} -eq 0 ] && roms=("$wb/roms/sprilo.nes")

if [ -z "$minihawk_root" ]; then
	for candidate in "$wb/../../miniHawk" "$HOME/miniHawk"; do
		[ -d "$candidate" ] && { minihawk_root="$candidate"; break; }
	done
fi
[ -n "$minihawk_root" ] && [ -d "$minihawk_root" ] || {
	echo "miniHawk checkout not found; pass --minihawk-root <path>" >&2; exit 1; }
minihawk_root="$(cd "$minihawk_root" && pwd)"

emu_hawk="$minihawk_root/build/EmuHawk.exe"
package="$minihawk_root/build/Cores/quickerneshawk.zip"
[ -f "$emu_hawk" ] || { echo "EmuHawk not built: $emu_hawk" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$wb/bin/run-native" ] || { echo "native reference not built (run ../build-core.sh)" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

export LD_LIBRARY_PATH="$minihawk_root/build/dll:$minihawk_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$minihawk_root" && timeout 120 mono "$emu_hawk" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
# GDI+ display: OpenGL on a hidden display is software rendering, and the display method cannot
# affect emulation anyway
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

# fnv-1a over a file: the same digest run-native prints for each memory domain
hash_file() { python3 "$here/fnv.py" "$1"; }

# run_frontend <tag> <config> <frames> [screenshot path]
# leaves the RAM dump at $work/<tag>.ram.bin; non-zero if the run did not report OK
run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$minihawk_root" && MINIHAWK_JOB="$job" timeout 600 mono "$emu_hawk" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "$rom" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# settings_config <output config> <sync settings JSON> - the same shape the settings dialog writes
settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2"; }

for rom in "${roms[@]}"; do
	name="$(basename "$rom" .nes)"
	if [ ! -f "$rom" ]; then report "$name" SKIP "rom not found"; continue; fi

	# even the baseline run has to say which core it wants; see settings-config.py
	settings_config "$work/config.$name.ini" '{}'

	# --- the machine the frontend builds must be the one the core-level gate signed off on ---
	if ! "$wb/bin/run-native" "$rom" "$frames" --blank > "$work/$name.native.txt" 2>"$work/$name.native.err"; then
		report "$name:frontend" FAIL "native runner error: $(head -1 "$work/$name.native.err")"; continue
	fi
	if ! run_frontend "$name.base" "$work/config.$name.ini" "$frames" "$work/$name.base.png"; then
		report "$name:frontend" FAIL "no OK meta (see tests/work/$name.base.log)"; continue
	fi
	nat="$(grep -o 'domain\[RAM\]=[0-9a-f]*' "$work/$name.native.txt" | cut -d= -f2)"
	got="$(hash_file "$work/$name.base.ram.bin")"
	if [ "$nat" = "$got" ]; then
		report "$name:frontend" PASS "$frames frames, RAM identical to the native reference"
	else
		report "$name:frontend" FAIL "RAM differs (native $nat, frontend $got)"
	fi

	# --- sync settings must reach the guest ---
	# A string setting: the power-on fill IS the RAM's first content, so one frame in the dump has to
	# differ. It runs at frame 1 because by frame 600 the game has overwritten every byte.
	settings_config "$work/config.$name.wram.ini" '{"initialWRamStatePattern": "FF"}'
	if run_frontend "$name.wram0" "$work/config.$name.ini" 1 \
		&& run_frontend "$name.wram1" "$work/config.$name.wram.ini" 1; then
		if cmp -s "$work/$name.wram0.ram.bin" "$work/$name.wram1.ram.bin"; then
			report "$name:settings:wram" FAIL "the RAM fill pattern changed nothing"
		else
			report "$name:settings:wram" PASS "a string sync setting reached the guest"
		fi
	else
		report "$name:settings:wram" FAIL "run did not report OK (see tests/work/$name.wram1.log)"
	fi

	# The region: a PAL machine runs a different frame and shows different scanlines, which the
	# picture shows even where RAM has converged.
	settings_config "$work/config.$name.pal.ini" '{"region": "pal"}'
	if run_frontend "$name.pal" "$work/config.$name.pal.ini" "$frames" "$work/$name.pal.png"; then
		if cmp -s "$work/$name.base.png" "$work/$name.pal.png"; then
			report "$name:settings:region" FAIL "PAL produced the same picture as NTSC"
		else
			report "$name:settings:region" PASS "the region setting reached the guest"
		fi
	else
		report "$name:settings:region" FAIL "run did not report OK (see tests/work/$name.pal.log)"
	fi
done

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0

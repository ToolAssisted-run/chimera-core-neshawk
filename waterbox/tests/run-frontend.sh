#!/bin/bash
# The frontend half of the gate: load the package in Chimera's
# window (run headless, it needs no display), emulate a fixed number of frames with nothing pressed, and require the resulting
# work RAM to be byte-identical to the native reference run. Then change two sync settings the way
# the settings dialog would, and require the machine to change accordingly.
#
# The core-level gate (../run-gate.sh) proves the sandbox changes no emulation. This one proves the
# whole frontend path does not either: package discovery, the generic waterbox adapter, the mounted
# settings channel and the input chain all sit between the window and the same machine.
#
# Usage:
#   ./run-frontend.sh [--chimera-root <path>] [--frames N] [rom...]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
frames=600
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

roms=("$@")
[ ${#roms[@]} -eq 0 ] && roms=("$wb/roms/sprilo.nes")

if [ -z "$chimera_root" ]; then
	for candidate in "$wb/../../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/quickerneshawk.chimeraCore"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
nat="$wb/../build/meson-native"
[ -x "$nat/run-native" ] || { echo "native reference not built: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

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
	local project
	project="$(game_project "$tag" "$cfg" "$package" "$rom")" || { echo "no project made (see tests/work/project.$tag/make.log)" > "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout -s KILL 600 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$package" \
		"--lua=$here/frontend-ram.lua" "--project=$project" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# settings_config <output settings file> <sync settings JSON>
# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings
# settings_config was given for the config it runs with.
declare -A settings_of
settings_config() { printf '{}' > "$1"; settings_of["$1"]="$2"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/project.$1" settings="${settings_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	rm -rf "$dir" && mkdir -p "$dir" && cp "$game" "$dir/"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$dir/$(basename "$game")" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

for rom in "${roms[@]}"; do
	name="$(basename "$rom" .nes)"
	if [ ! -f "$rom" ]; then report "$name" SKIP "rom not found"; continue; fi

	settings_config "$work/settings.$name.json" '{}'

	# --- the machine the frontend builds must be the one the core-level gate signed off on ---
	if ! "$nat/run-native" "$rom" "$frames" --blank > "$work/$name.native.txt" 2>"$work/$name.native.err"; then
		report "$name:frontend" FAIL "native runner error: $(head -1 "$work/$name.native.err")"; continue
	fi
	if ! run_frontend "$name.base" "$work/settings.$name.json" "$frames" "$work/$name.base.png"; then
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
	settings_config "$work/settings.$name.wram.json" '{"initialWRamStatePattern": "FF"}'
	if run_frontend "$name.wram0" "$work/settings.$name.json" 1 \
		&& run_frontend "$name.wram1" "$work/settings.$name.wram.json" 1; then
		if cmp -s "$work/$name.wram0.ram.bin" "$work/$name.wram1.ram.bin"; then
			report "$name:settings:wram" FAIL "the RAM fill pattern changed nothing"
		else
			report "$name:settings:wram" PASS "a string sync setting reached the guest"
		fi
	else
		report "$name:settings:wram" FAIL "run did not report OK (see tests/work/$name.wram1.log)"
	fi

	# --- the bindings the package ships are the machine's when nobody changed them ---
	# The window keeps only the keys a person changed, so with fresh settings the machine's bindings
	# are the package's: dump-controls.lua writes the ones the machine has.
	# (On a machine that also has the quickerNES package installed, both declare "NES Controller"
	# and agree on player 1; the run names this package, so it is the one that is read.)
	printf 'out=%s/%s.keys.controls.json\nmeta=%s/%s.keys.meta.txt\n' "$work" "$name" "$work" "$name" > "$work/job.$name.keys.txt"
	rm -f "$work/$name.keys.controls.json" "$work/$name.keys.meta.txt"
	settings_config "$work/settings.$name.keys.json" '{}'
	if project="$(game_project "$name.keys" "$work/settings.$name.keys.json" "$package" "$rom")" \
		&& ( cd "$chimera_root" && MINIHAWK_JOB="$work/job.$name.keys.txt" timeout -s KILL 600 "$chimera_exe" --headless \
			"--config=$work/settings.$name.keys.json" "--core=$package" \
			"--lua=$here/dump-controls.lua" "--project=$project" ) > "$work/$name.keys.log" 2>&1 \
		&& grep -q "^status=OK" "$work/$name.keys.meta.txt" 2>/dev/null; then
		if python3 "$here/check-keybinds.py" "$work/$name.keys.controls.json" \
			"$wb/default_keybinds.json" "NES Controller" > "$work/$name.keys.txt" 2>&1; then
			report "$name:keybinds" PASS "$(cat "$work/$name.keys.txt")"
		else
			report "$name:keybinds" FAIL "$(head -1 "$work/$name.keys.txt")"
		fi
	else
		report "$name:keybinds" FAIL "run did not report OK (see tests/work/$name.keys.log)"
	fi

	# The region: a PAL machine runs a different frame and shows different scanlines, which the
	# picture shows even where RAM has converged.
	settings_config "$work/settings.$name.pal.json" '{"region": "pal"}'
	if run_frontend "$name.pal" "$work/settings.$name.pal.json" "$frames" "$work/$name.pal.png"; then
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

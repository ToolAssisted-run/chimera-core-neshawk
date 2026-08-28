#!/bin/bash
# The core-level gate for the waterbox port: for every rom given, the sandboxed core must produce
# byte-identical video, audio, lag and memory-domain digests to the SAME core built natively, and
# must survive a whole-machine savestate round-trip around every frame.
#
# The two builds share every line of emulation source and differ only in toolchain and address
# space (glibc/PIE against musl/static/-mcmodel=large inside the sandbox), so a difference here is a
# sandbox or codegen problem, never a translation one. Fidelity to the C# NesHawk is a separate
# question, answered by ../harness and ../test.
#
# It needs only gcc, meson and python3 - no .NET, Mono or X.
#
# Usage:
#   ./run-gate.sh [-n <native build dir>] [-g <guest build dir>] [-f <frames>] [rom...]
# with no roms it uses the free-to-distribute set vendored in roms/.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
frames=600
while getopts "n:g:f:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

roms=("$@")
if [ ${#roms[@]} -eq 0 ]; then
	roms=("$here/roms/sprilo.nes")
fi

[ -x "$nat/run-wbx" ] && [ -x "$nat/run-native" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|videoHash|audioHash|lagFrames|domain\[)'; }
# What a turbo run can be held to: everything except the accumulated per-frame video hash,
# which a run that skipped drawing cannot possibly match.
turboDigests() { grep -E '^(frames|tailVideoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
# SKIP counts as neither: a check that does not apply to this rom (a cart with no save file) is not
# a pass to brag about and not a failure to fix.
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; SKIP) ;; *) failed=$((failed+1)) ;; esac; }

printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

for rom in "${roms[@]}"; do
	name="$(basename "$rom" .nes)"
	if [ ! -f "$rom" ]; then report "$name" SKIP "rom not found"; continue; fi

	if ! "$nat/run-native" "$rom" "$frames" 2>"$work/nat.err" | digests > "$work/nat.txt"; then
		report "$name:equivalence" FAIL "native runner error: $(head -1 "$work/nat.err")"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" 2>"$work/box.err" | digests > "$work/box.txt"; then
		report "$name:equivalence" FAIL "waterbox runner error: $(head -1 "$work/box.err")"; continue
	fi
	if cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name:equivalence" PASS "$frames frames, native == waterboxed"
	else
		report "$name:equivalence" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Round-trip the whole machine through save/load state around every frame: the digests must come
	# out exactly as they do without it.
	if ! "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name:savestate" FAIL "rerecord runner error"; continue
	fi
	if cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$name:savestate" FAIL "$(diff "$work/box.txt" "$work/rr.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Turbo: the same frames with the core's drawing switched off, and switched back on for the last
	# one. The machine, the sound, the lag count and the picture of that last frame must all be what
	# they would have been. A core that got this wrong shows up here as a different final picture
	# even when every byte of RAM still agrees.
	if "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" 2>/dev/null | turboDigests > "$work/norm.txt" &&
	   "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" --turbo 2>/dev/null | turboDigests > "$work/turbo.txt"; then
		if cmp -s "$work/norm.txt" "$work/turbo.txt"; then
			report "$name:turbo" PASS "$frames frames, half of them undrawn, same machine and same pictures"
		else
			report "$name:turbo" FAIL "$(diff "$work/norm.txt" "$work/turbo.txt" | tr '\n' ' ' | head -c 120)"
		fi
	else
		report "$name:turbo" FAIL "turbo runner error"
	fi

	# Save files, when the machine has one: what the core writes out has to be what a fresh machine
	# takes back in. A disk or a battery cart that reloaded a save differently would lose the save
	# silently, which is the one failure a user cannot notice until it is too late.
	if "$nat/run-wbx" "$gst/core.wbx" "$rom" "$frames" --saveram-out "$work/sram.bin" > "$work/sram.txt" 2>&1; then
		sram_bytes="$(grep -o 'saveRamBytes=[0-9]*' "$work/sram.txt" | cut -d= -f2)"
		if [ "${sram_bytes:-0}" -eq 0 ]; then
			report "$name:saveram" SKIP "this machine has nothing to save"
		elif ! "$nat/run-wbx" "$gst/core.wbx" "$rom" 0 --saveram-in "$work/sram.bin" \
			--saveram-out "$work/sram.rt.bin" > /dev/null 2>&1; then
			report "$name:saveram" FAIL "the core refused the save file it just wrote"
		elif cmp -s "$work/sram.bin" "$work/sram.rt.bin"; then
			report "$name:saveram" PASS "$sram_bytes bytes survive a write/read round-trip"
		else
			report "$name:saveram" FAIL "the save file changed across a round-trip"
		fi
	else
		report "$name:saveram" FAIL "save file runner error"
	fi

	# The optional tooling exports the frontend probes for: absence is allowed, but a core that
	# claims a surface must render one.
	if [ -x "$nat/run-tooling" ]; then
		if "$nat/run-tooling" "$gst/core.wbx" "$rom" 120 > "$work/tool.txt" 2>&1; then
			if grep -q "RENDER FAILED" "$work/tool.txt"; then
				report "$name:tooling" FAIL "a declared surface did not render"
			else
				report "$name:tooling" PASS "$(grep -c '^  \[' "$work/tool.txt") tooling entries reported"
			fi
		else
			report "$name:tooling" FAIL "runner error"
		fi
	fi
done

# ---- save data: out, and back in -------------------------------------------
#
# A machine that keeps saves - a cartridge with a battery, or a disk system -
# hands them out through the savedata channel (what Export Save Data writes)
# and takes them back through the savedata slot (what a project mounts). Proved
# with a save the machine could not have written; the battery-flagged sprilo
# the bundle legs already build is the cart.
batt="$work/sprilo.battery.nes"
if [ ! -f "$batt" ]; then
	python3 - "$here/roms/sprilo.nes" "$batt" <<'PYBATT'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[6] |= 0x02          # iNES flags 6, bit 1: battery-backed save RAM
open(sys.argv[2], 'wb').write(bytes(d))
PYBATT
fi
"$nat/run-wbx" "$gst/core.wbx" "$batt" 60 --savedata-out "$work/nh.sav" >/dev/null 2>&1
if [ ! -s "$work/nh.sav" ]; then
	report "savedata:export" FAIL "a battery cart exported nothing"
else
	report "savedata:export" PASS "$(stat -c%s "$work/nh.sav") bytes of battery WRAM left through the channel"
	python3 - "$work/nh.sav" "$work/nh-seed.sav" <<'PYSEED'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[0:16] = b'CHIMERA-SEED-TST'
open(sys.argv[2], 'wb').write(bytes(d))
PYSEED
	"$nat/run-wbx" "$gst/core.wbx" "$batt" 60 --savedata-in "$work/nh-seed.sav" \
		--savedata-out "$work/nh-back.sav" >/dev/null 2>&1
	if ! head -c 16 "$work/nh-back.sav" 2>/dev/null | grep -q "CHIMERA-SEED-TST"; then
		report "savedata:seeded" FAIL "the save the project supplied did not reach the machine"
	else
		report "savedata:seeded" PASS "a save the project supplied reached the machine and returned"
	fi
	if "$nat/run-wbx" "$gst/core.wbx" "$here/roms/sprilo.nes" 10 \
		--savedata-in "$work/nh-seed.sav" >/dev/null 2>&1; then
		report "savedata:refused" FAIL "a machine that keeps no saves accepted save data"
	else
		report "savedata:refused" PASS "a machine that keeps no saves refuses save data"
	fi
fi

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0

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
#   ./run-gate.sh [-o <build dir>] [-f <frames>] [rom...]
# with no roms it uses the free-to-distribute set vendored in roms/.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
out="$here/bin"
frames=600
while getopts "o:f:" opt; do
	case "$opt" in
		o) out="$OPTARG" ;;
		f) frames="$OPTARG" ;;
		*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))

roms=("$@")
if [ ${#roms[@]} -eq 0 ]; then
	roms=("$here/roms/sprilo.nes")
fi

[ -x "$out/run-wbx" ] || { echo "drivers not built: $out/run-wbx (run build-core.sh)" >&2; exit 1; }
[ -x "$out/run-native" ] || { echo "drivers not built: $out/run-native (run build-core.sh)" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|videoHash|audioHash|lagFrames|domain\[)'; }

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

	if ! "$out/run-native" "$rom" "$frames" 2>"$work/nat.err" | digests > "$work/nat.txt"; then
		report "$name:equivalence" FAIL "native runner error: $(head -1 "$work/nat.err")"; continue
	fi
	if ! "$out/run-wbx" "$out/core.wbx" "$rom" "$frames" 2>"$work/box.err" | digests > "$work/box.txt"; then
		report "$name:equivalence" FAIL "waterbox runner error: $(head -1 "$work/box.err")"; continue
	fi
	if cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name:equivalence" PASS "$frames frames, native == waterboxed"
	else
		report "$name:equivalence" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Round-trip the whole machine through save/load state around every frame: the digests must come
	# out exactly as they do without it.
	if ! "$out/run-wbx" "$out/core.wbx" "$rom" "$frames" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name:savestate" FAIL "rerecord runner error"; continue
	fi
	if cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$name:savestate" FAIL "$(diff "$work/box.txt" "$work/rr.txt" | tr '\n' ' ' | head -c 120)"
	fi

	# Save files, when the machine has one: what the core writes out has to be what a fresh machine
	# takes back in. A disk or a battery cart that reloaded a save differently would lose the save
	# silently, which is the one failure a user cannot notice until it is too late.
	if "$out/run-wbx" "$out/core.wbx" "$rom" "$frames" --saveram-out "$work/sram.bin" > "$work/sram.txt" 2>&1; then
		sram_bytes="$(grep -o 'saveRamBytes=[0-9]*' "$work/sram.txt" | cut -d= -f2)"
		if [ "${sram_bytes:-0}" -eq 0 ]; then
			report "$name:saveram" SKIP "this machine has nothing to save"
		elif ! "$out/run-wbx" "$out/core.wbx" "$rom" 0 --saveram-in "$work/sram.bin" \
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
	if [ -x "$out/run-tooling" ]; then
		if "$out/run-tooling" "$out/core.wbx" "$rom" 120 > "$work/tool.txt" 2>&1; then
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

echo ""
echo "$ok ok, $failed failed"
[ "$failed" -gt 0 ] && exit 1
exit 0

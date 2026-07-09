#!/usr/bin/env bash
# ROM-free, .NET-free regression guard for the translated MOS 6502X core.
#
# cpuFuzz seeds a deterministic RAM image and runs the CPU for a fixed number of cycles, emitting a
# per-cycle trace (PC/A/X/Y/P/S/opcode/microcode). Because the translation is deterministic, that
# trace is reproducible; here we diff it against the checked-in golden. Any behavioral change in the
# 6502 (a mistranslated opcode, a flag bug) shows up as a diff and fails the test.
#
# The golden itself was produced by this exact CYCLES/SEED. It has ALSO been validated byte-exact
# against genuine BizHawk NesHawk via harness/CpuFuzz.cs (run manually; needs .NET + BizHawk).
#
# Usage: run-cpufuzz-regression.sh <cpuFuzz-binary> <golden-file>
set -euo pipefail

CPUFUZZ="$1"
GOLDEN="$2"
CYCLES=4000
SEED=0xC0FFEE01

OUT="$(mktemp)"
trap 'rm -f "$OUT"' EXIT

"$CPUFUZZ" "$CYCLES" "$OUT" "$SEED"

if ! diff -q "$GOLDEN" "$OUT" >/dev/null 2>&1; then
  echo "FAIL: cpuFuzz trace diverged from golden ($GOLDEN)."
  echo "----- first differing lines (golden < / actual >) -----"
  diff "$GOLDEN" "$OUT" | head -20
  exit 1
fi

echo "cpufuzz-regression OK: $CYCLES cycles match golden."

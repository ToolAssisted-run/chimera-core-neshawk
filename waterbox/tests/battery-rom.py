#!/usr/bin/env python3
# Copies an iNES rom with the battery bit (flags6 bit 1) set, so the same machine gains the 8KB of
# battery-backed WRAM that makes it have a save file. Lets the save-file path be tested end to end
# over a rom that is free to distribute.
#
# Usage: battery-rom.py <in.nes> <out.nes>
import sys

data = bytearray(open(sys.argv[1], "rb").read())
if data[:4] != b"NES\x1a":
    sys.exit("not an iNES file")
data[6] |= 2
open(sys.argv[2], "wb").write(bytes(data))

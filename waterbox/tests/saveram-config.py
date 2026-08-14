#!/usr/bin/env python3
# Writes a copy of a miniHawk config whose Save RAM paths point at a scratch directory, so a test
# run's save files land somewhere disposable instead of in the user's tree.
#
# Usage: saveram-config.py <source config> <output config> <saveram dir>
import json
import sys

cfg = json.load(open(sys.argv[1]))
for entry in cfg.get("PathEntries", {}).get("Paths", []):
    if entry.get("Type") == "Save RAM":
        entry["Path"] = sys.argv[3]
json.dump(cfg, open(sys.argv[2], "w"), indent=2)

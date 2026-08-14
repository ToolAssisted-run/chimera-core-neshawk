#!/usr/bin/env python3
# Checks that a bundle still names its attachment correctly after a session wrote to it: the file
# is there, and the hash the bundle pins it to is the hash the file actually has. Exits non-zero
# if the pin went stale, which is what a write-back that forgot to re-pin would look like.
#
# Usage: check-bundle.py <bundle> [sha1 the attachment had before the session]
import hashlib
import json
import os
import sys

bundle = json.load(open(sys.argv[1]))
folder = os.path.dirname(os.path.abspath(sys.argv[1]))
for part in [bundle["rom"], *bundle.get("attach", [])]:
    with open(os.path.join(folder, part["file"]), "rb") as f:
        actual = hashlib.sha1(f.read()).hexdigest().upper()
    if part.get("sha1", actual).upper() != actual:
        sys.exit(f"{part['file']}: bundle pins {part['sha1']}, file is {actual}")

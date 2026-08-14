#!/usr/bin/env python3
# Writes a bundle: a catalogue naming a rom and one core's persistent data, both of which have to
# sit beside it. This is what the frontend's "Compose Bundle..." produces, written here by hand so
# the gate can build one without a person.
#
# Usage: compose-bundle.py <out.bundle> <rom> <core name> <id> <data file>
import hashlib
import json
import os
import shutil
import sys

out, rom, core, ident, data = sys.argv[1:6]
folder = os.path.dirname(os.path.abspath(out))


def place(path):
    """Copies a file next to the bundle (a bundle may only name files beside it) and pins it."""
    dest = os.path.join(folder, os.path.basename(path))
    if os.path.abspath(path) != dest:
        shutil.copyfile(path, dest)
    with open(dest, "rb") as f:
        return os.path.basename(dest), hashlib.sha1(f.read()).hexdigest().upper()


rom_file, rom_sha1 = place(rom)
data_file, data_sha1 = place(data)
json.dump({
    "bundle": 1,
    "name": os.path.splitext(os.path.basename(out))[0],
    "rom": {"file": rom_file, "sha1": rom_sha1},
    "attach": [{"core": core, "id": ident, "file": data_file, "sha1": data_sha1}],
}, open(out, "w"), indent=2)

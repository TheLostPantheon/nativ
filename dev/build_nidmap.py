#!/usr/bin/env python3
"""Flatten VitaSDK db/*.yml into a TSV of library, nid, name. usage: build_nidmap.py <db dir> <out.tsv>"""
import sys, yaml, pathlib
rows = {}
for f in sorted(pathlib.Path(sys.argv[1]).rglob("*.yml")):
    d = yaml.safe_load(open(f)) or {}
    for mod in (d.get("modules") or {}).values():
        for lib, ld in (mod.get("libraries") or {}).items():
            for kind in ("functions", "variables"):
                for name, nid in ((ld or {}).get(kind) or {}).items():
                    rows[(lib, int(nid) & 0xFFFFFFFF)] = name   # later firmware dirs win
with open(sys.argv[2], "w") as o:
    for (lib, nid), name in sorted(rows.items()):
        o.write(f"{lib}\t{nid:08X}\t{name}\n")
print(len(rows), "names")

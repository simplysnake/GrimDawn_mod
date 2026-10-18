"""Maps module RVAs to the nearest preceding exported symbol (from notes/exports_<Module>.txt, made with
   tools/pe_dump.py <game folder>/x64/<Module>.dll --exports notes/exports_<Module>.txt; not in git).

Usage: python rva_lookup.py Engine 0x112FA5 0x113342 ...
"""
import bisect
import sys
from pathlib import Path

mod = sys.argv[1]
rows = []
for line in (Path(__file__).parent.parent / "notes" / f"exports_{mod}.txt").read_text(encoding="utf-8").splitlines():
    _, rva, name = line.split("\t")
    rows.append((int(rva, 16), name))
rows.sort()
keys = [r for r, _ in rows]
for a in sys.argv[2:]:
    rva = int(a, 16)
    i = bisect.bisect_right(keys, rva) - 1
    base, name = rows[i] if i >= 0 else (0, "?")
    print(f"{a:>10}  {name}+0x{rva - base:X}")

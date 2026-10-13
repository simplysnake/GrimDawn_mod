"""Lists call sites of an imported function (call [rip+IAT]) in a DLL, with the containing function.

Usage: python find_import_calls.py <dll> <ModuleLabel> <ImportName>
"""
import bisect
import struct
import sys
from pathlib import Path

dll, label, imp = sys.argv[1], sys.argv[2], sys.argv[3]
data = Path(dll).read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
dirs = opt + 112
secs = []
sec = opt + struct.unpack_from("<H", data, pe + 20)[0]
for i in range(nsec):
    name, vsize, va, rsize, rptr, *_ , chars = struct.unpack_from("<8sIIIIIIHHI", data, sec + 40 * i)
    secs.append((name.rstrip(b"\0").decode(), va, max(vsize, rsize), rptr, rsize, chars))
def off(rva):
    for _, va, size, rptr, *_ in secs:
        if va <= rva < va + size:
            return rva - va + rptr
    raise ValueError(hex(rva))
def cstr(rva):
    o = off(rva); return data[o:data.index(b"\0", o)].decode()

# find IAT slot RVA of the import
imp_rva = struct.unpack_from("<I", data, dirs + 8)[0]
slot_rva = None
o = off(imp_rva)
while True:
    oft, _, _, name, ft = struct.unpack_from("<IIIII", data, o)
    if not name: break
    t = off(oft or ft); k = 0
    while True:
        v = struct.unpack_from("<Q", data, t + 8 * k)[0]
        if not v: break
        if not v >> 63 and cstr((v & 0x7FFFFFFF) + 2) == imp:
            slot_rva = ft + 8 * k
        k += 1
    o += 20
print(f"{imp} IAT slot RVA {slot_rva:#x}")

exc_rva, exc_size = struct.unpack_from("<II", data, dirs + 8 * 3)
funcs = sorted(struct.unpack_from("<II", data, off(exc_rva) + 12 * i) for i in range(exc_size // 12))
starts = [b for b, _ in funcs]
names = []
for line in (Path(__file__).parent.parent / "notes" / f"exports_{label}.txt").read_text(encoding="utf-8").splitlines():
    _, rva, n = line.split("\t"); names.append((int(rva, 16), n))
names.sort(); nkeys = [r for r, _ in names]
def nearest(rva):
    i = bisect.bisect_right(nkeys, rva) - 1
    return f"{names[i][1]}+0x{rva - names[i][0]:X}" if i >= 0 else "?"

for name, va, size, rptr, rsize, chars in secs:
    if not chars & 0x20000000:  # executable
        continue
    code = data[rptr:rptr + rsize]
    for i in range(len(code) - 6):
        if code[i] == 0xFF and code[i + 1] == 0x15:
            rel = struct.unpack_from("<i", code, i + 2)[0]
            site = va + i
            if site + 6 + rel == slot_rva:
                fi = bisect.bisect_right(starts, site) - 1
                print(f"call at {site:#x}  in func {funcs[fi][0]:#x}  (~{nearest(site)})")

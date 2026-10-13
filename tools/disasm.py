"""Disassembles the function containing an RVA in a game DLL, using .pdata for function bounds
and the exports dump for call/jump target names.

Needs iced-x86 (pip install iced-x86). Usage:
  python disasm.py "<path to Engine.dll>" Engine 0x1D1298 [--around 0] [--max 400]
"""
import argparse
import bisect
import struct
from pathlib import Path

from iced_x86 import Decoder, Formatter, FormatterSyntax, OpKind

ap = argparse.ArgumentParser()
ap.add_argument("dll")
ap.add_argument("module")
ap.add_argument("rva")
ap.add_argument("--max", type=int, default=400)
a = ap.parse_args()

data = Path(a.dll).read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
nsec = struct.unpack_from("<H", data, pe + 6)[0]
opt = pe + 24
image_base = struct.unpack_from("<Q", data, opt + 24)[0]
dirs = opt + 112
sections = []
sec = opt + struct.unpack_from("<H", data, pe + 20)[0]
for i in range(nsec):
    name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, sec + 40 * i)
    sections.append((va, max(vsize, rsize), rptr))


def off(rva):
    for va, size, rptr in sections:
        if va <= rva < va + size:
            return rva - va + rptr
    raise ValueError(hex(rva))


# Function bounds from the exception directory (.pdata: RUNTIME_FUNCTION begin/end/unwind)
exc_rva, exc_size = struct.unpack_from("<II", data, dirs + 8 * 3)
funcs = [struct.unpack_from("<III", data, off(exc_rva) + 12 * i)[:2] for i in range(exc_size // 12)]
target = int(a.rva, 16)
begin, end = next(((b, e) for b, e in funcs if b <= target < e), (target, target + 0x200))

# Export names
names = []
exp_file = Path(__file__).parent.parent / "notes" / f"exports_{a.module}.txt"
for line in exp_file.read_text(encoding="utf-8").splitlines():
    _, rva, name = line.split("\t")
    names.append((int(rva, 16), name))
names.sort()
keys = [r for r, _ in names]


def label(rva):
    i = bisect.bisect_right(keys, rva) - 1
    if i >= 0 and names[i][0] == rva:
        return names[i][1]
    fb = next((b for b, e in funcs if b <= rva < e), None)
    return f"sub_{rva:X}" if fb == rva else (f"sub_{fb:X}+0x{rva - fb:X}" if fb else f"0x{rva:X}")


print(f"function {begin:#x}-{end:#x} ({label(begin)}), target {target:#x}")
code = data[off(begin):off(begin) + (end - begin)]
dec = Decoder(64, code, ip=image_base + begin)
fmt = Formatter(FormatterSyntax.INTEL)
for n, ins in enumerate(dec):
    if n >= a.max:
        break
    rva = ins.ip - image_base
    text = fmt.format(ins)
    note = ""
    if ins.op0_kind in (OpKind.NEAR_BRANCH64,) and ins.mnemonic in (ins.mnemonic,):
        t = ins.near_branch_target - image_base
        if text.startswith("call"):
            note = "  ; " + label(t)
    if ins.is_ip_rel_memory_operand:
        t = ins.ip_rel_memory_address - image_base
        note += f"  ; [{label(t)}]"
    mark = ">>" if rva <= target < rva + ins.len else "  "
    print(f"{mark}{rva:08X}  {text}{note}")

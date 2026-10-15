"""Names of a module's import slots (IAT RVAs), from a file or a mapped in-memory image.

Usage: python iat_names.py <module file> [--image] [rva ...]   (no RVAs: list all)
"""
import struct
import sys

args = [x for x in sys.argv[1:] if x != "--image"]
image = "--image" in sys.argv
data = open(args[0], "rb").read()
want = {int(x, 16) for x in args[1:]}
pe = struct.unpack_from("<I", data, 0x3C)[0]
opt = pe + 24
nsec = struct.unpack_from("<H", data, pe + 6)[0]
so = opt + struct.unpack_from("<H", data, pe + 20)[0]
secs = [struct.unpack_from("<8sIIII", data, so + i * 40) for i in range(nsec)]


def off(rva):
    if image:
        return rva
    for _, vs, va, rs, ro in secs:
        if va <= rva < va + max(vs, rs):
            return ro + rva - va
    raise ValueError(hex(rva))


imp = struct.unpack_from("<I", data, opt + 112 + 8)[0]
i = off(imp)
while True:
    ilt, _, _, name, iat = struct.unpack_from("<IIIII", data, i)
    i += 20
    if not name:
        break
    dll = data[off(name):].split(b"\0")[0].decode()
    k = 0
    while True:
        e = struct.unpack_from("<Q", data, off(ilt or iat) + k * 8)[0]
        if not e:
            break
        slot = iat + k * 8
        if not want or slot in want:
            nm = f"ord {e & 0xFFFF}" if e >> 63 else data[off(e & 0x7FFFFFFF) + 2:].split(b"\0")[0].decode()
            print(f"{slot:#x}\t{dll}\t{nm}")
        k += 1

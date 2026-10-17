"""Reads Grim Dawn .arc archives (version 3): list and extract files.

Layout: 28-byte header (magic "ARC\\0", version, file count, part count, part table size,
string table size, part table offset); the part table (offset, compressed size, size, 12 bytes
each); the file name table; then one 44-byte entry per file. Each file is one or more parts,
LZ4-block compressed unless the compressed size equals the size.

Usage:
  python gd_arc.py <file.arc> list [filter]
  python gd_arc.py <file.arc> extract <name> <out file>
"""
import struct
import sys

import lz4.block


class Arc:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        magic, version, nfiles, nparts, ptsize, stsize, ptoff = struct.unpack_from("<7I", self.data, 0)
        assert magic == 0x435241 and version == 3, (hex(magic), version)
        self.parts = [struct.unpack_from("<3I", self.data, ptoff + 12 * i) for i in range(nparts)]
        stoff = ptoff + ptsize
        entoff = stoff + stsize
        self.files = {}
        for i in range(nfiles):
            (etype, offset, csize, size, crc, ftime, nfparts, first, namelen, nameoff) = struct.unpack_from(
                "<5IQ4I", self.data, entoff + 44 * i)
            name = self.data[stoff + nameoff:stoff + nameoff + namelen].decode("latin-1").lower().replace("\\", "/")
            self.files[name] = (etype, offset, csize, size, nfparts, first)

    def read(self, name):
        etype, offset, csize, size, nfparts, first = self.files[name.lower()]
        if etype == 1 and csize == size:  # stored
            return self.data[offset:offset + size]
        out = bytearray()
        for p in range(first, first + nfparts):
            poff, pcsize, psize = self.parts[p]
            chunk = self.data[poff:poff + pcsize]
            out += chunk if pcsize == psize else lz4.block.decompress(chunk, uncompressed_size=psize)
        return bytes(out)


if __name__ == "__main__":
    arc = Arc(sys.argv[1])
    if sys.argv[2] == "list":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for n, e in sorted(arc.files.items()):
            if flt in n:
                print(f"{e[3]:>10}  {n}")
    elif sys.argv[2] == "extract":
        data = arc.read(sys.argv[3])
        open(sys.argv[4], "wb").write(data)
        print(f"{len(data)} bytes -> {sys.argv[4]}")

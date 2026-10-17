"""Reads Grim Dawn .arz databases (version 3): list records and dump their fields.

Layout: 24-byte header (unknown u16, version u16, record table offset/size, record count, string
table offset/size). Record table: name string index, type (length-prefixed), data offset (from
byte 24), compressed size, size, file time. String table: count, then length-prefixed strings.
Record data (LZ4 block): entries of type u16 (0 int, 1 float, 2 string, 3 bool), count u16,
key string index u32, then count u32 values.

Usage:
  python gd_arz.py <file.arz> list [filter]
  python gd_arz.py <file.arz> dump <record name>
"""
import struct
import sys

import lz4.block


class Arz:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        _, version, rtoff, rtsize, rcount, stoff, stsize = struct.unpack_from("<2H5I", self.data, 0)
        assert version == 3, version
        # string table
        count = struct.unpack_from("<I", self.data, stoff)[0]
        p = stoff + 4
        self.strings = []
        for _ in range(count):
            n = struct.unpack_from("<I", self.data, p)[0]
            self.strings.append(self.data[p + 4:p + 4 + n].decode("latin-1"))
            p += 4 + n
        # records
        self.records = {}
        p = rtoff
        for _ in range(rcount):
            name_idx, tlen = struct.unpack_from("<2I", self.data, p)
            rtype = self.data[p + 8:p + 8 + tlen].decode("latin-1")
            doff, csize, size = struct.unpack_from("<3I", self.data, p + 8 + tlen)
            p += 8 + tlen + 12 + 8
            self.records[self.strings[name_idx].lower().replace("\\", "/")] = (rtype, doff + 24, csize, size)

    def fields(self, name):
        rtype, off, csize, size = self.records[name.lower()]
        raw = lz4.block.decompress(self.data[off:off + csize], uncompressed_size=size)
        out = {}
        p = 0
        while p + 8 <= len(raw):
            t, n, key = struct.unpack_from("<2HI", raw, p)
            p += 8
            vals = []
            for i in range(n):
                v = raw[p + 4 * i:p + 4 * i + 4]
                if t == 1:
                    vals.append(struct.unpack("<f", v)[0])
                elif t == 2:
                    vals.append(self.strings[struct.unpack("<I", v)[0]])
                else:
                    vals.append(struct.unpack("<i", v)[0])
            p += 4 * n
            out[self.strings[key]] = vals if n != 1 else vals[0]
        return rtype, out


if __name__ == "__main__":
    arz = Arz(sys.argv[1])
    if sys.argv[2] == "list":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for n, r in sorted(arz.records.items()):
            if flt in n:
                print(f"{r[0]:<28} {n}")
    elif sys.argv[2] == "dump":
        rtype, f = arz.fields(sys.argv[3])
        print("type", rtype)
        for k, v in f.items():
            if v not in (0, "", 0.0, []):
                print(f"  {k} = {v}")

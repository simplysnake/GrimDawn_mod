"""Minimal PE32+ export/import dumper (no dependencies).

Usage: python pe_dump.py <dll> [--exports OUT.txt] [--imports]
"""
import struct, sys, argparse


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        assert d[pe:pe + 4] == b"PE\0\0", "not a PE file"
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.magic = struct.unpack_from("<H", d, opt)[0]
        dd = opt + (112 if self.magic == 0x20B else 96)
        self.dirs = [struct.unpack_from("<II", d, dd + 8 * i) for i in range(16)]
        sec = opt + opt_size
        self.sections = []
        for i in range(nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", d, sec + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode(), va, max(vsize, rsize), rptr))

    def off(self, rva):
        for _, va, size, rptr in self.sections:
            if va <= rva < va + size:
                return rva - va + rptr
        raise ValueError(hex(rva))

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return []
        o = self.off(rva)
        (_, _, _, _, _, base, nfunc, nnames, afuncs, anames, aords) = struct.unpack_from("<IIHHIIIIIII", self.d, o)
        funcs = struct.unpack_from(f"<{nfunc}I", self.d, self.off(afuncs)) if nfunc else ()
        names = struct.unpack_from(f"<{nnames}I", self.d, self.off(anames)) if nnames else ()
        ords = struct.unpack_from(f"<{nnames}H", self.d, self.off(aords)) if nnames else ()
        out = [(base + o_, funcs[o_], self.cstr(n)) for n, o_ in zip(names, ords)]
        return sorted(out)

    def imports(self):
        rva, _ = self.dirs[1]
        res = {}
        if not rva:
            return res
        o = self.off(rva)
        ptr = 8 if self.magic == 0x20B else 4
        while True:
            oft, _, _, name, ft = struct.unpack_from("<IIIII", self.d, o)
            if not name:
                break
            dll = self.cstr(name)
            thunk = self.off(oft or ft)
            fns = []
            while True:
                v = struct.unpack_from("<Q" if ptr == 8 else "<I", self.d, thunk)[0]
                if not v:
                    break
                if v >> (63 if ptr == 8 else 31):
                    fns.append(f"#{v & 0xFFFF}")
                else:
                    fns.append(self.cstr((v & 0x7FFFFFFF) + 2))
                thunk += ptr
            res[dll] = fns
            o += 20
        return res


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("--exports")
    ap.add_argument("--imports", action="store_true")
    a = ap.parse_args()
    pe = PE(a.dll)
    if a.exports:
        ex = pe.exports()
        with open(a.exports, "w", encoding="utf-8") as f:
            for ordn, rva, name in ex:
                f.write(f"{ordn}\t{rva:08X}\t{name}\n")
        print(f"{len(ex)} exports -> {a.exports}")
    if a.imports:
        for dll, fns in pe.imports().items():
            print(f"{dll}: {len(fns)}")
            if len(fns) < 40:
                print("    " + ", ".join(fns))

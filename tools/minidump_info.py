"""Reads a Windows minidump: exception, faulting module+offset, and return addresses found on the
crashing thread's stack (module+offset), for lookup with sym_lookup.py.

Usage: python minidump_info.py crash.dmp [--frames 60]
"""
import argparse
import struct


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--frames", type=int, default=60)
    a = ap.parse_args()
    b = open(a.dump, "rb").read()
    assert b[:4] == b"MDMP"
    nstreams, dir_rva = u32(b, 8), u32(b, 12)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", b, dir_rva + 12 * i)
        streams[t] = (size, rva)

    mods = []
    size, rva = streams[4]  # ModuleListStream
    for i in range(u32(b, rva)):
        m = rva + 4 + 108 * i
        base, msize, name_rva = u64(b, m), u32(b, m + 8), u32(b, m + 20)
        name = b[name_rva + 4:name_rva + 4 + u32(b, name_rva)].decode("utf-16-le").split("\\")[-1]
        mods.append((base, base + msize, name))

    def where(addr):
        for lo, hi, n in mods:
            if lo <= addr < hi:
                return f"{n}+0x{addr - lo:X}"
        return None

    size, rva = streams[6]  # ExceptionStream
    tid = u32(b, rva)
    code, flags, _, addr, nparams = struct.unpack_from("<IIQQI", b, rva + 8)
    params = struct.unpack_from("<2Q", b, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from("<II", b, rva + 8 + 152)
    rip, rsp = u64(b, ctx_rva + 0xF8), u64(b, ctx_rva + 0x98)
    print(f"exception 0x{code:08X} at {where(addr) or hex(addr)} (thread {tid}) params {params[0]:#x} {params[1]:#x}")
    print(f"rip {where(rip) or hex(rip)}  rsp {rsp:#x}")
    regs = ["Rax", "Rcx", "Rdx", "Rbx", "Rsp", "Rbp", "Rsi", "Rdi"]
    print("  " + "  ".join(f"{r}={u64(b, ctx_rva + 0x78 + 8 * i):#x}" for i, r in enumerate(regs)))

    # Thread stack memory from ThreadListStream
    size, rva = streams[3]
    for i in range(u32(b, rva)):
        t = rva + 4 + 48 * i
        if u32(b, t) != tid:
            continue
        start, msize, mrva = u64(b, t + 24), u32(b, t + 32), u32(b, t + 36)
        mem = b[mrva:mrva + msize]
        frames = []
        for off in range(max(0, rsp - start), len(mem) - 7, 8):
            w = where(u64(mem, off))
            if w and (not frames or frames[-1] != w):
                frames.append(w)
        print("stack:")
        for f in frames[:a.frames]:
            print("   ", f)


if __name__ == "__main__":
    main()

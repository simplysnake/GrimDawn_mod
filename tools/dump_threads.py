"""Heuristic call stacks for every thread in a minidump (e.g. the freeze watchdog's freeze_*.dmp).

For each thread: its instruction pointer, then every value on its stack that points into a loaded module
(module+offset), top of stack first. Look the offsets up with rva_lookup.py / sym_lookup.py.

Usage: python dump_threads.py freeze.dmp [--depth 40] [--filter Engine,gdvr]
"""
import argparse
import struct


def read_modules(d, streams):
    size, rva = streams[4]  # ModuleListStream
    n = struct.unpack_from("<I", d, rva)[0]
    mods = []
    for i in range(n):
        off = rva + 4 + i * 108
        base, msize = struct.unpack_from("<QI", d, off)
        name_rva = struct.unpack_from("<I", d, off + 20)[0]
        nlen = struct.unpack_from("<I", d, name_rva)[0]
        name = d[name_rva + 4:name_rva + 4 + nlen].decode("utf-16-le").split("\\")[-1]
        mods.append((base, base + msize, name))
    return mods


def where(mods, a):
    for lo, hi, name in mods:
        if lo <= a < hi:
            return f"{name}+0x{a - lo:X}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--depth", type=int, default=40)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()
    d = open(args.dump, "rb").read()
    assert d[:4] == b"MDMP", "not a minidump"
    nstreams, dir_rva = struct.unpack_from("<II", d, 8)
    streams = {}
    for i in range(nstreams):
        typ, size, rva = struct.unpack_from("<III", d, dir_rva + i * 12)
        streams[typ] = (size, rva)
    mods = read_modules(d, streams)
    size, rva = streams[3]  # ThreadListStream
    n = struct.unpack_from("<I", d, rva)[0]
    wanted = [f.strip().lower() for f in args.filter.split(",") if f.strip()]
    for i in range(n):
        off = rva + 4 + i * 48
        tid = struct.unpack_from("<I", d, off)[0]
        stack_start, stack_size, stack_rva = struct.unpack_from("<QII", d, off + 24)
        ctx_size, ctx_rva = struct.unpack_from("<II", d, off + 40)
        rip = struct.unpack_from("<Q", d, ctx_rva + 0xF8)[0] if ctx_size >= 0x100 else 0
        frames = [where(mods, rip) or f"0x{rip:X}"]
        stack = d[stack_rva:stack_rva + stack_size]
        for k in range(0, len(stack) - 7, 8):
            v = struct.unpack_from("<Q", stack, k)[0]
            w = where(mods, v)
            if w:
                frames.append(w)
            if len(frames) >= args.depth:
                break
        text = "\n".join("    " + f for f in frames)
        if wanted and not any(x in text.lower() for x in wanted):
            continue
        print(f"thread {tid}:\n{text}")


if __name__ == "__main__":
    main()

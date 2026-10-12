"""Prints a heuristic call stack for every thread of a (hung) process.

Scans each thread's stack for values that point into loaded modules and resolves them with
dbghelp (PDBs where available, otherwise exports). Read-only: threads are suspended briefly.

Usage: python thread_stacks.py "Grim Dawn.exe" [--depth 4096] [--filter gdvr,Engine]
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import struct

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

PROCESS_ALL = 0x1F0FFF
THREAD_ALL = 0x1F03FF
TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
CONTEXT_FULL = 0x10000B


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


class CONTEXT64(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P1Home", ctypes.c_uint64 * 6), ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("Seg", ctypes.c_uint16 * 6), ("EFlags", wt.DWORD), ("Dr", ctypes.c_uint64 * 6),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R8_15", ctypes.c_uint64 * 8), ("Rip", ctypes.c_uint64), ("Rest", ctypes.c_byte * 976)]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                ("Name", ctypes.c_char * 512)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
dbghelp.SymFromAddr.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == name.lower():
            return pe.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    return None


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            yield te.th32ThreadID
        ok = k32.Thread32Next(snap, ctypes.byref(te))


def modules(hproc):
    arr = (ctypes.c_void_p * 1024)()
    needed = wt.DWORD()
    psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(needed), 3)
    mods = []
    for i in range(needed.value // 8):
        base = arr[i]
        name = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(hproc, ctypes.c_void_p(base), name, 260)
        info = (ctypes.c_uint64 * 3)()
        psapi.GetModuleInformation(hproc, ctypes.c_void_p(base), info, ctypes.sizeof(info))
        mods.append((base, base + (info[1] & 0xFFFFFFFF), name.value))
    return mods


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--depth", type=int, default=8192)
    ap.add_argument("--filter", default="")
    ap.add_argument("--max-frames", type=int, default=40)
    a = ap.parse_args()
    pid = find_pid(a.exe)
    hproc = k32.OpenProcess(PROCESS_ALL, False, pid)
    dbghelp.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES off; deferred loads
    dbghelp.SymInitializeW(hproc, None, True)
    mods = modules(hproc)
    sym = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 512 + 1, MaxNameLen=511)

    def resolve(addr):
        for lo, hi, name in mods:
            if lo <= addr < hi:
                disp = ctypes.c_uint64()
                if dbghelp.SymFromAddr(hproc, addr, ctypes.byref(disp), ctypes.byref(sym)):
                    return f"{name}!{sym.Name.decode(errors='replace')}+0x{disp.value:X}"
                return f"{name}+0x{addr - lo:X}"
        return None

    wanted = [f.lower() for f in a.filter.split(",") if f]
    for tid in threads(pid):
        ht = k32.OpenThread(THREAD_ALL, False, tid)
        k32.SuspendThread(ht)
        ctx = CONTEXT64(ContextFlags=CONTEXT_FULL)
        buf = ctypes.create_string_buffer(ctypes.sizeof(CONTEXT64) + 16)
        aligned = (ctypes.addressof(buf) + 15) & ~15
        ctypes.memmove(aligned, ctypes.addressof(ctx), ctypes.sizeof(ctx))
        k32.GetThreadContext(ht, ctypes.c_void_p(aligned))
        ctx = CONTEXT64.from_address(aligned)
        rip, rsp = ctx.Rip, ctx.Rsp
        stack = ctypes.create_string_buffer(a.depth)
        read = ctypes.c_size_t()
        k32.ReadProcessMemory(hproc, ctypes.c_void_p(rsp), stack, a.depth, ctypes.byref(read))
        k32.ResumeThread(ht)
        k32.CloseHandle(ht)
        frames = [resolve(rip) or f"0x{rip:X}"]
        for (v,) in struct.iter_unpack("<Q", stack.raw[: read.value - read.value % 8]):
            r = resolve(v)
            if r and r != frames[-1]:
                frames.append(r)
        text = "\n    ".join(frames[:a.max_frames])
        if wanted and not any(w in text.lower() for w in wanted):
            continue
        print(f"thread {tid}:\n    {text}\n")


if __name__ == "__main__":
    main()

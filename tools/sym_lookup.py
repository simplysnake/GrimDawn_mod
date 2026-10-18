import ctypes, ctypes.wintypes as wt, sys, pathlib
BIN = str(pathlib.Path(__file__).resolve().parent.parent / "bin").encode()
d = ctypes.WinDLL("dbghelp", use_last_error=True)
class SI(ctypes.Structure):
    _fields_=[("SizeOfStruct",wt.ULONG),("TypeIndex",wt.ULONG),("Reserved",ctypes.c_uint64*2),("Index",wt.ULONG),("Size",wt.ULONG),("ModBase",ctypes.c_uint64),("Flags",wt.ULONG),("Value",ctypes.c_uint64),("Address",ctypes.c_uint64),("Register",wt.ULONG),("Scope",wt.ULONG),("Tag",wt.ULONG),("NameLen",wt.ULONG),("MaxNameLen",wt.ULONG),("Name",ctypes.c_char*512)]
class LINE(ctypes.Structure):
    _fields_=[("SizeOfStruct",wt.DWORD),("Key",ctypes.c_void_p),("LineNumber",wt.DWORD),("FileName",ctypes.c_char_p),("Address",ctypes.c_uint64)]
h = wt.HANDLE(0x1234)
d.SymSetOptions(0x2|0x10)
print("init", d.SymInitialize(h, BIN, False))
d.SymLoadModuleEx.restype = ctypes.c_uint64
d.SymLoadModuleEx.argtypes=[wt.HANDLE, wt.HANDLE, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
base = d.SymLoadModuleEx(h, None, BIN + b"/gdvr.dll", None, 0x180000000, 0x400000, None, 0)
print("base", hex(base), ctypes.get_last_error())
d.SymFromAddr.argtypes=[wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SI)]
d.SymGetLineFromAddr64.argtypes=[wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(wt.DWORD), ctypes.POINTER(LINE)]
for off in [int(x,16) for x in sys.argv[1:]]:
    s=SI(SizeOfStruct=ctypes.sizeof(SI)-512+1, MaxNameLen=511); disp=ctypes.c_uint64()
    name = s.Name.decode() if d.SymFromAddr(h, 0x180000000+off, ctypes.byref(disp), ctypes.byref(s)) else f"? err {ctypes.get_last_error()}"
    ln=LINE(SizeOfStruct=ctypes.sizeof(LINE)); dd=wt.DWORD()
    where = f"{ln.FileName.decode().replace(chr(92),'/').split('/')[-1]}:{ln.LineNumber}" if d.SymGetLineFromAddr64(h, 0x180000000+off, ctypes.byref(dd), ctypes.byref(ln)) else ""
    print(f"+0x{off:X}  {name}+0x{disp.value:X}  {where}")

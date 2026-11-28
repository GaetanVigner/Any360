"""Prints every thread's stack of a process (dbghelp StackWalk64, x64). usage: python stacks.py <pid> [depth]"""
import ctypes, ctypes.wintypes as wt, sys, struct

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)
pid = int(sys.argv[1]); depth = int(sys.argv[2]) if len(sys.argv) > 2 else 20

PROCESS_ALL = 0x1F0FFF
THREAD_ALL = 0x1F03FF
k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
hp = k32.OpenProcess(PROCESS_ALL, False, pid)
dbg.SymSetOptions(0x2 | 0x10 | 0x4)  # UNDNAME | LOAD_LINES | DEFERRED_LOADS
dbg.SymInitialize(hp, None, True)

class SNAP(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
snap = k32.CreateToolhelp32Snapshot(4, 0)
te = SNAP(); te.dwSize = ctypes.sizeof(SNAP)
tids = []
ok = k32.Thread32First(snap, ctypes.byref(te))
while ok:
    if te.th32OwnerProcessID == pid: tids.append(te.th32ThreadID)
    ok = k32.Thread32Next(snap, ctypes.byref(te))

CTX_SIZE = 1232
class ADDR(ctypes.Structure):
    _fields_ = [("Offset", ctypes.c_uint64), ("Segment", wt.WORD), ("Mode", ctypes.c_int)]
class FRAME(ctypes.Structure):
    _fields_ = [("AddrPC", ADDR), ("AddrReturn", ADDR), ("AddrFrame", ADDR), ("AddrStack", ADDR), ("AddrBStore", ADDR),
                ("FuncTableEntry", ctypes.c_void_p), ("Params", ctypes.c_uint64 * 4), ("Far", wt.BOOL), ("Virtual", wt.BOOL),
                ("Reserved", ctypes.c_uint64 * 3), ("KdHelp", ctypes.c_byte * 112)]
class SYM(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2), ("Index", wt.ULONG),
                ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG), ("Value", ctypes.c_uint64),
                ("Address", ctypes.c_uint64), ("Register", wt.ULONG), ("Scope", wt.ULONG), ("Tag", wt.ULONG),
                ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG), ("Name", ctypes.c_char * 512)]
class LINE(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.DWORD), ("Key", ctypes.c_void_p), ("LineNumber", wt.DWORD), ("FileName", ctypes.c_char_p),
                ("Address", ctypes.c_uint64)]
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
FTA = ctypes.WINFUNCTYPE(ctypes.c_void_p, wt.HANDLE, ctypes.c_uint64)(("SymFunctionTableAccess64", dbg))
GMB = ctypes.WINFUNCTYPE(ctypes.c_uint64, wt.HANDLE, ctypes.c_uint64)(("SymGetModuleBase64", dbg))
dbg.StackWalk64.argtypes = [wt.DWORD, wt.HANDLE, wt.HANDLE, ctypes.POINTER(FRAME), ctypes.c_void_p, ctypes.c_void_p,
                            ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

def name(addr):
    s = SYM(); s.SizeOfStruct = 88; s.MaxNameLen = 500
    disp = ctypes.c_uint64()
    out = hex(addr)
    if dbg.SymFromAddr(hp, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(s)):
        out = s.Name.decode(errors="replace")[:110]
    ln = LINE(); ln.SizeOfStruct = ctypes.sizeof(LINE); d32 = wt.DWORD()
    if dbg.SymGetLineFromAddr64(hp, ctypes.c_uint64(addr), ctypes.byref(d32), ctypes.byref(ln)):
        out += f"  [{ln.FileName.decode(errors='replace').split(chr(92))[-1]}:{ln.LineNumber}]"
    return out

for tid in tids:
    ht = k32.OpenThread(THREAD_ALL, False, tid)
    k32.SuspendThread(ht)
    raw = (ctypes.c_byte * (CTX_SIZE + 16))()
    base = (ctypes.addressof(raw) + 15) & ~15
    ctx = (ctypes.c_byte * CTX_SIZE).from_address(base)
    struct.pack_into("<I", ctx, 0x30, 0x10000B)  # ContextFlags = CONTEXT_FULL (AMD64)
    if not k32.GetThreadContext(ht, ctypes.c_void_p(base)):
        print(f"thread {tid}: no context"); k32.ResumeThread(ht); continue
    rip, = struct.unpack_from("<Q", ctx, 0xF8); rsp, = struct.unpack_from("<Q", ctx, 0x98); rbp, = struct.unpack_from("<Q", ctx, 0xA0)
    f = FRAME(); f.AddrPC.Offset = rip; f.AddrPC.Mode = 3; f.AddrStack.Offset = rsp; f.AddrStack.Mode = 3
    f.AddrFrame.Offset = rbp; f.AddrFrame.Mode = 3
    print(f"--- thread {tid}")
    for i in range(depth):
        if not dbg.StackWalk64(0x8664, hp, ht, ctypes.byref(f), ctypes.c_void_p(base), None, FTA, GMB, None): break
        if f.AddrPC.Offset == 0: break
        print("   ", name(f.AddrPC.Offset))
    k32.ResumeThread(ht)

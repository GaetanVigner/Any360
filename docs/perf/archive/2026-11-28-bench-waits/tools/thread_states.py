"""Samples a process's thread states (NtQuerySystemInformation class 5) and summarises per thread:
share of samples Running / Ready / Waiting by wait reason, CPU time, context switches.
Also whole-process: samples by (#running, #ready) to tell 'idle because waiting' from 'runnable, no core'.
usage: python thread_states.py <exe name> <seconds> [interval_ms]"""
import ctypes, ctypes.wintypes as wt, sys, time, struct, collections

ntdll = ctypes.WinDLL("ntdll")
k32 = ctypes.WinDLL("kernel32")
NtQSI = ntdll.NtQuerySystemInformation
NtQSI.argtypes = [ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong)]
STATES = {0: "Init", 1: "Ready", 2: "Running", 3: "Standby", 4: "Term", 5: "Wait", 6: "Transition", 7: "DeferredReady"}
REASONS = {0: "Executive", 4: "DelayExecution", 5: "Suspended", 6: "UserRequest", 7: "WrExecutive", 11: "WrDelayExecution",
           13: "WrUserRequest", 15: "WrQueue", 16: "WrLpcReceive", 21: "WrKeyedEvent", 26: "WrKernel", 27: "WrResource",
           28: "WrPushLock", 29: "WrMutex", 32: "WrPreempted", 33: "WrYieldExecution", 36: "WrRundown",
           37: "WrAlertByThreadId", 38: "WrDeferredPreempt"}

buf = ctypes.create_string_buffer(8 << 20)

def snapshot(name):
    need = ctypes.c_ulong()
    st = NtQSI(5, buf, len(buf), ctypes.byref(need))
    if st != 0:
        raise OSError(hex(st & 0xFFFFFFFF))
    raw = buf.raw
    off = 0
    while True:
        nxt, nthreads = struct.unpack_from("<II", raw, off)
        ln, = struct.unpack_from("<H", raw, off + 56)
        ptr, = struct.unpack_from("<Q", raw, off + 64)
        img = ctypes.wstring_at(ptr, ln // 2) if ptr else ""
        pid, = struct.unpack_from("<Q", raw, off + 80)
        if (name.isdigit() and pid == int(name)) or img.lower() == name.lower():
            threads = []
            for i in range(nthreads):
                t = off + 256 + 80 * i
                kt, ut = struct.unpack_from("<qq", raw, t)
                start, = struct.unpack_from("<Q", raw, t + 32)
                tid, = struct.unpack_from("<Q", raw, t + 48)
                prio, = struct.unpack_from("<i", raw, t + 56)
                cs, state, reason = struct.unpack_from("<III", raw, t + 64)
                threads.append((tid, kt + ut, cs, state, reason, prio, start))
            return pid, threads
        if not nxt:
            return None, []
        off += nxt

def main():
    name, seconds = sys.argv[1], float(sys.argv[2])
    interval = float(sys.argv[3]) / 1000 if len(sys.argv) > 3 else 0.001
    k32.timeBeginPeriod = ctypes.WinDLL("winmm").timeBeginPeriod
    ctypes.WinDLL("winmm").timeBeginPeriod(1)
    while snapshot(name)[0] is None:
        time.sleep(0.05)
    t0 = time.perf_counter()
    first = {}
    last = {}
    per = collections.defaultdict(collections.Counter)
    combo = collections.Counter()
    n = 0
    while time.perf_counter() - t0 < seconds:
        pid, threads = snapshot(name)
        if pid is None:
            break
        running = ready = 0
        for tid, cpu, cs, state, reason, prio, start in threads:
            first.setdefault(tid, (cpu, cs))
            last[tid] = (cpu, cs, prio)
            key = STATES.get(state, str(state))
            if state == 5:
                key = "W:" + REASONS.get(reason, str(reason))
            per[tid][key] += 1
            if state == 2:
                running += 1
            elif state in (1, 3, 7):
                ready += 1
        combo[(running, ready)] += 1
        n += 1
        time.sleep(interval)
    wall = time.perf_counter() - t0
    print(f"{n} samples over {wall:.1f} s")
    print("process samples by (running, ready):")
    for k, v in sorted(combo.items()):
        print(f"  running {k[0]} ready {k[1]}: {100*v/n:.1f}%")
    rows = []
    for tid, c in per.items():
        cpu = (last[tid][0] - first[tid][0]) / 1e7
        cs = last[tid][1] - first[tid][1]
        rows.append((cpu, tid, c, cs, last[tid][2]))
    rows.sort(reverse=True)
    for cpu, tid, c, cs, prio in rows:
        if cpu < 0.01 * wall and c.get("Running", 0) == 0:
            continue
        tot = sum(c.values())
        parts = ", ".join(f"{k} {100*v/tot:.0f}%" for k, v in c.most_common(5))
        print(f"tid {tid:6d} prio {prio:2d}: cpu {100*cpu/wall:5.1f}% of a core, {cs/wall:8.0f} cs/s | {parts}")

main()

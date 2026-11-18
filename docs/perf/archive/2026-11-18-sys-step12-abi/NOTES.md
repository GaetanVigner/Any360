# Step 12: the original against Any360 through the PowerPC ABI

`sys_abi_bench` (new) times step 12's 39 calls that do not block, each as a title calls it: the reference runtime's
`__imp__<name>` (the original: ReXGlue, which stubs all of them) and Any360's adapter, on the same registers and
guest memory, hashes and ciphers on 256 bytes. Measured on 2026-11-18 with `sys_abi_bench 9` three times on cores 0-7
(another session was measuring on cores 8-15), RelWithDebInfo, clang, the owner's desktop. Raw output:
[run 1](sys_abi_bench_run1.txt), [run 2](sys_abi_bench_run2.txt), [run 3](sys_abi_bench_run3.txt).

- **The original costs 70-115 ns a call and does nothing** (a stub that returns at once, through the runtime's
  logging check). Any360 answers each call for real.
- **Faster than the original** (Any360 3-50x): every information call (1.7-35 ns), the Win32 forms (`GetLastError`,
  `GetCurrentThreadId`, `QueryPerformanceFrequency` about 3 ns, `lstrlenW` of 64 characters 9 ns, `GetTickCount`,
  `GetSystemTimeAsFileTime`, `GetModuleHandleA` about 20 ns), and the state setups `XeCryptMd5Init`,
  `XeCryptHmacShaInit`, `XeCryptDesEcb` (one block).
- **Slower than the original only where the original skips the work**: the hashes and ciphers over 256 bytes
  (`XeCryptMd5` 440 ns, `XeCryptHmacMd5` 690 ns, `XeCryptDesCbc` 2.5 µs for 32 blocks, `XeCryptDesKey` 560 ns): the
  stub returns without computing anything. For that work the comparison is with what the host already offers for it,
  in [2026-11-18-sys-step12-opt](../2026-11-18-sys-step12-opt/): MD5 1.0-1.7x CNG, SHA-256 1.0-2.7x, HMAC-SHA-1
  2.6-4.1x, DES 2.3-3.4x.
- **Host memory**: Any360 0 B and 0 allocations a call (7.0 M timed calls a run). The original's heap is the reference
  runtime DLL's own, which the allocation meter (operator new of the benchmark) does not see: not measured.
- Left out: `Sleep`, `SleepEx`, the waits, `CreateThread`, `ResumeThread`, `ExitThread`, which block or end a thread;
  their cost is their kernel call's (measured with it) plus the Win32 part [sys_step12_bench](../2026-11-18-sys-step12/)
  times (1-5 ns).

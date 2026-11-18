# Step 12: the system calls Xenia implements and the reference stubs (first two batches)

27 calls of `any360::sys` (docs/SYS_STUBS.md, docs/SYS_BINDING.md "Step 12"): 13 cryptography calls (`crypt.h`) and
14 Win32 forms of xam.xex (`win32.h`). Measured on 2026-11-18 with `sys_step12_bench 9` (fastest of 9 runs, RelWithDebInfo,
clang, the owner's desktop: NVIDIA machine, Windows 11). Raw output: [sys_step12_bench.txt](sys_step12_bench.txt).

- **Original**: none to time. The reference runtime stubs every one of them (it returns at once and does nothing), and
  no title calls them yet, so there is no in-game check or measure either: they wait for a title.
- **Host memory**: 0 B and 0 allocations a call for every one, over 43.9 M timed calls. The guest state each uses is the
  console's own (MD5 0x54 bytes, HMAC-SHA-1 0xB0, DES 0x80), in the title's memory.
- **Against what the host offers for the same work** (Windows' CNG, the C library's `wcslen`):
  - MD5 and HMAC-MD5: 0.94-1.36x CNG (faster on small inputs, within 6% on large ones).
  - SHA-256: 0.15-0.54x CNG, and HMAC-SHA-1 (built on the SHA-1 of `XeCryptSha`): 0.50-0.63x. CNG uses the processor's
    SHA instructions (SHA-NI); Any360's SHA-1 and SHA-256 are plain C++.
  - DES CBC: 0.24x CNG. Any360's DES permutes bit by bit (the triple DES it shares the code with was written for
    exactness against the reference, not speed).
  - `lstrlenW`: 1.8x `wcslen` on an empty string, 0.06-0.9x on 8-256 characters. (Said here at first: `wcslen`
    cannot read a guest's big-endian strings. Wrong: a zero 16-bit unit is zero in either byte order, so it can, and
    the optimization pass, [2026-11-18-sys-step12-opt](../2026-11-18-sys-step12-opt/), runs `lstrlenW` on it.)
- **The Win32 part over the kernel calls** (what Sleep, waits, CreateThread, ResumeThread add): 1-5 ns a call.
  `GetTickCount` 18 ns, `GetSystemTimeAsFileTime` 19 ns (the host clocks), `GetModuleHandleA` 25 ns.

Left then for an optimization pass (each kept only when this benchmark confirms it and the sys_object_tests vectors
still pass): SHA-1 / SHA-256 with SHA-NI where the processor has it (the Steam Deck's Zen 2 does) with the C++ path as
fallback, a table-driven DES, and a wider `lstrlenW` scan. Done the same day: [2026-11-18-sys-step12-opt](../2026-11-18-sys-step12-opt/).
